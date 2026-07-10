// camera_app — pro viewfinder for the 2MP MIPI-CSI camera (OV02C10).
// Live RGB565 preview (PPA-downscaled from the 1920x1080 sensor), a round shutter, a photo/video
// mode toggle, an on-screen REC timer, a last-shot thumbnail, and a folder picker so the user
// chooses where captures land. Photos -> JPEG, videos -> Motion-JPEG AVI (see nv_camera).
#include "apps_internal.h"

#include "nv_app.h"
#include "nv_ui_kit.h"
#include "nv_icons.h"
#include "nv_i18n.h"
#include "nv_theme.h"
#include "nv_fonts.h"
#include "nv_camera.h"
#include "nv_config.h"

#include "lvgl.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <sys/stat.h>
#include <dirent.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr int kPvW = 720;    // 16:9 viewfinder (matches the sensor aspect exactly — no distortion)
constexpr int kPvH = 405;
constexpr int kThW = 52;     // last-shot thumbnail
constexpr int kThH = 40;

lv_obj_t   *s_root      = nullptr;
lv_obj_t   *s_canvas    = nullptr;
uint8_t    *s_buf       = nullptr;   // preview RGB565 buffer
lv_timer_t *s_timer     = nullptr;

lv_obj_t   *s_status    = nullptr;   // transient toast label
lv_timer_t *s_status_tmr = nullptr;

lv_obj_t   *s_shutter_core = nullptr;
lv_obj_t   *s_mode_lbl  = nullptr;
lv_obj_t   *s_folder_lbl = nullptr;

lv_obj_t   *s_rec_badge = nullptr;   // "● REC 00:00" pill, shown while recording
lv_obj_t   *s_rec_lbl   = nullptr;
lv_timer_t *s_rec_tmr   = nullptr;

lv_obj_t   *s_thumb     = nullptr;
uint8_t    *s_thumb_buf = nullptr;

lv_obj_t   *s_picker    = nullptr;
lv_obj_t   *s_picker_ta = nullptr;

bool s_video_mode = false;
bool s_rec_mp4    = false;   // video container: false=MJPEG/AVI (plays on-device), true=H.264/MP4
lv_obj_t *s_fmt_lbl = nullptr;
char s_dir[96];

constexpr int kMaxDirs = 40;
char s_dirs[kMaxDirs][64];
int  s_ndirs = 0;

inline lv_color_t rec_color() { return lv_color_hex(0xE5484D); }
// A themed chip padding helper (LVGL v9 has no pad_hor/pad_ver).
inline void chip_pad(lv_obj_t *o, int h, int v) {
    lv_obj_set_style_pad_left(o, h, 0);   lv_obj_set_style_pad_right(o, h, 0);
    lv_obj_set_style_pad_top(o, v, 0);    lv_obj_set_style_pad_bottom(o, v, 0);
}

// -------- helpers --------
const char *dir_name() {
    const char *p = strrchr(s_dir, '/');
    return (p && p[1]) ? p + 1 : s_dir;
}
void refresh_folder_lbl() {
    if (s_folder_lbl) lv_label_set_text_fmt(s_folder_lbl, LV_SYMBOL_DIRECTORY "  %s", dir_name());
}

void status_clear(lv_timer_t *) { if (s_status) lv_label_set_text(s_status, ""); s_status_tmr = nullptr; }
void show_status(const char *msg, bool ok) {
    if (!s_status) return;
    const NvTheme *th = nv_theme_get();
    lv_label_set_text(s_status, msg);
    lv_obj_set_style_text_color(s_status, ok ? th->success : th->danger, 0);
    if (s_status_tmr) lv_timer_delete(s_status_tmr);
    s_status_tmr = lv_timer_create(status_clear, 1900, nullptr);
    lv_timer_set_repeat_count(s_status_tmr, 1);
}

void update_thumb() {
    if (s_thumb && s_thumb_buf && nv_camera_render(s_thumb_buf, kThW, kThH))
        lv_obj_invalidate(s_thumb);
}

// -------- preview --------
void preview_tick(lv_timer_t *) {
    if (!s_canvas || !s_buf) return;
    if (nv_camera_render(s_buf, kPvW, kPvH)) lv_obj_invalidate(s_canvas);
}

// -------- capture --------
void do_photo() {
    mkdir(s_dir, 0777);
    char path[160];
    snprintf(path, sizeof path, "%s/img-%lu.jpg", s_dir,
             (unsigned long)(esp_timer_get_time() / 1000));
    const bool ok = nv_camera_save_jpeg(path);
    if (ok) update_thumb();
    show_status(ok ? nv_tr(NV_STR_PHOTO_SAVED) : nv_tr(NV_STR_SAVE_FAILED), ok);
}

