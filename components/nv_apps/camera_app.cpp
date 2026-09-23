// camera_app — full-screen camera for the 2MP MIPI-CSI sensor (OV02C10).
// Live viewfinder (PPA-downscaled from 1920x1080) steered by nv_camera's software 3A (auto exposure
// + white balance), photo/video modes, exposure compensation, tap-to-meter, rule-of-thirds grid,
// self-timer, capture flash + shutter sound, REC timer, last-shot thumbnail (opens the Gallery),
// folder picker, free-space and exposure readouts. Photos -> JPEG; videos -> MJPEG AVI or MP4.
#include "apps_internal.h"

#include "nv_app.h"
#include "nv_ui_kit.h"
#include "nv_icons.h"
#include "nv_i18n.h"
#include "nv_theme.h"
#include "nv_fonts.h"
#include "nv_camera.h"
#include "nv_config.h"
#include "nv_mem_attr.h"   // NV_PSRAM_BSS
#include "nv_notify.h"     // nv_toast
#include "nv_ui.h"         // fullscreen plane, close, open the Gallery
#include "nv_audio.h"      // shutter + countdown tones
#include "nv_time.h"       // dated file names, clock
#include "nv_sd.h"         // free space
#include "nv_bgwork.h"     // free-space query off the UI thread

#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <sys/stat.h>
#include <dirent.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace {

// ---- geometry ----
// Full screen: top bar, viewfinder and bottom bar beside a controls rail (landscape), or stacked
// above a controls block (portrait). The viewfinder is the largest exact k/16 step of the sensor
// frame that fits, because the PPA scales in 1/16 steps (nv_camera_preview_size): 840x472 on the
// 1024x600 panel, 600x337 in portrait. Any other size is refused or leaves rows unwritten.
constexpr int kBar         = 64;    // top / bottom bar height
constexpr int kRail        = 184;   // landscape controls rail width
constexpr int kPortraitCtl = 240;   // minimum controls block height in portrait
int s_pv_w = 0, s_pv_h = 0;         // viewfinder (k/16 of the sensor frame)
int s_th_w = 0, s_th_h = 0;         // thumbnail (1/16)
bool s_land = true;

// Camera chrome stays dark whatever the OS theme: a viewfinder is judged against black.
inline lv_color_t c_ctl()    { return lv_color_hex(0x26262B); }
inline lv_color_t c_ctl_on() { return lv_color_hex(0x3A3A42); }
inline lv_color_t c_dim()    { return lv_color_hex(0x9C9CA6); }
inline lv_color_t c_accent() { return lv_color_hex(0xFFD60A); }
inline lv_color_t c_rec()    { return lv_color_hex(0xE5484D); }

lv_obj_t   *s_root = nullptr;
lv_obj_t   *s_vf = nullptr;          // viewfinder container (tap = meter here)
lv_obj_t   *s_canvas = nullptr;
uint8_t    *s_buf = nullptr;         // preview RGB565
lv_timer_t *s_timer = nullptr;       // preview refresh

lv_obj_t   *s_grid[4] = {};
lv_obj_t   *s_reticle = nullptr;     // spot-metering square
lv_timer_t *s_ret_tmr = nullptr;
lv_obj_t   *s_flash = nullptr;       // white capture flash
lv_obj_t   *s_count = nullptr;       // self-timer countdown disc
lv_obj_t   *s_count_lbl = nullptr;
lv_timer_t *s_count_tmr = nullptr;
int         s_count_left = 0;
lv_obj_t   *s_status = nullptr;      // transient pill at the bottom of the viewfinder
lv_timer_t *s_status_tmr = nullptr;
lv_obj_t   *s_rec_badge = nullptr;   // "• REC 00:00" while recording
lv_obj_t   *s_rec_dot = nullptr;
lv_obj_t   *s_rec_lbl = nullptr;
lv_timer_t *s_rec_tmr = nullptr;

lv_obj_t   *s_folder_lbl = nullptr;
lv_obj_t   *s_ev_lbl = nullptr;
lv_obj_t   *s_grid_btn = nullptr;
lv_obj_t   *s_grid_lbl = nullptr;
lv_obj_t   *s_timer_btn = nullptr;
lv_obj_t   *s_timer_ring = nullptr;
lv_obj_t   *s_timer_hand = nullptr;
lv_obj_t   *s_timer_lbl = nullptr;

lv_obj_t   *s_info_lbl = nullptr;    // resolution / format
lv_obj_t   *s_seg = nullptr;         // PHOTO | VIDEO
lv_obj_t   *s_seg_btn[2] = {};
lv_obj_t   *s_seg_lbl[2] = {};
lv_obj_t   *s_free_lbl = nullptr;

lv_obj_t   *s_clock_lbl = nullptr;
lv_obj_t   *s_exp_lbl = nullptr;
lv_timer_t *s_clock_tmr = nullptr;
lv_obj_t   *s_fmt_btn = nullptr;
lv_obj_t   *s_fmt_lbl = nullptr;
lv_obj_t   *s_shutter_core = nullptr;
lv_obj_t   *s_thumb = nullptr;
lv_obj_t   *s_thumb_ph = nullptr;    // placeholder icon until the first shot
uint8_t    *s_thumb_buf = nullptr;

lv_obj_t   *s_picker = nullptr;
lv_obj_t   *s_picker_ta = nullptr;

bool     s_video_mode = false;
bool     s_rec_mp4 = false;          // video container: false = MJPEG/AVI (plays on-device), true = MP4
bool     s_grid_on = false;
int      s_self_timer = 0;           // 0 / 3 / 10 s
uint32_t s_gen = 0;                  // bumped on teardown: stale background results are dropped
NV_PSRAM_BSS char s_dir[96];

constexpr int kMaxDirs = 40;
NV_PSRAM_BSS char s_dirs[kMaxDirs][64];   // folder picker table (2.5 KB): LVGL thread only
int s_ndirs = 0;

