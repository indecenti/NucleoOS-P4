// video_app — the NucleoOS "mini VLC" video player. A preview canvas on top (HW JPEG decode for
// .avi/MJPEG, esp_h264 SW decode for .mp4/H.264, letterboxed/stretched/zoomed by nv_vplayer), a
// full-width scrub bar with ±10 s skip, a transport row (prev/play/next/stop + volume + gear +
// fullscreen), a folder browser rooted at /sdcard (reaches DCIM, Video, wherever content lives),
// a slide-in Player settings panel (repeat / aspect / keep-screen-on), and a true fullscreen mode
// with auto-hiding floating controls. Engine: nv_vplayer (decode/ring/render + MP4 AAC audio +
// real seek — see nv_vplayer.h).
#include "apps_internal.h"

#include "nv_app.h"
#include "nv_ui.h"
#include "nv_ui_kit.h"
#include "nv_icons.h"
#include "nv_i18n.h"
#include "nv_theme.h"
#include "nv_fonts.h"
#include "nv_vplayer.h"
#include "nv_gesture.h"
#include "nv_config.h"
#include "nv_audio.h"
#include "nv_hal.h"     // nv_hal_video_blit — direct-to-panel video (bypass LVGL compositing)

#include "lvgl.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <strings.h>
#include <dirent.h>