void rec_tick(lv_timer_t *) {
    if (!s_rec_lbl) return;
    uint32_t s = nv_camera_video_secs();
    lv_label_set_text_fmt(s_rec_lbl, LV_SYMBOL_VIDEO "  REC  %02u:%02u",
                          (unsigned)(s / 60), (unsigned)(s % 60));
}
void set_recording_ui(bool on) {
    if (s_rec_badge) {
        if (on) lv_obj_clear_flag(s_rec_badge, LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_add_flag(s_rec_badge, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_shutter_core)   // red disc -> red rounded square while recording
        lv_obj_set_style_radius(s_shutter_core, on ? NV_RAD_SM : LV_RADIUS_CIRCLE, 0);
    if (on) {
        rec_tick(nullptr);
        if (!s_rec_tmr) s_rec_tmr = lv_timer_create(rec_tick, 500, nullptr);
    } else if (s_rec_tmr) {
        lv_timer_delete(s_rec_tmr); s_rec_tmr = nullptr;
    }
}

void do_video() {
    if (nv_camera_video_recording()) {
        nv_camera_video_stop();
        set_recording_ui(false);
        update_thumb();
        show_status(LV_SYMBOL_OK "  Video", true);
    } else {
        mkdir(s_dir, 0777);
        char path[160];
        snprintf(path, sizeof path, "%s/vid-%lu.%s", s_dir,
                 (unsigned long)(esp_timer_get_time() / 1000), s_rec_mp4 ? "mp4" : "avi");
        if (nv_camera_video_start(path)) set_recording_ui(true);
        else show_status(nv_tr(NV_STR_SAVE_FAILED), false);
    }
}

void shutter_cb(lv_event_t *) { if (s_video_mode) do_video(); else do_photo(); }

void apply_mode() {
    if (s_shutter_core) {
        lv_obj_set_style_bg_color(s_shutter_core, s_video_mode ? rec_color() : lv_color_white(), 0);
        lv_obj_set_style_radius(s_shutter_core, LV_RADIUS_CIRCLE, 0);
    }
    if (s_mode_lbl) lv_label_set_text(s_mode_lbl, s_video_mode ? LV_SYMBOL_VIDEO : LV_SYMBOL_IMAGE);
    if (s_fmt_lbl) {
        lv_label_set_text(s_fmt_lbl, s_rec_mp4 ? "MP4" : "AVI");
        if (s_video_mode) lv_obj_clear_flag(lv_obj_get_parent(s_fmt_lbl), LV_OBJ_FLAG_HIDDEN);
        else              lv_obj_add_flag(lv_obj_get_parent(s_fmt_lbl), LV_OBJ_FLAG_HIDDEN);
    }
}
void mode_cb(lv_event_t *) {
    if (nv_camera_video_recording()) return;   // stop the recording before switching modes
    s_video_mode = !s_video_mode;
    apply_mode();
}
void fmt_cb(lv_event_t *) {
    if (nv_camera_video_recording()) return;   // don't switch container mid-recording
    s_rec_mp4 = !s_rec_mp4;                     // AVI (on-device play) <-> MP4 (H.264, small)
    apply_mode();
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
    picker_close();
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
    char full[96];
    snprintf(full, sizeof full, "/sdcard/%s", name);
    mkdir(full, 0777);
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
    if (s_picker) return;
    const NvTheme *th = nv_theme_get();
    scan_dirs();

    // scrim
    s_picker = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_picker);
    lv_obj_set_size(s_picker, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_picker, th->scrim, 0);
    lv_obj_set_style_bg_opa(s_picker, LV_OPA_70, 0);
    lv_obj_center(s_picker);
    lv_obj_clear_flag(s_picker, LV_OBJ_FLAG_SCROLLABLE);

    // panel
    lv_obj_t *panel = lv_obj_create(s_picker);
    lv_obj_set_size(panel, 460, lv_pct(88));
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
    lv_label_set_text(title, LV_SYMBOL_DIRECTORY "  Cartella");
    lv_obj_set_style_text_color(title, th->text_strong, 0);
    lv_obj_t *close = nv_kit_button(hdr, LV_SYMBOL_CLOSE, false);
    lv_obj_add_event_cb(close, [](lv_event_t *) { picker_close(); }, LV_EVENT_CLICKED, nullptr);

    // new-folder row: text field + create button
    lv_obj_t *nf = lv_obj_create(panel);
    lv_obj_remove_style_all(nf);
    lv_obj_set_size(nf, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(nf, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nf, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(nf, 8, 0);
    s_picker_ta = nv_kit_textarea(nf, "Nuova cartella…", true);
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
        lv_label_set_text(empty, "—");
        lv_obj_set_style_text_color(empty, th->text_dim, 0);
    }
}

// -------- teardown --------
void page_deleted(lv_event_t *) {
    if (s_timer)      { lv_timer_delete(s_timer); s_timer = nullptr; }
    if (s_status_tmr) { lv_timer_delete(s_status_tmr); s_status_tmr = nullptr; }
    if (s_rec_tmr)    { lv_timer_delete(s_rec_tmr); s_rec_tmr = nullptr; }
    nv_camera_stop();                       // stops recording + streaming before buffers go away
    if (s_buf)       { heap_caps_free(s_buf); s_buf = nullptr; }
    if (s_thumb_buf) { heap_caps_free(s_thumb_buf); s_thumb_buf = nullptr; }
    s_root = s_canvas = s_status = s_shutter_core = s_mode_lbl = s_fmt_lbl = nullptr;
    s_folder_lbl = s_rec_badge = s_rec_lbl = s_thumb = s_picker = s_picker_ta = nullptr;
}

// -------- build --------
void cam_build(lv_obj_t *content) {
    const NvTheme *th = nv_theme_get();
    lv_obj_set_style_bg_color(content, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);

    nv_config_get_str("cam_dir", "/sdcard/DCIM", s_dir, sizeof s_dir);

    lv_obj_t *root = lv_obj_create(content);
    s_root = root;
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(root, 8, 0);
    lv_obj_set_style_pad_row(root, 8, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, page_deleted, LV_EVENT_DELETE, nullptr);

    if (!nv_camera_start()) {
        lv_obj_t *msg = lv_label_create(root);
        lv_label_set_text(msg, nv_tr(NV_STR_NO_CAMERA));
        lv_obj_set_style_text_color(msg, th->text_dim, 0);
        return;
    }

    s_buf = (uint8_t *)heap_caps_aligned_calloc(64, 1, (size_t)kPvW * kPvH * 2,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_thumb_buf = (uint8_t *)heap_caps_aligned_calloc(64, 1, (size_t)kThW * kThH * 2,
                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buf || !s_thumb_buf) {
        nv_camera_stop();
        lv_obj_t *msg = lv_label_create(root);
        lv_label_set_text(msg, nv_tr(NV_STR_NO_CAMERA));
        lv_obj_set_style_text_color(msg, th->text_dim, 0);
        return;
    }

    // ---- TOP BAR: folder chip (left) · REC pill (center) · status toast (right) ----
    lv_obj_t *top = lv_obj_create(root);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *folder = lv_obj_create(top);
    lv_obj_remove_style_all(folder);
    lv_obj_set_size(folder, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(folder, th->surface2, 0);
    lv_obj_set_style_bg_opa(folder, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(folder, LV_RADIUS_CIRCLE, 0);
    chip_pad(folder, 14, 8);
    lv_obj_add_flag(folder, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(folder, open_picker, LV_EVENT_CLICKED, nullptr);
    s_folder_lbl = lv_label_create(folder);
    lv_obj_set_style_text_color(s_folder_lbl, th->text, 0);
    refresh_folder_lbl();

    s_rec_badge = lv_obj_create(top);
    lv_obj_remove_style_all(s_rec_badge);
    lv_obj_set_size(s_rec_badge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_rec_badge, rec_color(), 0);
    lv_obj_set_style_bg_opa(s_rec_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_rec_badge, LV_RADIUS_CIRCLE, 0);
    chip_pad(s_rec_badge, 14, 8);
    s_rec_lbl = lv_label_create(s_rec_badge);
    lv_label_set_text(s_rec_lbl, LV_SYMBOL_VIDEO "  REC  00:00");
    lv_obj_set_style_text_color(s_rec_lbl, lv_color_white(), 0);
    lv_obj_add_flag(s_rec_badge, LV_OBJ_FLAG_HIDDEN);

    s_status = lv_label_create(top);
    lv_label_set_text(s_status, "");
    lv_obj_set_style_text_color(s_status, th->text_dim, 0);

    // ---- PREVIEW ----
    s_canvas = lv_canvas_create(root);
    lv_canvas_set_buffer(s_canvas, s_buf, kPvW, kPvH, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_style_radius(s_canvas, NV_RAD_MD, 0);
    lv_obj_set_style_clip_corner(s_canvas, false, 0);   // P4 renderer bans clip-corner draw layers
    lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);

    // ---- BOTTOM BAR: thumbnail (left) · shutter (center) · mode toggle (right) ----
    lv_obj_t *bot = lv_obj_create(root);
    lv_obj_remove_style_all(bot);
    lv_obj_set_size(bot, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bot, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(bot, 24, 0);
    lv_obj_clear_flag(bot, LV_OBJ_FLAG_SCROLLABLE);

    // gallery thumbnail (framed)
    lv_obj_t *thumb_frame = lv_obj_create(bot);
    lv_obj_remove_style_all(thumb_frame);
    lv_obj_set_size(thumb_frame, kThW + 6, kThH + 6);
    lv_obj_set_style_bg_color(thumb_frame, th->surface2, 0);
    lv_obj_set_style_bg_opa(thumb_frame, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(thumb_frame, NV_RAD_SM, 0);
    lv_obj_set_style_border_width(thumb_frame, 2, 0);
    lv_obj_set_style_border_color(thumb_frame, th->divider, 0);
    lv_obj_clear_flag(thumb_frame, LV_OBJ_FLAG_SCROLLABLE);
    s_thumb = lv_canvas_create(thumb_frame);
    lv_canvas_set_buffer(s_thumb, s_thumb_buf, kThW, kThH, LV_COLOR_FORMAT_RGB565);
    lv_canvas_fill_bg(s_thumb, th->surface3, LV_OPA_COVER);
    lv_obj_center(s_thumb);

    // round shutter
    lv_obj_t *shutter = lv_obj_create(bot);
    lv_obj_remove_style_all(shutter);
    lv_obj_set_size(shutter, 74, 74);
    lv_obj_set_style_radius(shutter, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(shutter, 4, 0);
    lv_obj_set_style_border_color(shutter, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(shutter, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(shutter, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(shutter, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(shutter, shutter_cb, LV_EVENT_CLICKED, nullptr);
    s_shutter_core = lv_obj_create(shutter);
    lv_obj_remove_style_all(s_shutter_core);
    lv_obj_set_size(s_shutter_core, 58, 58);
    lv_obj_center(s_shutter_core);
    lv_obj_set_style_radius(s_shutter_core, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_shutter_core, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_shutter_core, LV_OBJ_FLAG_CLICKABLE);

    // photo/video mode toggle
    lv_obj_t *mode = lv_obj_create(bot);
    lv_obj_remove_style_all(mode);
    lv_obj_set_size(mode, 52, 52);
    lv_obj_set_style_radius(mode, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(mode, th->surface2, 0);
    lv_obj_set_style_bg_opa(mode, LV_OPA_COVER, 0);
    lv_obj_add_flag(mode, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(mode, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(mode, mode_cb, LV_EVENT_CLICKED, nullptr);
    s_mode_lbl = lv_label_create(mode);
    lv_obj_set_style_text_color(s_mode_lbl, th->accent, 0);
    lv_obj_center(s_mode_lbl);

    // video-container toggle (AVI <-> MP4), shown only in video mode
    lv_obj_t *fmt = lv_obj_create(bot);
    lv_obj_remove_style_all(fmt);
    lv_obj_set_size(fmt, 52, 52);
    lv_obj_set_style_radius(fmt, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(fmt, th->surface2, 0);
    lv_obj_set_style_bg_opa(fmt, LV_OPA_COVER, 0);
    lv_obj_add_flag(fmt, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(fmt, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(fmt, fmt_cb, LV_EVENT_CLICKED, nullptr);
    s_fmt_lbl = lv_label_create(fmt);
    lv_obj_set_style_text_font(s_fmt_lbl, &nv_font_14, 0);
    lv_obj_set_style_text_color(s_fmt_lbl, th->accent, 0);
    lv_obj_center(s_fmt_lbl);

    apply_mode();
    s_timer = lv_timer_create(preview_tick, 66, nullptr);   // ~15 fps viewfinder refresh
}

// 18 MB is the HONEST budget: 4×4.15 MB contiguous frame buffers (nv_camera.c CAM_NBUF) + the
// ~583 KB preview canvas + slack. The old 8 MB passed the broker gate and then died at the
// frame-pool alloc once the ANIMA/WASM PSRAM caches had grown — the broker now reclaims those
// caches to meet this figure, so declaring less only turns a clean refusal into a broken open.
const NvApp kCameraApp = {"camera", "Camera", &nv_icon_camera, 18u << 20, cam_build,
                          NV_STR_APP_CAMERA, nullptr};

}  // namespace

void camera_app_register(void) { nv_app_register(&kCameraApp); }