// Horizontal + vertical padding in one call.
inline void chip_pad(lv_obj_t *o, int h, int v) {
    lv_obj_set_style_pad_left(o, h, 0);   lv_obj_set_style_pad_right(o, h, 0);
    lv_obj_set_style_pad_top(o, v, 0);    lv_obj_set_style_pad_bottom(o, v, 0);
}
// Bare container: no theme styles, no scrolling, not clickable (taps fall through to the parent).
lv_obj_t *box(lv_obj_t *parent) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}
lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, lv_color_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, "");
    return l;
}
// Round or pill control on the dark chrome.
lv_obj_t *ctl(lv_obj_t *parent, int w, int h, lv_event_cb_t cb, void *user = nullptr) {
    lv_obj_t *b = box(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, c_ctl(), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, c_ctl_on(), LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(b, 6);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    return b;
}
lv_obj_t *ctl_icon(lv_obj_t *parent, int d, const char *sym, lv_event_cb_t cb) {
    lv_obj_t *b = ctl(parent, d, d, cb);
    lv_obj_t *l = text(b, &nv_font_20, lv_color_white());
    lv_label_set_text(l, sym);
    lv_obj_center(l);
    return b;
}

// -------- helpers --------
const char *dir_name() {
    const char *p = strrchr(s_dir, '/');
    return (p && p[1]) ? p + 1 : s_dir;
}
void refresh_folder_lbl() {
    if (s_folder_lbl) lv_label_set_text_fmt(s_folder_lbl, LV_SYMBOL_DIRECTORY "  %s", dir_name());
}

void status_clear(lv_timer_t *) {
    if (s_status) lv_obj_add_flag(s_status, LV_OBJ_FLAG_HIDDEN);
    s_status_tmr = nullptr;
}
void show_status(const char *msg, bool ok) {
    if (!s_status) return;
    lv_label_set_text(s_status, msg);
    lv_obj_set_style_text_color(s_status, ok ? lv_color_white() : c_rec(), 0);
    lv_obj_remove_flag(s_status, LV_OBJ_FLAG_HIDDEN);
    if (s_status_tmr) lv_timer_delete(s_status_tmr);
    s_status_tmr = lv_timer_create(status_clear, 2200, nullptr);
    lv_timer_set_repeat_count(s_status_tmr, 1);
}

// IMG_20260923_223901.jpg: sortable and unique across reboots. The old boot-relative millisecond
// stamp restarted at every boot, so a new shot could silently overwrite an older one.
bool make_path(char *out, size_t n, const char *prefix, const char *ext) {
    struct tm tm = {};
    nv_time_now(&tm);
    char stem[32];
    if (tm.tm_year + 1900 >= 2024) strftime(stem, sizeof stem, "%Y%m%d_%H%M%S", &tm);
    else snprintf(stem, sizeof stem, "%lu", (unsigned long)(esp_timer_get_time() / 1000));
    struct stat st;
    for (int i = 1; i <= 99; i++) {
        if (i == 1) snprintf(out, n, "%s/%s_%s.%s", s_dir, prefix, stem, ext);
        else        snprintf(out, n, "%s/%s_%s_%d.%s", s_dir, prefix, stem, i, ext);
        if (stat(out, &st) != 0) return true;
    }
    return false;
}

// Free space: FATFS may walk the whole FAT on its first query, so it runs on the background worker
// and the label is filled under the LVGL lock (dropped if the app closed meanwhile).
void free_job(void *arg) {
    const uint32_t gen = (uint32_t)(uintptr_t)arg;
    uint64_t fb = 0;
    const bool ok = nv_sd_info(nullptr, &fb);
    if (!lvgl_port_lock(1000)) return;
    if (gen == s_gen && s_free_lbl) {
        char sz[24], line[64];
        if (fb >= (1ull << 30)) {
            const unsigned t = (unsigned)(fb * 10 / (1ull << 30));
            snprintf(sz, sizeof sz, "%u.%u GB", t / 10, t % 10);
        } else {
            snprintf(sz, sizeof sz, "%u MB", (unsigned)(fb >> 20));
        }
        snprintf(line, sizeof line, nv_tr(NV_STR_CAM_FREE), sz);
        lv_label_set_text_fmt(s_free_lbl, LV_SYMBOL_SD_CARD "  %s", ok ? line : nv_tr(NV_STR_CAM_NO_SD));
    }
    lvgl_port_unlock();
}
void refresh_free() { nv_bgwork_submit(free_job, (void *)(uintptr_t)s_gen); }

// "No SD card" only when there really is none: the generic SAVE_FAILED string says exactly that,
// and it was shown for every failed capture — including recordings refused for lack of PSRAM.
const char *capture_error() {
    return nv_tr(nv_sd_is_mounted() ? NV_STR_CAM_CAPTURE_FAILED : NV_STR_CAM_NO_SD);
}

void update_thumb() {
    if (!s_thumb || !s_thumb_buf || !nv_camera_render(s_thumb_buf, s_th_w, s_th_h)) return;
    if (s_thumb_ph) lv_obj_add_flag(s_thumb_ph, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(s_thumb);
}

// -------- viewfinder --------
void preview_tick(lv_timer_t *) {
    if (!s_canvas || !s_buf) return;
    if (!nv_camera_render(s_buf, s_pv_w, s_pv_h)) return;
    nv_camera_auto_update(s_buf, s_pv_w, s_pv_h);   // AE/AWB meter the frame just shown
    lv_obj_invalidate(s_canvas);
}

void ret_dim(lv_timer_t *) {
    if (s_reticle) lv_obj_set_style_border_opa(s_reticle, LV_OPA_40, 0);
    s_ret_tmr = nullptr;
}
void vf_tap_cb(lv_event_t *) {
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    lv_area_t a;
    lv_obj_get_coords(s_vf, &a);
    const int x = LV_CLAMP(0, p.x - a.x1, s_pv_w - 1), y = LV_CLAMP(0, p.y - a.y1, s_pv_h - 1);
    const bool first = lv_obj_has_flag(s_reticle, LV_OBJ_FLAG_HIDDEN);
    nv_camera_set_meter_point(x * 1000 / s_pv_w, y * 1000 / s_pv_h);
    const int r = lv_obj_get_width(s_reticle) / 2;
    lv_obj_set_pos(s_reticle, LV_CLAMP(0, x - r, s_pv_w - 2 * r), LV_CLAMP(0, y - r, s_pv_h - 2 * r));
    lv_obj_set_style_border_opa(s_reticle, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_reticle, LV_OBJ_FLAG_HIDDEN);
    if (s_ret_tmr) lv_timer_delete(s_ret_tmr);
    s_ret_tmr = lv_timer_create(ret_dim, 1500, nullptr);
    lv_timer_set_repeat_count(s_ret_tmr, 1);
    if (first) show_status(nv_tr(NV_STR_CAM_METER_SPOT), true);
}
void vf_hold_cb(lv_event_t *) {
    nv_camera_set_meter_point(-1, -1);
    if (s_ret_tmr) { lv_timer_delete(s_ret_tmr); s_ret_tmr = nullptr; }
    lv_obj_add_flag(s_reticle, LV_OBJ_FLAG_HIDDEN);
    show_status(nv_tr(NV_STR_CAM_METER_AUTO), true);
}

void flash_exec(void *o, int32_t v) { lv_obj_set_style_bg_opa((lv_obj_t *)o, (lv_opa_t)v, 0); }
void flash_done(lv_anim_t *a) { lv_obj_add_flag((lv_obj_t *)a->var, LV_OBJ_FLAG_HIDDEN); }
void flash() {
    if (!s_flash) return;
    lv_obj_remove_flag(s_flash, LV_OBJ_FLAG_HIDDEN);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_flash);
    lv_anim_set_values(&a, 230, 0);
    lv_anim_set_duration(&a, 320);
    lv_anim_set_exec_cb(&a, flash_exec);
    lv_anim_set_completed_cb(&a, flash_done);
    lv_anim_start(&a);
}
void shutter_sound() {
    nv_audio_tone(2600, 7);    // two quick ticks read as a mechanical shutter
    nv_audio_tone(1250, 18);
}

// -------- capture --------
void do_photo() {
    mkdir(s_dir, 0777);
    char path[160];
    shutter_sound();
    const bool ok = make_path(path, sizeof path, "IMG", "jpg") && nv_camera_save_jpeg(path);
    if (!ok) { show_status(capture_error(), false); return; }
    flash();
    update_thumb();
    refresh_free();
    char msg[96];
    snprintf(msg, sizeof msg, nv_tr(NV_STR_CAM_SAVED_IN), dir_name());
    show_status(msg, true);
}

void rec_tick(lv_timer_t *) {
    if (!s_rec_lbl) return;
    const uint32_t s = nv_camera_video_secs();
    lv_label_set_text_fmt(s_rec_lbl, "REC  %02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
    if (s_rec_dot)   // blink
        lv_obj_set_style_bg_opa(s_rec_dot, lv_obj_get_style_bg_opa(s_rec_dot, LV_PART_MAIN) ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
}
void set_recording_ui(bool on) {
    if (s_rec_badge) {
        if (on) lv_obj_remove_flag(s_rec_badge, LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_add_flag(s_rec_badge, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_seg) {   // no mode switching mid-recording
        if (on) lv_obj_add_flag(s_seg, LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_remove_flag(s_seg, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_fmt_btn) {
        if (on || !s_video_mode) lv_obj_add_flag(s_fmt_btn, LV_OBJ_FLAG_HIDDEN);
        else                     lv_obj_remove_flag(s_fmt_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_shutter_core) {   // red disc -> red rounded square while recording
        lv_obj_set_size(s_shutter_core, on ? 36 : 66, on ? 36 : 66);
        lv_obj_set_style_radius(s_shutter_core, on ? 8 : LV_RADIUS_CIRCLE, 0);
        lv_obj_center(s_shutter_core);
    }
    if (on) {
        rec_tick(nullptr);
        if (!s_rec_tmr) s_rec_tmr = lv_timer_create(rec_tick, 500, nullptr);
    } else if (s_rec_tmr) {
        lv_timer_delete(s_rec_tmr); s_rec_tmr = nullptr;
    }
}
void video_start() {
    mkdir(s_dir, 0777);
    char path[160];
    if (make_path(path, sizeof path, "VID", s_rec_mp4 ? "mp4" : "avi") && nv_camera_video_start(path)) {
        nv_audio_tone(1500, 60);
        set_recording_ui(true);
    } else {
        show_status(capture_error(), false);
    }
}
void video_stop() {
    nv_camera_video_stop();   // finalizes the file (index + header back-patch)
    nv_audio_tone(1000, 60);
    set_recording_ui(false);
    update_thumb();
    refresh_free();
    show_status(nv_tr(NV_STR_CAM_VIDEO_SAVED), true);
}
void fire_capture() {
    if (s_video_mode) video_start();
    else do_photo();
}

void count_cancel() {
    if (s_count_tmr) { lv_timer_delete(s_count_tmr); s_count_tmr = nullptr; }
    s_count_left = 0;
    if (s_count) lv_obj_add_flag(s_count, LV_OBJ_FLAG_HIDDEN);
}
void count_tick(lv_timer_t *) {
    if (--s_count_left <= 0) {
        count_cancel();
        fire_capture();
        return;
    }
    lv_label_set_text_fmt(s_count_lbl, "%d", s_count_left);
    nv_audio_tone(s_count_left <= 2 ? 1800 : 1400, 50);
}
void shutter_cb(lv_event_t *) {
    if (s_count_tmr) { count_cancel(); return; }                     // second press cancels
    if (nv_camera_video_recording()) { video_stop(); return; }
    if (s_self_timer <= 0) { fire_capture(); return; }
    s_count_left = s_self_timer;
    lv_label_set_text_fmt(s_count_lbl, "%d", s_count_left);
    lv_obj_remove_flag(s_count, LV_OBJ_FLAG_HIDDEN);
    nv_audio_tone(1400, 50);
    s_count_tmr = lv_timer_create(count_tick, 1000, nullptr);
}

// -------- modes & toggles --------
void apply_mode() {
    for (int i = 0; i < 2; i++) {
        const bool sel = (i == 1) == s_video_mode;
        if (s_seg_btn[i]) lv_obj_set_style_bg_opa(s_seg_btn[i], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        if (s_seg_lbl[i]) lv_obj_set_style_text_color(s_seg_lbl[i], sel ? c_accent() : c_dim(), 0);
    }
    if (s_shutter_core) lv_obj_set_style_bg_color(s_shutter_core, s_video_mode ? c_rec() : lv_color_white(), 0);
    if (s_fmt_lbl) lv_label_set_text(s_fmt_lbl, s_rec_mp4 ? "MP4" : "AVI");
    if (s_fmt_btn) {
        if (s_video_mode) lv_obj_remove_flag(s_fmt_btn, LV_OBJ_FLAG_HIDDEN);
        else              lv_obj_add_flag(s_fmt_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_info_lbl) {
        int w = 0, h = 0;
        if (s_video_mode) nv_camera_video_dims(&w, &h);
        else              nv_camera_dims(&w, &h);
        lv_label_set_text_fmt(s_info_lbl, "%d\xC3\x97%d  \xC2\xB7  %s", w, h,
                              !s_video_mode ? "JPEG" : s_rec_mp4 ? "MP4" : "MJPEG");
    }
}
void seg_cb(lv_event_t *e) {
    const bool video = lv_event_get_user_data(e) != nullptr;
    if (nv_camera_video_recording() || video == s_video_mode) return;
    count_cancel();
    s_video_mode = video;
    nv_config_set_bool("cam_video", s_video_mode);
    apply_mode();
}
void fmt_cb(lv_event_t *) {
    if (nv_camera_video_recording()) return;   // never switch container mid-recording
    s_rec_mp4 = !s_rec_mp4;
    nv_config_set_bool("cam_mp4", s_rec_mp4);
    apply_mode();
}

void ev_refresh() {
    if (!s_ev_lbl) return;
    const int e = nv_camera_get_ev();
    if (e == 0) lv_label_set_text(s_ev_lbl, "EV 0");
    else lv_label_set_text_fmt(s_ev_lbl, "EV %c%d.%d", e > 0 ? '+' : '-', abs(e) / 2, (abs(e) % 2) * 5);
    lv_obj_set_style_text_color(s_ev_lbl, e ? c_accent() : lv_color_white(), 0);
}
void ev_cb(lv_event_t *e) {
    const int d = (int)(intptr_t)lv_event_get_user_data(e);
    nv_camera_set_ev(d ? nv_camera_get_ev() + d : 0);   // the value itself resets to 0
    ev_refresh();
}

void grid_apply() {
    for (lv_obj_t *g : s_grid) {
        if (!g) continue;
        if (s_grid_on) lv_obj_remove_flag(g, LV_OBJ_FLAG_HIDDEN);
        else           lv_obj_add_flag(g, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_grid_btn) lv_obj_set_style_bg_color(s_grid_btn, s_grid_on ? c_ctl_on() : c_ctl(), 0);
    if (s_grid_lbl) lv_obj_set_style_text_color(s_grid_lbl, s_grid_on ? c_accent() : lv_color_white(), 0);
}
void grid_cb(lv_event_t *) {
    s_grid_on = !s_grid_on;
    nv_config_set_bool("cam_grid", s_grid_on);
    grid_apply();
}

void timer_apply() {
    const bool on = s_self_timer > 0;
    const lv_color_t ink = on ? c_accent() : lv_color_white();
    if (s_timer_ring) lv_obj_set_style_border_color(s_timer_ring, ink, 0);
    if (s_timer_hand) lv_obj_set_style_bg_color(s_timer_hand, ink, 0);
    if (s_timer_btn) lv_obj_set_style_bg_color(s_timer_btn, on ? c_ctl_on() : c_ctl(), 0);
    if (s_timer_lbl) {
        lv_label_set_text_fmt(s_timer_lbl, "%ds", s_self_timer);
        lv_obj_set_style_text_color(s_timer_lbl, on ? c_accent() : c_dim(), 0);
    }
}
void timer_cb(lv_event_t *) {
    count_cancel();
    s_self_timer = s_self_timer == 0 ? 3 : s_self_timer == 3 ? 10 : 0;
    nv_config_set_int("cam_timer", s_self_timer);
    timer_apply();
}

void clock_tick(lv_timer_t *) {
    if (s_clock_lbl) {
        char b[16];
        nv_time_format(b, sizeof b, nv_time_is_24h() ? "%H:%M" : "%I:%M");
        lv_label_set_text(s_clock_lbl, b);
    }
    if (s_exp_lbl) {
        uint32_t us = 0, g = 0;
        nv_camera_exposure_info(&us, &g);
        if (us) lv_label_set_text_fmt(s_exp_lbl, "1/%u s  \xC2\xB7  ISO %u",
                                      (unsigned)((1000000u + us / 2) / us), (unsigned)g);
    }
}

void back_cb(lv_event_t *) {
    // Deferred: closing deletes this button while its own event is still running.
    lv_async_call([](void *) { nv_ui_close_app(); }, nullptr);
}
void thumb_cb(lv_event_t *) {
    if (nv_camera_video_recording()) return;   // leaving would cut the recording short
    lv_async_call([](void *) { nv_ui_open_app_id("gallery"); }, nullptr);
}

// -------- folder picker --------
void picker_close() {
    if (s_picker) { lv_obj_delete(s_picker); s_picker = nullptr; s_picker_ta = nullptr; }
}
void pick_select(const char *full) {
    strncpy(s_dir, full, sizeof s_dir - 1);
    s_dir[sizeof s_dir - 1] = 0;
    nv_config_set_str("cam_dir", s_dir);
    refresh_folder_lbl();
    lv_async_call([](void *) { picker_close(); }, nullptr);   // the tapped row lives in the picker
}
void pick_row_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_ndirs) return;
    char full[96];
    snprintf(full, sizeof full, "/sdcard/%s", s_dirs[idx]);
    pick_select(full);
}
void newfolder_cb(lv_event_t *) {
    if (!s_picker_ta) return;
    const char *name = lv_textarea_get_text(s_picker_ta);
    if (!name || !name[0]) return;
    // One plain folder name: no separators, no "..", no leading dot. mkdir failed on those but the
    // path was persisted anyway and every later save died with SAVE_FAILED.
    if (strchr(name, '/') || strchr(name, '\\') || strstr(name, "..") || name[0] == '.' || strlen(name) > 60) {
        nv_toast(NV_NOTE_WARN, nv_tr(NV_STR_CAM_BAD_FOLDER));
        return;
    }
    char full[96];
    snprintf(full, sizeof full, "/sdcard/%s", name);
    struct stat st;
    if (mkdir(full, 0777) != 0 && !(stat(full, &st) == 0 && S_ISDIR(st.st_mode))) {
        nv_toast(NV_NOTE_ERROR, nv_tr(NV_STR_CAM_FOLDER_FAIL));
        return;
    }
    pick_select(full);
}
void scan_dirs() {
    s_ndirs = 0;
    DIR *d = opendir("/sdcard");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && s_ndirs < kMaxDirs) {
        if (e->d_name[0] == '.') continue;
        if (strlen(e->d_name) > 63) continue;   // won't fit our name slot; skip
        char p[288];
        snprintf(p, sizeof p, "/sdcard/%s", e->d_name);
        struct stat st;
        if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
            strncpy(s_dirs[s_ndirs], e->d_name, 63);
            s_dirs[s_ndirs][63] = 0;
            s_ndirs++;
        }
    }
    closedir(d);
}
void open_picker(lv_event_t *) {
    if (s_picker || nv_camera_video_recording()) return;
    const NvTheme *th = nv_theme_get();
    scan_dirs();

    // scrim
    s_picker = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_picker);
    lv_obj_set_size(s_picker, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_picker, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_picker, LV_OPA_70, 0);
    lv_obj_center(s_picker);
    lv_obj_remove_flag(s_picker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_picker, [](lv_event_t *e) {   // tap outside the panel closes
        if (lv_event_get_target(e) == s_picker) lv_async_call([](void *) { picker_close(); }, nullptr);
    }, LV_EVENT_CLICKED, nullptr);

    // panel
    lv_obj_t *panel = lv_obj_create(s_picker);
    lv_obj_set_size(panel, 460, lv_pct(86));
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, th->surface, 0);
    lv_obj_set_style_radius(panel, NV_RAD_MD, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 14, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(panel, 8, 0);

    // header row: title + close
    lv_obj_t *hdr = lv_obj_create(panel);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *title = lv_label_create(hdr);
    lv_label_set_text_fmt(title, LV_SYMBOL_DIRECTORY "  %s", nv_tr(NV_STR_CAM_FOLDER));
    lv_obj_set_style_text_color(title, th->text_strong, 0);
    lv_obj_t *close = nv_kit_button(hdr, LV_SYMBOL_CLOSE, false);
    lv_obj_add_event_cb(close, [](lv_event_t *) { lv_async_call([](void *) { picker_close(); }, nullptr); },
                        LV_EVENT_CLICKED, nullptr);

    // new-folder row: text field + create button
    lv_obj_t *nf = lv_obj_create(panel);
    lv_obj_remove_style_all(nf);
    lv_obj_set_size(nf, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(nf, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nf, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(nf, 8, 0);
    s_picker_ta = nv_kit_textarea(nf, nv_tr(NV_STR_CAM_NEW_FOLDER), true);
    lv_obj_set_flex_grow(s_picker_ta, 1);
    lv_obj_t *add = nv_kit_button(nf, LV_SYMBOL_PLUS, true);
    lv_obj_add_event_cb(add, newfolder_cb, LV_EVENT_CLICKED, nullptr);

    // scrollable list of existing folders
    lv_obj_t *list = lv_obj_create(panel);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_set_style_pad_all(list, 2, 0);

    const char *cur = dir_name();
    for (int i = 0; i < s_ndirs; i++) {
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), 48);
        lv_obj_set_style_bg_color(row, th->surface2, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, NV_RAD_SM, 0);
        lv_obj_set_style_pad_left(row, 12, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 10, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, pick_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *ic = lv_label_create(row);
        bool sel = strcmp(s_dirs[i], cur) == 0;
        lv_label_set_text(ic, sel ? LV_SYMBOL_OK : LV_SYMBOL_DIRECTORY);
        lv_obj_set_style_text_color(ic, sel ? th->success : th->accent, 0);
        lv_obj_t *nm = lv_label_create(row);
        lv_label_set_text(nm, s_dirs[i]);
        lv_obj_set_style_text_color(nm, th->text, 0);
    }
    if (s_ndirs == 0) {
        lv_obj_t *empty = lv_label_create(list);
        lv_label_set_text(empty, "-");
        lv_obj_set_style_text_color(empty, th->text_dim, 0);
    }
}

// -------- teardown --------
void page_deleted(lv_event_t *) {
    s_gen++;   // a free-space result still in flight must not touch the widgets below
    lv_timer_t **tmrs[] = { &s_timer, &s_status_tmr, &s_rec_tmr, &s_count_tmr, &s_clock_tmr, &s_ret_tmr };
    for (lv_timer_t **t : tmrs)
        if (*t) { lv_timer_delete(*t); *t = nullptr; }
    s_count_left = 0;
    nv_camera_stop();                       // stops recording + streaming before buffers go away
    if (s_buf)       { heap_caps_free(s_buf); s_buf = nullptr; }
    if (s_thumb_buf) { heap_caps_free(s_thumb_buf); s_thumb_buf = nullptr; }
    s_root = s_vf = s_canvas = s_reticle = s_flash = s_count = s_count_lbl = s_status = nullptr;
    s_rec_badge = s_rec_dot = s_rec_lbl = s_folder_lbl = s_ev_lbl = s_grid_btn = s_grid_lbl = nullptr;
    s_timer_btn = s_timer_ring = s_timer_hand = s_timer_lbl = s_info_lbl = s_seg = s_free_lbl = nullptr;
    s_clock_lbl = s_exp_lbl = s_fmt_btn = s_fmt_lbl = s_shutter_core = s_thumb = s_thumb_ph = nullptr;
    s_picker = s_picker_ta = nullptr;
    for (int i = 0; i < 4; i++) s_grid[i] = nullptr;
    for (int i = 0; i < 2; i++) s_seg_btn[i] = s_seg_lbl[i] = nullptr;
    nv_ui_app_fullscreen(false);
}

// -------- build --------
void build_top_bar(lv_obj_t *bar) {
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(bar, 12, 0);
    lv_obj_set_style_pad_column(bar, 10, 0);

    ctl_icon(bar, 44, LV_SYMBOL_LEFT, back_cb);

    lv_obj_t *folder = ctl(bar, LV_SIZE_CONTENT, 44, open_picker);
    chip_pad(folder, 16, 0);
    s_folder_lbl = text(folder, &nv_font_20, lv_color_white());
    lv_label_set_long_mode(s_folder_lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_max_width(s_folder_lbl, s_land ? 200 : 110, 0);
    lv_obj_center(s_folder_lbl);
    refresh_folder_lbl();

    lv_obj_t *spacer = box(bar);
    lv_obj_set_height(spacer, 1);
    lv_obj_set_flex_grow(spacer, 1);

    // exposure compensation: [ - | EV +0.5 | + ]; tapping the value resets it
    lv_obj_t *ev = box(bar);
    lv_obj_set_size(ev, LV_SIZE_CONTENT, 44);
    lv_obj_set_style_radius(ev, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ev, c_ctl(), 0);
    lv_obj_set_style_bg_opa(ev, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(ev, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ev, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *minus = ctl_icon(ev, 44, LV_SYMBOL_MINUS, nullptr);
    lv_obj_add_event_cb(minus, ev_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    lv_obj_t *mid = box(ev);
    lv_obj_set_size(mid, 88, 44);
    lv_obj_add_flag(mid, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(mid, ev_cb, LV_EVENT_CLICKED, (void *)(intptr_t)0);
    s_ev_lbl = text(mid, &nv_font_20, lv_color_white());
    lv_obj_center(s_ev_lbl);
    lv_obj_t *plus = ctl_icon(ev, 44, LV_SYMBOL_PLUS, nullptr);
    lv_obj_add_event_cb(plus, ev_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    ev_refresh();

    // rule-of-thirds grid
    s_grid_btn = ctl(bar, 44, 44, grid_cb);
    s_grid_lbl = text(s_grid_btn, &nv_font_28, lv_color_white());
    lv_label_set_text(s_grid_lbl, "#");
    lv_obj_center(s_grid_lbl);

    // self-timer: a small drawn stopwatch + "0s/3s/10s"
    s_timer_btn = ctl(bar, LV_SIZE_CONTENT, 44, timer_cb);
    chip_pad(s_timer_btn, 12, 0);
    lv_obj_set_flex_flow(s_timer_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_timer_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_timer_btn, 8, 0);
    s_timer_ring = box(s_timer_btn);
    lv_obj_set_size(s_timer_ring, 20, 20);
    lv_obj_set_style_radius(s_timer_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_timer_ring, 2, 0);
    s_timer_hand = box(s_timer_ring);
    lv_obj_set_size(s_timer_hand, 2, 7);
    lv_obj_set_style_bg_opa(s_timer_hand, LV_OPA_COVER, 0);
    lv_obj_align(s_timer_hand, LV_ALIGN_CENTER, 0, -3);
    s_timer_lbl = text(s_timer_btn, &nv_font_20, lv_color_white());
    timer_apply();
}

void build_bottom_bar(lv_obj_t *bar) {
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(bar, 16, 0);

    // three cells, the outer two growing equally, keep the mode selector exactly centred
    lv_obj_t *left = box(bar);
    lv_obj_set_size(left, 1, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(left, 1);
    s_info_lbl = text(left, &nv_font_14, c_dim());

    s_seg = box(bar);
    lv_obj_set_size(s_seg, LV_SIZE_CONTENT, 44);
    lv_obj_set_style_radius(s_seg, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_seg, c_ctl(), 0);
    lv_obj_set_style_bg_opa(s_seg, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_seg, 4, 0);
    lv_obj_set_flex_flow(s_seg, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_seg, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    static const nv_str_id_t kSeg[2] = { NV_STR_CAM_PHOTO, NV_STR_CAM_VIDEO };
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = box(s_seg);
        lv_obj_set_size(b, LV_SIZE_CONTENT, lv_pct(100));
        chip_pad(b, 18, 0);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(b, c_ctl_on(), 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(b, 6);
        lv_obj_add_event_cb(b, seg_cb, LV_EVENT_CLICKED, i ? (void *)1 : nullptr);
        s_seg_btn[i] = b;
        s_seg_lbl[i] = text(b, &nv_font_14, c_dim());
        lv_label_set_text(s_seg_lbl[i], nv_tr(kSeg[i]));
        lv_obj_center(s_seg_lbl[i]);
    }

    lv_obj_t *right = box(bar);
    lv_obj_set_size(right, 1, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(right, 1);
    s_free_lbl = text(right, &nv_font_14, c_dim());
    lv_obj_align(s_free_lbl, LV_ALIGN_RIGHT_MID, 0, 0);
    refresh_free();
}

void build_viewfinder(lv_obj_t *vf) {
    s_canvas = lv_canvas_create(vf);
    lv_canvas_set_buffer(s_canvas, s_buf, s_pv_w, s_pv_h, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_canvas, 0, 0);
    lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);

    // rule-of-thirds grid: four hairlines
    for (int i = 0; i < 4; i++) {
        lv_obj_t *g = box(vf);
        lv_obj_set_style_bg_color(g, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(g, LV_OPA_40, 0);
        if (i < 2) { lv_obj_set_size(g, 1, s_pv_h); lv_obj_set_pos(g, s_pv_w * (i + 1) / 3, 0); }
        else       { lv_obj_set_size(g, s_pv_w, 1); lv_obj_set_pos(g, 0, s_pv_h * (i - 1) / 3); }
        s_grid[i] = g;
    }

    s_reticle = box(vf);
    lv_obj_set_size(s_reticle, 76, 76);
    lv_obj_set_style_radius(s_reticle, 8, 0);
    lv_obj_set_style_border_width(s_reticle, 2, 0);
    lv_obj_set_style_border_color(s_reticle, c_accent(), 0);
    lv_obj_add_flag(s_reticle, LV_OBJ_FLAG_HIDDEN);

    s_rec_badge = box(vf);
    lv_obj_set_size(s_rec_badge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_rec_badge, c_rec(), 0);
    lv_obj_set_style_bg_opa(s_rec_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_rec_badge, LV_RADIUS_CIRCLE, 0);
    chip_pad(s_rec_badge, 14, 7);
    lv_obj_set_flex_flow(s_rec_badge, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_rec_badge, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_rec_badge, 8, 0);
    lv_obj_align(s_rec_badge, LV_ALIGN_TOP_MID, 0, 12);
    s_rec_dot = box(s_rec_badge);
    lv_obj_set_size(s_rec_dot, 10, 10);
    lv_obj_set_style_radius(s_rec_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_rec_dot, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_rec_dot, LV_OPA_COVER, 0);
    s_rec_lbl = text(s_rec_badge, &nv_font_20, lv_color_white());
    lv_obj_add_flag(s_rec_badge, LV_OBJ_FLAG_HIDDEN);

    s_count = box(vf);
    lv_obj_set_size(s_count, 110, 110);
    lv_obj_set_style_radius(s_count, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_count, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_count, LV_OPA_50, 0);
    lv_obj_set_style_border_width(s_count, 3, 0);
    lv_obj_set_style_border_color(s_count, c_accent(), 0);
    lv_obj_center(s_count);
    s_count_lbl = text(s_count, &nv_font_28, lv_color_white());
    lv_obj_center(s_count_lbl);
    lv_obj_add_flag(s_count, LV_OBJ_FLAG_HIDDEN);

    s_status = text(vf, &nv_font_14, lv_color_white());
    lv_obj_set_style_bg_color(s_status, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_status, LV_OPA_60, 0);
    lv_obj_set_style_radius(s_status, LV_RADIUS_CIRCLE, 0);
    chip_pad(s_status, 16, 8);
    lv_obj_align(s_status, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_add_flag(s_status, LV_OBJ_FLAG_HIDDEN);

    s_flash = box(vf);
    lv_obj_set_size(s_flash, s_pv_w, s_pv_h);
    lv_obj_set_style_bg_color(s_flash, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_flash, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_flash, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(vf, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(vf, vf_tap_cb, LV_EVENT_SHORT_CLICKED, nullptr);
    lv_obj_add_event_cb(vf, vf_hold_cb, LV_EVENT_LONG_PRESSED, nullptr);
}

// Controls: clock + exposure readout, container toggle (video), shutter, last shot. In landscape
// they stack down the rail; in portrait the readout sits above a thumb | shutter | format row.
void build_controls(lv_obj_t *rail, bool land) {
    lv_obj_set_flex_flow(rail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(rail, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *info = box(rail);
    lv_obj_set_size(info, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(info, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(info, 2, 0);
    s_clock_lbl = text(info, &nv_font_20, lv_color_white());
    s_exp_lbl = text(info, &nv_font_14, c_dim());

    lv_obj_t *row = rail;
    if (!land) {
        row = box(rail);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    }

    // last shot (tap -> Gallery); built first in portrait so it sits on the left
    auto build_thumb = [&]() {
        lv_obj_t *f = ctl(row, s_th_w + 6, s_th_h + 6, thumb_cb);
        lv_obj_set_style_radius(f, 10, 0);
        lv_obj_set_style_border_width(f, 2, 0);
        lv_obj_set_style_border_color(f, c_ctl_on(), 0);
        lv_obj_set_style_border_color(f, c_accent(), LV_STATE_PRESSED);
        s_thumb = lv_canvas_create(f);
        lv_canvas_set_buffer(s_thumb, s_thumb_buf, s_th_w, s_th_h, LV_COLOR_FORMAT_RGB565);
        lv_canvas_fill_bg(s_thumb, c_ctl(), LV_OPA_COVER);
        lv_obj_center(s_thumb);
        lv_obj_remove_flag(s_thumb, LV_OBJ_FLAG_CLICKABLE);
        s_thumb_ph = text(f, &nv_font_28, c_dim());
        lv_label_set_text(s_thumb_ph, LV_SYMBOL_IMAGE);
        lv_obj_center(s_thumb_ph);
    };
    // video container toggle; a fixed slot so the shutter never moves when it hides
    auto build_fmt = [&]() {
        lv_obj_t *slot = box(row);
        lv_obj_set_size(slot, land ? 56 : s_th_w + 6, 56);
        s_fmt_btn = ctl(slot, 56, 56, fmt_cb);
        lv_obj_center(s_fmt_btn);
        s_fmt_lbl = text(s_fmt_btn, &nv_font_14, c_accent());
        lv_obj_center(s_fmt_lbl);
    };

    if (!land) build_thumb();
    else build_fmt();

    lv_obj_t *shutter = box(row);
    lv_obj_set_size(shutter, 86, 86);
    lv_obj_set_style_radius(shutter, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(shutter, 4, 0);
    lv_obj_set_style_border_color(shutter, lv_color_white(), 0);
    lv_obj_set_style_border_opa(shutter, LV_OPA_50, LV_STATE_PRESSED);   // press feedback
    lv_obj_add_flag(shutter, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(shutter, 10);
    lv_obj_add_event_cb(shutter, shutter_cb, LV_EVENT_CLICKED, nullptr);
    s_shutter_core = box(shutter);
    lv_obj_set_size(s_shutter_core, 66, 66);
    lv_obj_set_style_radius(s_shutter_core, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_shutter_core, LV_OPA_COVER, 0);
    lv_obj_center(s_shutter_core);

    if (land) build_thumb();
    else build_fmt();
}

void no_camera(lv_obj_t *content) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *msg = lv_label_create(content);
    lv_label_set_text(msg, nv_tr(NV_STR_NO_CAMERA));
    lv_obj_set_style_text_color(msg, th->text_dim, 0);
    lv_obj_center(msg);
}

void cam_build(lv_obj_t *content) {
    nv_config_get_str("cam_dir", "/sdcard/DCIM", s_dir, sizeof s_dir);
    s_video_mode = nv_config_get_bool("cam_video", false);
    s_rec_mp4    = nv_config_get_bool("cam_mp4", false);
    s_grid_on    = nv_config_get_bool("cam_grid", false);
    s_self_timer = nv_config_get_int("cam_timer", 0);
    if (s_self_timer != 3 && s_self_timer != 10) s_self_timer = 0;

    // Without a sensor the app stays windowed (header + Back) with a plain message.
    if (!nv_camera_start()) { no_camera(content); return; }
    nv_camera_set_ev(0);                   // compensation and spot metering are per session
    nv_camera_set_meter_point(-1, -1);

    nv_ui_app_fullscreen(true);
    lv_obj_set_style_bg_color(content, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);
    lv_obj_update_layout(content);
    const int W = lv_obj_get_content_width(content), H = lv_obj_get_content_height(content);
    const bool land = W >= H;
    s_land = land;

    lv_obj_t *root = box(content);
    s_root = root;
    lv_obj_set_size(root, W, H);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_add_event_cb(root, page_deleted, LV_EVENT_DELETE, nullptr);

    // Largest exact viewfinder beside (landscape) or above (portrait) the controls.
    const int avail_w = land ? W - kRail : W;
    const int avail_h = H - 2 * kBar - (land ? 0 : kPortraitCtl);
    int k = 16;
    for (; k > 1; k--) {
        int w = 0, h = 0;
        nv_camera_preview_size(k, &w, &h);
        if (w <= avail_w && h <= avail_h) break;
    }
    nv_camera_preview_size(k, &s_pv_w, &s_pv_h);
    nv_camera_preview_size(1, &s_th_w, &s_th_h);

    s_buf = (uint8_t *)heap_caps_aligned_calloc(64, 1, NV_CAMERA_RENDER_BYTES(s_pv_w, s_pv_h),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_thumb_buf = (uint8_t *)heap_caps_aligned_calloc(64, 1, NV_CAMERA_RENDER_BYTES(s_th_w, s_th_h),
                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buf || !s_thumb_buf) {
        nv_camera_stop();
        nv_ui_app_fullscreen(false);
        lv_obj_delete(root);   // page_deleted frees whichever buffer did allocate
        no_camera(content);
        return;
    }

    // Landscape: [top bar / viewfinder / bottom bar] | rail. Portrait: top bar, viewfinder, bottom
    // bar, then the controls block filling the rest.
    const int col_w = land ? W - kRail : W;
    const int vf_x = (col_w - s_pv_w) / 2;
    const int vf_y = land ? kBar + (avail_h - s_pv_h) / 2 : kBar;

    lv_obj_t *top = box(root);
    lv_obj_set_size(top, col_w, kBar);
    lv_obj_set_pos(top, 0, 0);
    build_top_bar(top);

    s_vf = box(root);
    lv_obj_set_size(s_vf, s_pv_w, s_pv_h);
    lv_obj_set_pos(s_vf, vf_x, vf_y);
    build_viewfinder(s_vf);

    lv_obj_t *bottom = box(root);
    lv_obj_set_size(bottom, col_w, kBar);
    lv_obj_set_pos(bottom, 0, land ? H - kBar : vf_y + s_pv_h);
    build_bottom_bar(bottom);

    lv_obj_t *rail = box(root);
    if (land) { lv_obj_set_size(rail, W - col_w, H);  lv_obj_set_pos(rail, col_w, 0); }
    else      { const int y = vf_y + s_pv_h + kBar;
                lv_obj_set_size(rail, W, H - y);       lv_obj_set_pos(rail, 0, y); }
    lv_obj_set_style_pad_ver(rail, 16, 0);
    build_controls(rail, land);

    apply_mode();
    grid_apply();
    clock_tick(nullptr);
    s_clock_tmr = lv_timer_create(clock_tick, 1000, nullptr);
    s_timer = lv_timer_create(preview_tick, 66, nullptr);   // ~15 fps viewfinder refresh
}

// 20 MB is the HONEST budget: 3 x 4.05 MB frame buffers (nv_camera.c CAM_NBUF) + the CSI driver's
// own 4.05 MB backup frame + the 0.8 MB preview canvas + the larger of the photo scratch (1.4 MB)
// and the recorder's buffers (~3.3 MB). The old 18 MB left out the backup frame and the capture
// scratch: the gate passed, then every photo save failed for lack of PSRAM. The broker reclaims the
// ANIMA/WASM caches to meet this figure, so declaring less only turns a clean refusal into a broken open.
const NvApp kCameraApp = {"camera", "Camera", &nv_icon_camera, 20u << 20, cam_build,
                          NV_STR_APP_CAMERA, nullptr};

}  // namespace

void camera_app_register(void) { nv_app_register(&kCameraApp); }