namespace {

constexpr char kRootDir[]  = "/sdcard";
constexpr int  kMaxEnts    = 256;
constexpr int  kNameLen    = 128;
constexpr int  kWinVW = 1024, kWinVH = 336;   // windowed preview canvas (full width, letterboxed).
                                              // Sized so canvas + control bar always fit the content
                                              // area with margin — controls never pushed off-screen.
constexpr int  kFsVW  = 1024, kFsVH  = 600;   // fullscreen canvas (whole panel)
constexpr int  kFsCtlH = 132;                 // fullscreen: opaque control strip reserved at the bottom
                                              // must fit seek row (40) + gap (8) + transport row (58) + pad_ver (16) = 122;
                                              // too short and the transport buttons overflow below the screen = untappable
                                              // (video shrinks above it when controls show -> no flicker)
constexpr int  kSkipMs = 10000;               // ±10 s scrub-bar skip
constexpr uint32_t kCtlHideMs = 3500;         // fullscreen: auto-hide the floating bar after this

struct Entry { char name[kNameLen]; bool is_dir; };

char     s_dir[300] = "/sdcard";       // current browse directory
Entry   *s_ents = nullptr;             // PSRAM, allocated on app open, freed on close
int      s_nents = 0;
int      s_cur   = -1;                 // index of the playing clip WITHIN s_ents (-1 = none / stale after nav)
int      s_repeat = 0;                 // 0 off · 1 all · 2 one
int      s_aspect = 0;                 // nv_vp_aspect_t: 0 fit · 1 stretch · 2 zoom

uint8_t   *s_buf   = nullptr;   // canvas RGB565 buffer (sized for fullscreen, reused windowed)
lv_obj_t  *s_canvas= nullptr;
lv_obj_t  *s_play  = nullptr;
lv_obj_t  *s_pos   = nullptr;
lv_obj_t  *s_dur   = nullptr;
lv_obj_t  *s_seek  = nullptr;   // scrub slider
lv_obj_t  *s_ctlbar= nullptr;   // holds seek row + transport row; floats in fullscreen
lv_obj_t  *s_vol_row = nullptr; // hidden when the current clip has no audio track
lv_obj_t  *s_vol_ic  = nullptr;
lv_obj_t  *s_badge = nullptr;   // codec/fps badge (floats over the canvas top-left)
lv_obj_t  *s_err_msg = nullptr; // "unsupported format" overlay, shown on NV_VP_ERROR mid-clip
lv_obj_t  *s_list  = nullptr;       // scroll column (folder + clip rows), lives inside s_list_panel
lv_obj_t  *s_list_panel = nullptr;  // slide-in drawer pinned to the right edge, like s_settings
bool       s_list_open  = false;
lv_obj_t  *s_root  = nullptr;
lv_obj_t  *s_fs_btn= nullptr;
lv_timer_t *s_timer= nullptr;
uint32_t   s_seen_gen = 0;
bool       s_scrubbing = false;
bool       s_blit_clear = true;   // black the letterbox margins on the next direct-blit (open/resize)
int        s_vx = 0, s_vy = 0, s_vw = 0, s_vh = 0;   // cached on-screen video rect (LVGL thread writes, decode cb reads)
bool       s_fs = false;                // fullscreen active
uint32_t   s_ctl_shown_ms = 0;          // lv_tick when the floating bar was last revealed

// settings panel
lv_obj_t  *s_settings = nullptr;        // slide-in panel (right edge)
lv_obj_t  *s_scrim    = nullptr;        // tap-outside-to-close dim
bool       s_settings_open = false;
lv_obj_t  *s_rep_pills[3] = {nullptr, nullptr, nullptr};
lv_obj_t  *s_asp_pills[3] = {nullptr, nullptr, nullptr};

void build_list(void);   // fwd (play_index/nav re-highlight/rebuild)
void update_badge(void);
void fs_exit(void);      // fwd (installed as Back handler by fs_apply)
void settings_close(void);   // fwd (list_toggle_cb closes the sibling drawer)
void list_close(void);       // fwd (settings_toggle_cb / fs_apply close the sibling drawer)
int  cur_vw(void){ return s_fs ? kFsVW : kWinVW; }
int  cur_vh(void){ return s_fs ? kFsVH : kWinVH; }

void fmt_ms(char *b, size_t n, int ms){ if (ms<0) ms=0; lv_snprintf(b,n,"%d:%02d", ms/60000, (ms/1000)%60); }

// Display task — pinned to core 0, PARALLEL with the decode task on core 1 (the real 2-core video
// pipeline). It pops the freshest ring frame and blits it straight to the panel on an EXACT source-
// framerate cadence (vTaskDelayUntil), so motion is perfectly even regardless of per-frame decode
// jitter, and the ~1 ms PPA blit is lifted off the decode core (more decode headroom → higher res).
TaskHandle_t s_disp_task = nullptr;
volatile bool s_disp_run = false;
uint32_t s_disp_gen = 0;
void disp_task(void *){
    TickType_t next = xTaskGetTickCount();
    bool was_occluded = false;
    while (s_disp_run) {
        int per = nv_vplayer_period_ms(); if (per < 8) per = 8; if (per > 100) per = 100;
        vTaskDelayUntil(&next, pdMS_TO_TICKS(per));
        // NEVER paint over a system overlay (notification shade pulled down) or an in-app drawer
        // (settings / clip list): the direct blit bypasses LVGL, so it would draw the video ON TOP of
        // them. Pause while occluded — LVGL owns those pixels — and re-black the letterbox on resume.
        if (nv_ui_shade_is_open() || s_settings_open || s_list_open) { was_occluded = true; continue; }
        if (was_occluded) { was_occluded = false; s_blit_clear = true; }
        if (s_vw <= 1 || s_vh <= 1) continue;
        uint32_t gen = 0; int w = 0, h = 0;
        const uint8_t *f = nv_vplayer_frame(&w, &h, &gen);
        if (f && w > 0 && h > 0 && (gen != s_disp_gen || s_blit_clear)) {
            nv_hal_video_blit(f, w, h, s_vx, s_vy, s_vw, s_vh, true);   // always wipe the margins (cheap:
            s_blit_clear = false;                                       // only the bars, not the picture)
            s_disp_gen = gen;
        }
    }
    s_disp_task = nullptr;
    vTaskDelete(NULL);
}

// Re-blit the freshest frame immediately (aspect/size change). While PLAYING the frame cb repaints
// on its own on the next frame — only paint here when paused/stopped (then the decode task is idle,
// so there's no concurrent blit on the shared PPA client).
void redraw_now(void){
    s_blit_clear = true;
    if (nv_vplayer_state() == NV_VP_PLAYING) return;
    uint32_t gen; int fw = 0, fh = 0;
    const uint8_t *frame = nv_vplayer_frame(&fw, &fh, &gen);
    if (frame && fw > 0 && fh > 0 && s_vw > 1)
        nv_hal_video_blit(frame, fw, fh, s_vx, s_vy, s_vw, s_vh, true);
}

void scan_dir(void){
    s_nents = 0;
    if (!s_ents) return;
    DIR *d = opendir(s_dir);
    if (!d) return;
    struct dirent *e;
    while (s_nents < kMaxEnts && (e = readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') continue;
        const bool dir = (e->d_type == DT_DIR);
        if (!dir && !nv_vplayer_is_video(e->d_name)) continue;
        strncpy(s_ents[s_nents].name, e->d_name, kNameLen-1);
        s_ents[s_nents].name[kNameLen-1] = '\0';
        s_ents[s_nents].is_dir = dir;
        s_nents++;
    }
    closedir(d);
}

// Next/previous PLAYABLE (non-folder) entry, wrapping when repeat is on (all OR one — manual skip
// wraps in both; repeat-one only changes the EOT auto-advance, handled in tick()).
int pick_delta(int dir){
    if (s_nents <= 0 || s_cur < 0) return -1;
    int i = s_cur;
    for (int steps = 0; steps < s_nents; steps++) {
        i += dir;
        if (i < 0)        { if (s_repeat != 0) i = s_nents - 1; else return -1; }
        if (i >= s_nents)  { if (s_repeat != 0) i = 0;          else return -1; }
        if (!s_ents[i].is_dir) return i;
    }
    return -1;
}

void play_index(int i){
    if (i < 0 || i >= s_nents || s_ents[i].is_dir) return;
    s_cur = i;
    char full[400];
    lv_snprintf(full, sizeof full, "%s/%s", s_dir, s_ents[i].name);
    s_seen_gen = 0;
    s_blit_clear = true;   // black the letterbox margins on the first frame of the new clip
    if (s_canvas) lv_obj_invalidate(s_canvas);   // one LVGL redraw of the black placeholder base
    nv_vplayer_open(full);
    build_list();   // re-highlight the active clip
}

void enter_dir(const char *name){
    const size_t len = strlen(s_dir);
    if (len + 1 + strlen(name) >= sizeof s_dir) return;   // would truncate: refuse
    snprintf(s_dir + len, sizeof s_dir - len, "/%s", name);
    s_cur = -1;   // stale in the new listing — browsing away doesn't stop playback, just the highlight
    scan_dir();
    build_list();
}
void up_dir(void){
    if (strcmp(s_dir, kRootDir) == 0) return;
    char *slash = strrchr(s_dir, '/');
    if (!slash || slash == s_dir) return;
    *slash = '\0';
    s_cur = -1;
    scan_dir();
    build_list();
}

void row_cb(lv_event_t *e){
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i == -1) { up_dir(); return; }
    if (s_ents[i].is_dir) enter_dir(s_ents[i].name);
    else                  play_index(i);
}

// keep the fullscreen floating bar alive when the user touches a control
void bump_ctl(void){ s_ctl_shown_ms = lv_tick_get(); }

void next_cb(lv_event_t *){ bump_ctl(); int n = pick_delta(+1); if (n >= 0) play_index(n); }
void prev_cb(lv_event_t *){ bump_ctl(); int p = pick_delta(-1); if (p >= 0) play_index(p); }
void gesture_next(void *)  { int n = pick_delta(+1); if (n >= 0) play_index(n); }
void gesture_prev(void *)  { int p = pick_delta(-1); if (p >= 0) play_index(p); }

void playpause_cb(lv_event_t *){
    bump_ctl();
    switch (nv_vplayer_state()) {
        case NV_VP_PLAYING: nv_vplayer_pause(true);  break;
        case NV_VP_PAUSED:  nv_vplayer_pause(false); break;
        default:            if (s_cur >= 0) play_index(s_cur);
                            else { int n = 0; while (n < s_nents && s_ents[n].is_dir) n++; if (n < s_nents) play_index(n); }
                            break;
    }
}
void stop_cb(lv_event_t *){ bump_ctl(); nv_vplayer_stop(); }

void skip_ms(int delta){
    const int dur = nv_vplayer_dur_ms();
    if (dur <= 0) return;
    int p = nv_vplayer_pos_ms() + delta;
    if (p < 0) p = 0; if (p > dur) p = dur;
    nv_vplayer_seek(p);
}
void skip_back_cb(lv_event_t *){ bump_ctl(); skip_ms(-kSkipMs); }
void skip_fwd_cb (lv_event_t *){ bump_ctl(); skip_ms(+kSkipMs); }

// ---------------------------------------------------------------- seek + volume
void seek_pressed_cb(lv_event_t *)  { s_scrubbing = true; bump_ctl(); }
void seek_released_cb(lv_event_t *) {
    s_scrubbing = false;
    const int dur = nv_vplayer_dur_ms();
    if (s_seek && dur > 0) nv_vplayer_seek((int)((int64_t)dur * lv_slider_get_value(s_seek) / 1000));
}
void seek_scrub_cb(lv_event_t *) {   // live time preview under the finger
    if (!s_scrubbing || !s_pos || !s_seek) return;
    const int dur = nv_vplayer_dur_ms();
    if (dur <= 0) return;
    char b[16];
    fmt_ms(b, sizeof b, (int)((int64_t)dur * lv_slider_get_value(s_seek) / 1000));
    lv_label_set_text(s_pos, b);
}

void vol_icon_paint(int vol){
    if (!s_vol_ic) return;
    const bool muted = nv_config_get_bool("mute", false);
    lv_label_set_text(s_vol_ic, muted || vol == 0 ? LV_SYMBOL_MUTE
                              : vol < 55          ? LV_SYMBOL_VOLUME_MID
                                                  : LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_color(s_vol_ic, muted ? nv_theme_get()->danger : nv_theme_get()->text_dim, 0);
}
void vol_changed_cb(lv_event_t *e){   // live while dragging
    const int v = (int)lv_slider_get_value((lv_obj_t *)lv_event_get_target(e));
    nv_audio_set_volume(v);
    vol_icon_paint(v);
    bump_ctl();
}
void vol_released_cb(lv_event_t *e){  // persist once, on release (settings-app pattern)
    nv_config_set_int("volume", (int)lv_slider_get_value((lv_obj_t *)lv_event_get_target(e)));
}
void mute_toggle_cb(lv_event_t *){    // tap the volume icon
    const bool m = !nv_config_get_bool("mute", false);
    nv_config_set_bool("mute", m);
    nv_audio_set_mute(m);
    vol_icon_paint(nv_config_get_int("volume", 60));
}

lv_obj_t *round_btn(lv_obj_t *parent, const char *sym, lv_event_cb_t cb, bool primary, int size){
    lv_obj_t *b = nv_kit_button(parent, sym, primary);
    lv_obj_set_size(b, size, size);
    lv_obj_set_style_radius(b, size / 2, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    return b;
}

void update_badge(void){
    if (!s_badge) return;
    if (!nv_config_get_bool("vid_fps", false)) { lv_obj_add_flag(s_badge, LV_OBJ_FLAG_HIDDEN); return; }
    if (s_cur < 0 || nv_vplayer_state() == NV_VP_STOPPED) { lv_obj_add_flag(s_badge, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_clear_flag(s_badge, LV_OBJ_FLAG_HIDDEN);
    const char *ext = strrchr(s_ents[s_cur].name, '.');
    const char *codec = !ext                                       ? "H.264"
                      : strcasecmp(ext, ".avi") == 0                ? "MJPEG"
                      : (strcasecmp(ext, ".mpg") == 0 || strcasecmp(ext, ".mpeg") == 0 ||
                         strcasecmp(ext, ".m1v") == 0)              ? "MPEG-1"
                                                                    : "H.264";
    const int f = nv_vplayer_fps10();
    char b[32];
    if (f > 0) lv_snprintf(b, sizeof b, "%s \xC2\xB7 %d.%d fps", codec, f/10, f%10);
    else       lv_snprintf(b, sizeof b, "%s", codec);
    lv_label_set_text(s_badge, b);
}

// ---------------------------------------------------------------- fullscreen
void set_canvas_size(int w, int h){
    if (!s_canvas || !s_buf) return;
    memset(s_buf, 0, (size_t)w*h*2);
    lv_canvas_set_buffer(s_canvas, s_buf, w, h, LV_COLOR_FORMAT_RGB565);
    s_seen_gen = 0;   // force the next tick to re-render at the new size
}

void fs_apply(bool on){
    if (on == s_fs) return;
    s_fs = on;
    nv_ui_app_fullscreen(on);
    if (on) {
        // canvas fills the whole panel; controls float; close any open drawer first
        set_canvas_size(kFsVW, kFsVH);
        list_close();
        if (s_ctlbar) {
            lv_obj_add_flag(s_ctlbar, LV_OBJ_FLAG_IGNORE_LAYOUT);
            lv_obj_set_size(s_ctlbar, lv_pct(100), kFsCtlH);   // fixed strip; video sits above it
            lv_obj_align(s_ctlbar, LV_ALIGN_BOTTOM_MID, 0, 0);
            lv_obj_set_style_bg_color(s_ctlbar, nv_theme_get()->surface, 0);
            lv_obj_set_style_bg_opa(s_ctlbar, LV_OPA_COVER, 0);   // OPAQUE: the blit never touches this band
            lv_obj_set_style_pad_hor(s_ctlbar, 16, 0);
            lv_obj_set_style_pad_ver(s_ctlbar, 8, 0);
            lv_obj_set_style_radius(s_ctlbar, 0, 0);
            lv_obj_move_foreground(s_ctlbar);   // ABOVE the full-screen clickable canvas, else the
                                                // canvas eats taps meant for the transport buttons
            lv_obj_add_flag(s_ctlbar, LV_OBJ_FLAG_CLICKABLE);   // catch taps on the strip so they
                                                                // don't fall through to the canvas
        }
        if (s_fs_btn) lv_label_set_text(lv_obj_get_child(s_fs_btn, 0), LV_SYMBOL_CLOSE);
        nv_ui_set_back_handler(fs_exit);   // Back exits fullscreen instead of closing the app
        // Immersive: the control bar is pinned to the screen's bottom/left EDGE, exactly where the
        // system edge strips live. Those strips are CLICKABLE screen children stacked ABOVE the whole
        // app plane (kept topmost by nv_gesture_raise), so they SWALLOW every tap on the transport /
        // seek / volume controls (a strip owns the indev hit; it never falls through to the bar under
        // it). move_foreground only reorders WITHIN the app plane, so it can't beat them. Suppress the
        // strips while fullscreen — exit is the on-screen Close button (s_fs_btn) / Back handler.
        nv_gesture_set_edge_enabled(NV_GESTURE_EDGE_BOTTOM, false);
        nv_gesture_set_edge_enabled(NV_GESTURE_EDGE_LEFT,   false);
        bump_ctl();
    } else {
        set_canvas_size(kWinVW, kWinVH);
        if (s_ctlbar) {
            lv_obj_clear_flag(s_ctlbar, LV_OBJ_FLAG_IGNORE_LAYOUT);
            lv_obj_clear_flag(s_ctlbar, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_size(s_ctlbar, lv_pct(100), LV_SIZE_CONTENT);
            lv_obj_set_style_bg_opa(s_ctlbar, LV_OPA_TRANSP, 0);
            lv_obj_set_style_pad_hor(s_ctlbar, 12, 0);
            lv_obj_set_style_pad_ver(s_ctlbar, 4, 0);
            lv_obj_clear_flag(s_ctlbar, LV_OBJ_FLAG_CLICKABLE);
        }
        if (s_fs_btn) lv_label_set_text(lv_obj_get_child(s_fs_btn, 0), LV_SYMBOL_IMAGE);
        // Restore the system edge strips to their normal in-app state (mirrors open_app: LEFT = back,
        // BOTTOM = swipe-up-home) now that the controls are back inside the app content area.
        nv_gesture_set_edge_enabled(NV_GESTURE_EDGE_BOTTOM, true);
        nv_gesture_set_edge_enabled(NV_GESTURE_EDGE_LEFT,   true);
        nv_gesture_raise();
        nv_ui_set_back_handler(nullptr);
        // Leaving FS: the direct-blit painted the whole panel; the windowed canvas is smaller, so the
        // area outside it would keep a GHOST of the last full-screen frame. Force LVGL to repaint the
        // whole screen (chrome + margins) so nothing lingers.
        lv_obj_invalidate(lv_screen_active());
    }
    redraw_now();
}
void fs_exit(void){ fs_apply(false); }   // system Back handler while fullscreen
void fs_toggle_cb(lv_event_t *){ bump_ctl(); fs_apply(!s_fs); }

void canvas_click_cb(lv_event_t *){
    if (s_fs) {
        // toggle the floating controls; revealing also restarts the auto-hide timer
        if (s_ctlbar) {
            if (lv_obj_has_flag(s_ctlbar, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_clear_flag(s_ctlbar, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(s_ctlbar);   // above the canvas so its buttons take taps
                bump_ctl();
            }
            else lv_obj_add_flag(s_ctlbar, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        // windowed: tap-to-play/pause (VLC-style), but only when a clip is loaded
        if (s_cur >= 0) {
            const nv_vp_state_t st = nv_vplayer_state();
            if (st == NV_VP_PLAYING) nv_vplayer_pause(true);
            else if (st == NV_VP_PAUSED) nv_vplayer_pause(false);
        }
    }
}

// ---------------------------------------------------------------- settings panel
void aspect_paint(void){
    const NvTheme *th = nv_theme_get();
    for (int i = 0; i < 3; i++) if (s_asp_pills[i]) {
        const bool sel = (i == s_aspect);
        lv_obj_set_style_bg_color(s_asp_pills[i], sel ? th->accent : th->surface3, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(s_asp_pills[i], 0), sel ? th->on_primary : th->text, 0);
    }
}
void repeat_paint(void){
    const NvTheme *th = nv_theme_get();
    for (int i = 0; i < 3; i++) if (s_rep_pills[i]) {
        const bool sel = (i == s_repeat);
        lv_obj_set_style_bg_color(s_rep_pills[i], sel ? th->accent : th->surface3, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(s_rep_pills[i], 0), sel ? th->on_primary : th->text, 0);
    }
}
void aspect_pick_cb(lv_event_t *e){
    s_aspect = (int)(intptr_t)lv_event_get_user_data(e);
    nv_vplayer_set_aspect((nv_vp_aspect_t)s_aspect);
    nv_config_set_int("vid_aspect", s_aspect);
    aspect_paint();
    redraw_now();
}
void repeat_pick_cb(lv_event_t *e){
    s_repeat = (int)(intptr_t)lv_event_get_user_data(e);
    nv_config_set_int("vid_repeat", s_repeat);
    repeat_paint();
}
void keepawake_cb(lv_event_t *e){
    nv_config_set_bool("vid_awake", lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED));
}
void fps_cb(lv_event_t *e){   // show/hide the codec·fps badge (top-left); tick() re-reads it live
    nv_config_set_bool("vid_fps", lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED));
}

lv_obj_t *pill(lv_obj_t *row, const char *txt, lv_event_cb_t cb, int idx){
    lv_obj_t *b = lv_button_create(row);
    lv_obj_remove_style_all(b);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_height(b, 46);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    return b;
}
lv_obj_t *seg_row(lv_obj_t *parent){
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(r, 8, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}
lv_obj_t *seg_label(lv_obj_t *parent, const char *txt){
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, nv_theme_get()->text_dim, 0);
    lv_obj_set_style_pad_top(l, 6, 0);
    return l;
}

void settings_close(void){
    s_settings_open = false;
    if (s_settings) lv_obj_add_flag(s_settings, LV_OBJ_FLAG_HIDDEN);
    if (s_scrim && !s_list_open) lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
}
void scrim_tap_cb(lv_event_t *){ settings_close(); list_close(); }
void settings_toggle_cb(lv_event_t *){
    bump_ctl();
    if (s_settings_open) { settings_close(); return; }
    list_close();   // only one drawer open at a time
    s_settings_open = true;
    if (s_scrim)    { lv_obj_clear_flag(s_scrim, LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(s_scrim); }
    if (s_settings) { lv_obj_clear_flag(s_settings, LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(s_settings); }
}

// ---------------------------------------------------------------- clip list drawer
// Slide-in panel pinned to the right edge, same mechanics as the settings panel above (and
// shares its scrim) so only one of the two is ever open at once.
void list_close(void){
    s_list_open = false;
    if (s_list_panel) lv_obj_add_flag(s_list_panel, LV_OBJ_FLAG_HIDDEN);
    if (s_scrim && !s_settings_open) lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
}
void list_toggle_cb(lv_event_t *){
    bump_ctl();
    if (s_list_open) { list_close(); return; }
    settings_close();   // only one drawer open at a time
    s_list_open = true;
    if (s_scrim)      { lv_obj_clear_flag(s_scrim, LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(s_scrim); }
    if (s_list_panel) { lv_obj_clear_flag(s_list_panel, LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(s_list_panel); }
}

void build_list_panel(lv_obj_t *root){
    const NvTheme *th = nv_theme_get();

    s_list_panel = lv_obj_create(root);
    lv_obj_remove_style_all(s_list_panel);
    lv_obj_add_flag(s_list_panel, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(s_list_panel, 360, lv_pct(100));
    lv_obj_align(s_list_panel, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(s_list_panel, th->surface, 0);
    lv_obj_set_style_bg_opa(s_list_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_list_panel, 14, 0);
    lv_obj_set_style_pad_row(s_list_panel, 8, 0);
    lv_obj_set_flex_flow(s_list_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_border_side(s_list_panel, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(s_list_panel, 1, 0);
    lv_obj_set_style_border_color(s_list_panel, th->surface3, 0);
    lv_obj_clear_flag(s_list_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_list_panel);
    lv_label_set_text(title, "Elenco");
    lv_obj_set_style_text_color(title, th->text_strong, 0);
    lv_obj_set_style_text_font(title, &nv_font_20, 0);

    s_list = nv_kit_scroll_column(s_list_panel);
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_width(s_list, lv_pct(100));

    lv_obj_add_flag(s_list_panel, LV_OBJ_FLAG_HIDDEN);
}

void build_settings(lv_obj_t *root){
    const NvTheme *th = nv_theme_get();

    // dim + tap-to-close scrim (floats over the whole app content)
    s_scrim = lv_obj_create(root);
    lv_obj_remove_style_all(s_scrim);
    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(s_scrim, lv_pct(100), lv_pct(100));
    lv_obj_align(s_scrim, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scrim, 110, 0);
    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_scrim, scrim_tap_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);

    // panel pinned to the right edge
    s_settings = lv_obj_create(root);
    lv_obj_remove_style_all(s_settings);
    lv_obj_add_flag(s_settings, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(s_settings, 360, lv_pct(100));
    lv_obj_align(s_settings, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(s_settings, th->surface, 0);
    lv_obj_set_style_bg_opa(s_settings, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_settings, 18, 0);
    lv_obj_set_style_pad_row(s_settings, 10, 0);
    lv_obj_set_flex_flow(s_settings, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_border_side(s_settings, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(s_settings, 1, 0);
    lv_obj_set_style_border_color(s_settings, th->surface3, 0);

    lv_obj_t *title = lv_label_create(s_settings);
    lv_label_set_text(title, "Player");
    lv_obj_set_style_text_color(title, th->text_strong, 0);
    lv_obj_set_style_text_font(title, &nv_font_20, 0);

    // Repeat
    seg_label(s_settings, "Ripeti");
    lv_obj_t *rr = seg_row(s_settings);
    s_rep_pills[0] = pill(rr, "Off", repeat_pick_cb, 0);
    s_rep_pills[1] = pill(rr, "Tutti", repeat_pick_cb, 1);
    s_rep_pills[2] = pill(rr, "Uno", repeat_pick_cb, 2);
    repeat_paint();

    // Aspect
    seg_label(s_settings, "Proporzioni");
    lv_obj_t *ar = seg_row(s_settings);
    s_asp_pills[0] = pill(ar, "Adatta", aspect_pick_cb, 0);
    s_asp_pills[1] = pill(ar, "Estendi", aspect_pick_cb, 1);
    s_asp_pills[2] = pill(ar, "Zoom", aspect_pick_cb, 2);
    aspect_paint();

    // Keep screen on
    nv_kit_switch_row(s_settings, "Schermo sempre acceso",
                      nv_config_get_bool("vid_awake", true), keepawake_cb);

    // Show the codec·fps badge (off by default — clean picture)
    nv_kit_switch_row(s_settings, "Mostra FPS",
                      nv_config_get_bool("vid_fps", false), fps_cb);

    lv_obj_add_flag(s_settings, LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------- periodic tick
void tick(lv_timer_t *){
    // Keep the cached on-screen video rect fresh for the decode-thread blit (LVGL geometry must be read
    // on this thread). In fullscreen the video fills the panel, but when the control strip is visible
    // it shrinks to sit ABOVE it — so the opaque controls are never overwritten by the blit (no flicker).
    if (s_fs) {
        const bool ctl_vis = s_ctlbar && !lv_obj_has_flag(s_ctlbar, LV_OBJ_FLAG_HIDDEN);
        int nvh = ctl_vis ? (kFsVH - kFsCtlH) : kFsVH;
        if (nvh != s_vh) s_blit_clear = true;   // rect changed -> re-black the letterbox margins
        s_vx = 0; s_vy = 0; s_vw = kFsVW; s_vh = nvh;
    } else if (s_canvas) {
        lv_area_t a; lv_obj_get_coords(s_canvas, &a);
        s_vx = a.x1; s_vy = a.y1; s_vw = a.x2 - a.x1 + 1; s_vh = a.y2 - a.y1 + 1;
    }
    const nv_vp_state_t st = nv_vplayer_state();
    if (s_play) lv_label_set_text(lv_obj_get_child(s_play, 0),
                                  st == NV_VP_PLAYING ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    if (s_err_msg) {
        if (st == NV_VP_ERROR && s_cur >= 0) {
            const char *why = nv_vplayer_err_reason();
            char msg[160];
            lv_snprintf(msg, sizeof msg, "%s\n\nConverti il file in H.264 Baseline per riprodurlo.",
                        (why && why[0]) ? why : "Formato video non supportato");
            lv_label_set_text(s_err_msg, msg);
            lv_obj_clear_flag(s_err_msg, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_err_msg, LV_OBJ_FLAG_HIDDEN);
        }
    }

    const int pos = nv_vplayer_pos_ms(), dur = nv_vplayer_dur_ms();
    char b[16];
    if (s_pos && !s_scrubbing) { fmt_ms(b, sizeof b, pos); lv_label_set_text(s_pos, b); }
    if (s_dur) { fmt_ms(b, sizeof b, dur); lv_label_set_text(s_dur, b); }
    if (s_seek && !s_scrubbing) {
        int pct = (dur>0) ? (int)((int64_t)pos*1000/dur) : 0;
        if (pct>1000) pct=1000;
        lv_slider_set_value(s_seek, pct, LV_ANIM_OFF);
    }
    if (s_vol_row) {
        const bool has_audio = nv_vplayer_has_audio();
        if (has_audio) lv_obj_clear_flag(s_vol_row, LV_OBJ_FLAG_HIDDEN);
        else           lv_obj_add_flag(s_vol_row, LV_OBJ_FLAG_HIDDEN);
    }
    update_badge();

    // Keep-screen-on: while actually playing, poke LVGL's idle clock so the panel saver never fires.
    if (st == NV_VP_PLAYING && nv_config_get_bool("vid_awake", true))
        lv_display_trigger_activity(nullptr);

    // Fullscreen: auto-hide the floating controls after inactivity (only while playing, so a paused
    // or stopped clip keeps them visible).
    if (s_fs && s_ctlbar && !lv_obj_has_flag(s_ctlbar, LV_OBJ_FLAG_HIDDEN)
        && st == NV_VP_PLAYING && lv_tick_elaps(s_ctl_shown_ms) > kCtlHideMs)
        lv_obj_add_flag(s_ctlbar, LV_OBJ_FLAG_HIDDEN);

    if (nv_vplayer_took_eot()) {
        if (s_repeat == 2) { if (s_cur >= 0) play_index(s_cur); }          // repeat one
        else { const int n = (s_repeat == 1) ? pick_delta(+1) : -1;        // repeat all / off
               if (n >= 0) play_index(n); }
        // else: leave the last frame on screen, transport shows Play
    }
}

void build_list(void){
    if (!s_list) return;
    lv_obj_clean(s_list);
    const NvTheme *th = nv_theme_get();

    if (strcmp(s_dir, kRootDir) != 0) {
        lv_obj_t *row = lv_obj_create(s_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(row, th->surface, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, th->surface3, LV_STATE_PRESSED);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_pad_all(row, 12, 0);
        lv_obj_set_style_pad_column(row, 10, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_event_cb(row, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
        lv_obj_t *ic = lv_label_create(row);
        lv_label_set_text(ic, LV_SYMBOL_LEFT);
        lv_obj_set_style_text_color(ic, th->text_dim, 0);
        lv_obj_t *nm = lv_label_create(row);
        lv_label_set_text(nm, "..");
        lv_obj_set_style_text_color(nm, th->text_dim, 0);
    }

    if (s_nents == 0) {
        lv_obj_t *l = lv_label_create(s_list);
        lv_label_set_text(l, nv_tr(NV_STR_NO_VIDEO));
        lv_obj_set_style_text_color(l, th->text_dim, 0);
        return;
    }
    for (int i = 0; i < s_nents; i++) {
        lv_obj_t *row = lv_obj_create(s_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        const bool active = (i == s_cur);
        lv_obj_set_style_bg_color(row, active ? th->surface2 : th->surface, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, th->surface3, LV_STATE_PRESSED);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_pad_all(row, 12, 0);
        lv_obj_set_style_pad_column(row, 10, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        if (active) {
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
            lv_obj_set_style_border_width(row, 3, 0);
            lv_obj_set_style_border_color(row, th->accent, 0);
        }
        lv_obj_add_event_cb(row, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *ic = lv_label_create(row);
        lv_label_set_text(ic, s_ents[i].is_dir ? LV_SYMBOL_DIRECTORY : (active ? LV_SYMBOL_PLAY : LV_SYMBOL_VIDEO));
        lv_obj_set_style_text_color(ic, s_ents[i].is_dir ? th->text_dim : th->accent, 0);
        lv_obj_t *nm = lv_label_create(row);
        lv_label_set_text(nm, s_ents[i].name);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(nm, 1);
        lv_obj_set_style_text_color(nm, th->text_strong, 0);
    }
}

void page_deleted(lv_event_t *){
    nv_ui_set_shade_gesture_enabled(true);   // restore the notification shade gesture on exit
    s_vw = s_vh = 0;                      // stop the display task from blitting into a torn-down state
    s_disp_run = false;                   // and wait for it to actually exit before freeing the ring
    for (int i = 0; i < 60 && s_disp_task; i++) vTaskDelay(pdMS_TO_TICKS(5));
    if (s_fs) { s_fs = false; nv_ui_app_fullscreen(false); }   // restore chrome if torn down mid-FS
    // The direct-blit wrote video straight to the framebuffer behind LVGL's back, so LVGL doesn't know
    // that region is dirty — force a full-screen invalidate so the launcher we return to fully repaints
    // and no ghost of the last frame lingers.
    lv_obj_invalidate(lv_screen_active());
    nv_ui_set_back_handler(nullptr);
    if (s_timer) { lv_timer_delete(s_timer); s_timer = nullptr; }
    nv_vplayer_release();                 // stop + free the engine's ring/decoder buffers
    if (s_buf) { heap_caps_free(s_buf); s_buf = nullptr; }
    if (s_ents) { heap_caps_free(s_ents); s_ents = nullptr; s_nents = 0; }
    s_canvas = s_play = s_pos = s_dur = s_seek = s_ctlbar = s_vol_row = s_vol_ic = s_badge = s_err_msg = s_list = nullptr;
    s_root = s_fs_btn = s_settings = s_scrim = s_list_panel = nullptr;
    s_rep_pills[0] = s_rep_pills[1] = s_rep_pills[2] = nullptr;
    s_asp_pills[0] = s_asp_pills[1] = s_asp_pills[2] = nullptr;
    s_settings_open = s_list_open = s_scrubbing = false;
    strcpy(s_dir, kRootDir);
    s_cur = -1;
}

void video_build(lv_obj_t *content){
    s_fs = false; s_vw = s_vh = 0; s_blit_clear = true;   // always (re)open windowed & clean
    nv_ui_set_shade_gesture_enabled(false);   // no shade swipe over the film while the player is open
    nv_vplayer_init();
    if (!s_ents) s_ents = (Entry *)heap_caps_malloc((size_t)kMaxEnts * sizeof(Entry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    strcpy(s_dir, kRootDir);
    scan_dir();
    const NvTheme *th = nv_theme_get();

    // restore persisted player prefs
    s_repeat = nv_config_get_int("vid_repeat", 0);  if (s_repeat < 0 || s_repeat > 2) s_repeat = 0;
    s_aspect = nv_config_get_int("vid_aspect", 0);  if (s_aspect < 0 || s_aspect > 2) s_aspect = 0;
    nv_vplayer_set_aspect((nv_vp_aspect_t)s_aspect);

    // PPA writes into this buffer (nv_vplayer_render): external-memory PPA targets need L1+L2
    // cache-line alignment, same convention as nv_camera's frame buffers.
    if (!s_buf) s_buf = (uint8_t *)heap_caps_aligned_alloc(64, (size_t)kFsVW*kFsVH*2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    lv_obj_t *root = lv_obj_create(content);
    s_root = root;
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(root, 0, 0);          // canvas is full-bleed; children pad themselves
    lv_obj_set_style_pad_row(root, 8, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, page_deleted, LV_EVENT_DELETE, nullptr);

    // video canvas (windowed size to start; grows to full panel in fullscreen)
    if (s_buf) {
        memset(s_buf, 0, (size_t)kWinVW*kWinVH*2);
        s_canvas = lv_canvas_create(root);
        lv_canvas_set_buffer(s_canvas, s_buf, kWinVW, kWinVH, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_style_radius(s_canvas, NV_RAD_MD, 0);
        lv_obj_set_style_clip_corner(s_canvas, false, 0);   // P4 renderer bans clip-corner draw layers
        lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);   // tap = play/pause (windowed) / toggle controls (FS)
        lv_obj_add_event_cb(s_canvas, canvas_click_cb, LV_EVENT_CLICKED, nullptr);
        nv_gesture_bind(s_canvas, LV_DIR_LEFT,  gesture_next, nullptr);
        nv_gesture_bind(s_canvas, LV_DIR_RIGHT, gesture_prev, nullptr);
    }

    // codec/fps badge — floats over the canvas top-left
    s_badge = lv_label_create(root);
    lv_obj_add_flag(s_badge, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(s_badge, LV_ALIGN_TOP_LEFT, 14, 14);
    lv_label_set_text(s_badge, "");
    lv_obj_set_style_text_font(s_badge, &nv_font_14, 0);
    lv_obj_set_style_text_color(s_badge, lv_color_white(), 0);
    lv_obj_set_style_bg_color(s_badge, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_badge, 130, 0);
    lv_obj_set_style_pad_hor(s_badge, 8, 0);
    lv_obj_set_style_pad_ver(s_badge, 3, 0);
    lv_obj_set_style_radius(s_badge, 6, 0);
    lv_obj_add_flag(s_badge, LV_OBJ_FLAG_HIDDEN);

    // "unsupported format" overlay — centered over the canvas, shown only on NV_VP_ERROR mid-clip
    // (e.g. H.264 High/Main profile: this decoder only supports Baseline/CAVLC) so a failed decode
    // reads as a clear message instead of a silent black screen with audio still playing.
    s_err_msg = lv_label_create(root);
    lv_obj_add_flag(s_err_msg, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_width(s_err_msg, lv_pct(80));
    lv_label_set_long_mode(s_err_msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_err_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_err_msg, "Formato video non supportato\n(profilo H.264 non compatibile)");
    lv_obj_set_style_text_color(s_err_msg, th->text_dim, 0);
    lv_obj_align_to(s_err_msg, s_canvas, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_err_msg, LV_OBJ_FLAG_HIDDEN);

    // control bar: [seek row] + [transport row]. Normal flow child windowed; floats in fullscreen.
    s_ctlbar = lv_obj_create(root);
    lv_obj_remove_style_all(s_ctlbar);
    lv_obj_set_size(s_ctlbar, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_ctlbar, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_ctlbar, 8, 0);
    lv_obj_set_style_pad_hor(s_ctlbar, 12, 0);   // keep sliders off the panel edge (windowed)
    lv_obj_set_style_pad_ver(s_ctlbar, 4, 0);
    lv_obj_clear_flag(s_ctlbar, LV_OBJ_FLAG_SCROLLABLE);

    // seek row: pos  [-10]  [=====slider=====]  [+10]  dur
    lv_obj_t *sr = lv_obj_create(s_ctlbar);
    lv_obj_remove_style_all(sr);
    lv_obj_set_size(sr, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(sr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(sr, 8, 0);
    lv_obj_clear_flag(sr, LV_OBJ_FLAG_SCROLLABLE);

    s_pos = lv_label_create(sr); lv_label_set_text(s_pos, "0:00"); lv_obj_set_style_text_color(s_pos, th->text_dim, 0);
    round_btn(sr, LV_SYMBOL_PREV, skip_back_cb, false, 40);
    s_seek = lv_slider_create(sr);
    lv_obj_set_flex_grow(s_seek, 1);
    lv_obj_set_height(s_seek, 10);
    lv_slider_set_range(s_seek, 0, 1000);
    lv_obj_set_style_bg_color(s_seek, th->surface3, 0);
    lv_obj_set_style_bg_color(s_seek, th->accent, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_seek, th->accent, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_seek, 8, LV_PART_KNOB);
    lv_obj_set_ext_click_area(s_seek, 16);
    lv_obj_add_event_cb(s_seek, seek_pressed_cb,  LV_EVENT_PRESSED,       nullptr);
    lv_obj_add_event_cb(s_seek, seek_released_cb, LV_EVENT_RELEASED,      nullptr);
    lv_obj_add_event_cb(s_seek, seek_scrub_cb,    LV_EVENT_VALUE_CHANGED, nullptr);
    round_btn(sr, LV_SYMBOL_NEXT, skip_fwd_cb, false, 40);
    s_dur = lv_label_create(sr); lv_label_set_text(s_dur, "0:00"); lv_obj_set_style_text_color(s_dur, th->text_dim, 0);

    // transport row: prev · play · next · stop  ...  volume  ...  gear · fullscreen
    lv_obj_t *tr = lv_obj_create(s_ctlbar);
    lv_obj_remove_style_all(tr);
    lv_obj_set_size(tr, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(tr, 10, 0);
    lv_obj_clear_flag(tr, LV_OBJ_FLAG_SCROLLABLE);

    round_btn(tr, LV_SYMBOL_PREV, prev_cb, false, 48);
    s_play = round_btn(tr, LV_SYMBOL_PLAY, playpause_cb, true, 58);
    round_btn(tr, LV_SYMBOL_NEXT, next_cb, false, 48);
    round_btn(tr, LV_SYMBOL_STOP, stop_cb, false, 44);

    lv_obj_t *spacer = lv_obj_create(tr);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_size(spacer, 1, 1);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_clear_flag(spacer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(spacer, LV_OBJ_FLAG_SCROLLABLE);

    s_vol_row = lv_obj_create(tr);
    lv_obj_remove_style_all(s_vol_row);
    lv_obj_set_size(s_vol_row, 190, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_vol_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_vol_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_vol_row, 8, 0);
    lv_obj_clear_flag(s_vol_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_vol_row, LV_OBJ_FLAG_HIDDEN);   // shown once the open clip proves to have audio

    s_vol_ic = lv_label_create(s_vol_row);
    lv_obj_add_flag(s_vol_ic, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(s_vol_ic, 6, 0);
    lv_obj_add_event_cb(s_vol_ic, mute_toggle_cb, LV_EVENT_CLICKED, nullptr);
    vol_icon_paint(nv_config_get_int("volume", 60));

    lv_obj_t *vol = lv_slider_create(s_vol_row);
    lv_obj_set_flex_grow(vol, 1);
    lv_obj_set_height(vol, 10);
    lv_slider_set_range(vol, 0, 100);
    lv_slider_set_value(vol, nv_config_get_int("volume", 60), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(vol, th->surface3, 0);
    lv_obj_set_style_bg_color(vol, th->accent, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(vol, th->accent, LV_PART_KNOB);
    lv_obj_set_style_pad_all(vol, 8, LV_PART_KNOB);
    lv_obj_set_ext_click_area(vol, 16);
    lv_obj_add_event_cb(vol, vol_changed_cb,  LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(vol, vol_released_cb, LV_EVENT_RELEASED,      nullptr);

    round_btn(tr, LV_SYMBOL_LIST, list_toggle_cb, false, 44);
    round_btn(tr, LV_SYMBOL_SETTINGS, settings_toggle_cb, false, 44);
    s_fs_btn = round_btn(tr, LV_SYMBOL_IMAGE, fs_toggle_cb, false, 44);

    // slide-in settings + clip-list drawers (built last so they stack above the flow; the second
    // call's scrim reuses the first's — see build_list_panel). Both start hidden.
    build_settings(root);
    build_list_panel(root);
    build_list();

    nv_ui_set_back_handler(nullptr);   // set only while fullscreen (see fs_apply)
    // Back exits fullscreen instead of closing the app while immersed:
    // installed lazily by fs_apply(true); here we just make sure it starts clean.

    s_blit_clear = true;
    s_disp_gen = 0; s_disp_run = true;
    xTaskCreatePinnedToCore(disp_task, "viddisp", 4096, nullptr, 4, &s_disp_task, 0);  // core 0, || decode
    s_timer = lv_timer_create(tick, 33, nullptr);   // UI-only refresh (pos/controls/rect cache)
}

const NvApp kVideoApp = {"video", "Video", &nv_icon_video, 8u << 20, video_build,
                         NV_STR_APP_VIDEO, nullptr};

}  // namespace

void video_app_register(void) { nv_app_register(&kVideoApp); }
