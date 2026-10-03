// nv_capture — see include/nv_capture.h.
//
// The overlay lives on lv_layer_top (like the lock screen and Recents), not as an app: opening an
// app would tear down the very screen being captured. It shows the FROZEN frame (an RGB565 image of
// the copy nv_hal_screen_freeze() took), dims it with four rectangles around the selection, and
// draws the selection border, its size and a Lightshot-style toolbar. Saving (JPEG encode + SD
// write) runs on the background worker; the overlay is gone before it starts.
#include "nv_capture.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/stat.h>

#include "lvgl.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"

#include "nv_hal.h"
#include "nv_pins.h"
#include "nv_bgwork.h"
#include "nv_clipboard.h"
#include "nv_notify.h"
#include "nv_theme.h"
#include "nv_fonts.h"
#include "nv_i18n.h"
#include "nv_ui.h"
#include "nv_log.h"

extern "C" void lv_image_cache_drop(const void *src);

namespace {

constexpr const char *TAG = "capture";
constexpr int kW = NV_LCD_H_RES, kH = NV_LCD_V_RES;
constexpr int kMinSel = 8;                  // a smaller drag is a tap: the whole screen

enum class Act { Copy, Save, Anima };
enum class Drag { None, New, Move };

struct Ui {
    lv_obj_t *root, *img, *dim[4], *sel, *size, *hint, *bar;
    lv_image_dsc_t dsc;
    uint16_t *frame;                        // the frozen screen (physical == logical: rotation 0)
    int x0, y0, x1, y1;                     // selection, normalized (x0<=x1), x1/y1 exclusive
    bool has_sel;
    Drag drag;
    lv_point_t p0;                          // press point
    int mx0, my0;                           // selection origin when a move started
};
Ui U;

bool en() { return nv_i18n_get_lang() != NV_LANG_IT; }
#define TR(it, e) (en() ? (e) : (it))

int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// ---- the job handed to the background worker
struct Job {
    uint16_t *px;                           // the cropped region (PSRAM), freed by the job
    int w, h;
    Act act;
};

void job_run(void *arg) {
    Job *j = (Job *)arg;
    char path[96];
    bool ok;
    if (j->act == Act::Save) {
        mkdir("/sdcard/home", 0777);
        mkdir("/sdcard/home/shots", 0777);
        time_t t = time(nullptr);
        struct tm tmv;
        localtime_r(&t, &tmv);
        char stamp[24];
        strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tmv);
        snprintf(path, sizeof path, "/sdcard/home/shots/shot-%s.jpg", stamp);
    } else {
        // The clipboard's file: a few rotating names, so an older history entry keeps its own file.
        static unsigned n;
        mkdir("/sdcard/data", 0777);
        mkdir("/sdcard/data/clip", 0777);
        snprintf(path, sizeof path, "/sdcard/data/clip/clip-%u.jpg", n++ % 5);
    }
    ok = nv_hal_jpeg_save_rgb565(j->px, j->w, j->h, 0, path, 92);
    // The clipboard gets the pixels (instant paste) and the file (path-based consumers).
    nv_clip_set_image(j->px, j->w & ~7, j->h, j->w, ok ? path : nullptr, "capture");
    heap_caps_free(j->px);
    const Act act = j->act;
    const int w = j->w & ~7, h = j->h;
    free(j);
    if (!lvgl_port_lock(2000)) return;
    char msg[160];
    if (!ok && act == Act::Save) {
        nv_notify_post(NV_NOTE_WARN, TR("Screenshot", "Screenshot"), TR("Salvataggio non riuscito (SD?)", "Saving failed (SD?)"));
    } else if (act == Act::Save) {
        snprintf(msg, sizeof msg, TR("Salvato in ~/shots/%s e copiato negli appunti", "Saved to ~/shots/%s and copied to the clipboard"),
                 strrchr(path, '/') + 1);
        nv_notify_post(NV_NOTE_OK, TR("Screenshot", "Screenshot"), msg);
    } else if (act == Act::Copy) {
        snprintf(msg, sizeof msg, TR("Copiato negli appunti (%dx%d)", "Copied to the clipboard (%dx%d)"), w, h);
        nv_toast(NV_NOTE_OK, msg);
    } else {
        nv_ui_open_app_page("anima", "paste");      // ANIMA attaches the clipboard image on open
    }
    lvgl_port_unlock();
}

// ---- overlay

void close_overlay() {
    if (U.root) { lv_obj_delete(U.root); U.root = nullptr; }
    if (U.frame) { lv_image_cache_drop(&U.dsc); heap_caps_free(U.frame); U.frame = nullptr; }
    U.has_sel = false;
    U.drag = Drag::None;
}

void finish(Act act) {
    if (!U.root || !U.frame) return;
    int x0 = U.x0, y0 = U.y0, x1 = U.x1, y1 = U.y1;
    if (!U.has_sel) { x0 = 0; y0 = 0; x1 = kW; y1 = kH; }
    int w = (x1 - x0) & ~7, h = y1 - y0;                // whole 8-px JPEG blocks
    if (w < 8) w = 8;
    if (x0 + w > kW) x0 = kW - w;
    uint16_t *crop = (uint16_t *)heap_caps_malloc((size_t)w * h * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!crop) { nv_toast(NV_NOTE_WARN, TR("Memoria insufficiente per lo screenshot", "Not enough memory for the screenshot")); close_overlay(); return; }
    for (int y = 0; y < h; y++) memcpy(crop + (size_t)y * w, U.frame + (size_t)(y0 + y) * kW + x0, (size_t)w * 2);
    close_overlay();
    Job *j = (Job *)calloc(1, sizeof *j);
    if (!j) { heap_caps_free(crop); return; }
    *j = {crop, w, h, act};
    if (!nv_bgwork_submit(job_run, j)) { heap_caps_free(crop); free(j); nv_toast(NV_NOTE_WARN, TR("Sistema occupato, riprova", "System busy, try again")); }
}

lv_obj_t *plain(lv_obj_t *parent) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

void place_rect(lv_obj_t *o, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) { lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
}

void toolbar_place() {
    if (!U.bar) return;
    lv_obj_update_layout(U.bar);
    const int bw = lv_obj_get_width(U.bar), bh = lv_obj_get_height(U.bar);
    int x = U.x1 - bw, y = U.y1 + 8;                       // below the selection, right-aligned
    if (y + bh > kH - 4) y = U.y0 - bh - 8;                // no room: above it
    if (y < 4) y = U.y1 - bh - 8;                          // nor there: inside, bottom
    lv_obj_set_pos(U.bar, clampi(x, 4, kW - bw - 4), clampi(y, 4, kH - bh - 4));
}

void redraw() {
    const bool s = U.has_sel || U.drag == Drag::New;
    const int x0 = s ? U.x0 : 0, y0 = s ? U.y0 : 0, x1 = s ? U.x1 : 0, y1 = s ? U.y1 : 0;
    if (!s) {                                                // nothing yet: all dimmed
        place_rect(U.dim[0], 0, 0, kW, kH);
        for (int i = 1; i < 4; i++) lv_obj_add_flag(U.dim[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(U.sel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(U.size, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    place_rect(U.dim[0], 0, 0, kW, y0);                       // above
    place_rect(U.dim[1], 0, y1, kW, kH - y1);                 // below
    place_rect(U.dim[2], 0, y0, x0, y1 - y0);                 // left
    place_rect(U.dim[3], x1, y0, kW - x1, y1 - y0);           // right
    place_rect(U.sel, x0 - 2, y0 - 2, x1 - x0 + 4, y1 - y0 + 4);   // the border sits outside the selection
    char b[24];
    snprintf(b, sizeof b, "%d x %d", (x1 - x0) & ~7, y1 - y0);
    lv_label_set_text(U.size, b);
    lv_obj_clear_flag(U.size, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(U.size);
    const int sy = y0 - lv_obj_get_height(U.size) - 4;
    lv_obj_set_pos(U.size, x0, sy >= 2 ? sy : y0 + 4);
    toolbar_place();
}

void bar_btn(const char *sym, const char *label, Act act, bool primary) {
    const NvTheme *t = nv_theme_get();
    lv_obj_t *b = lv_obj_create(U.bar);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(b, 12, 0);
    lv_obj_set_style_pad_ver(b, 8, 0);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_bg_opa(b, primary ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(b, t->primary, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(b, primary ? t->accent : t->surface3, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, 6, 0);
    lv_obj_t *ic = lv_label_create(b);
    lv_label_set_text(ic, sym);
    lv_obj_set_style_text_font(ic, &nv_font_14, 0);
    lv_obj_set_style_text_color(ic, primary ? t->on_primary : t->text_strong, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, &nv_font_14, 0);
    lv_obj_set_style_text_color(l, primary ? t->on_primary : t->text_strong, 0);
    lv_obj_add_event_cb(b, [](lv_event_t *e) {
        const Act a = (Act)(intptr_t)lv_event_get_user_data(e);
        lv_async_call([](void *p) { finish((Act)(intptr_t)p); }, (void *)(intptr_t)a);
    }, LV_EVENT_CLICKED, (void *)(intptr_t)act);
}

void toolbar_show() {
    if (U.bar) { toolbar_place(); return; }
    const NvTheme *t = nv_theme_get();
    U.bar = lv_obj_create(U.root);
    lv_obj_remove_style_all(U.bar);
    lv_obj_set_size(U.bar, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(U.bar, t->surface, 0);
    lv_obj_set_style_bg_opa(U.bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(U.bar, 1, 0);
    lv_obj_set_style_border_color(U.bar, t->divider, 0);
    lv_obj_set_style_radius(U.bar, 8, 0);
    lv_obj_set_style_pad_all(U.bar, 4, 0);
    lv_obj_set_style_pad_column(U.bar, 2, 0);
    lv_obj_set_flex_flow(U.bar, LV_FLEX_FLOW_ROW);
    lv_obj_add_flag(U.bar, LV_OBJ_FLAG_CLICKABLE);          // taps on its padding never reach the root
    lv_obj_clear_flag(U.bar, LV_OBJ_FLAG_SCROLLABLE);
    bar_btn(LV_SYMBOL_COPY, TR("Copia", "Copy"), Act::Copy, true);
    bar_btn(LV_SYMBOL_SAVE, TR("Salva", "Save"), Act::Save, false);
    bar_btn(LV_SYMBOL_IMAGE, TR("Chiedi ad ANIMA", "Ask ANIMA"), Act::Anima, false);
    lv_obj_t *x = lv_obj_create(U.bar);                     // Cancel: an icon-only close
    lv_obj_remove_style_all(x);
    lv_obj_set_size(x, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(x, 8, 0);
    lv_obj_set_style_radius(x, 6, 0);
    lv_obj_set_style_bg_color(x, t->surface3, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(x, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_flag(x, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *xl = lv_label_create(x);
    lv_label_set_text(xl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(xl, t->text_dim, 0);
    lv_obj_add_event_cb(x, [](lv_event_t *) { lv_async_call([](void *) { close_overlay(); }, nullptr); }, LV_EVENT_CLICKED, nullptr);
    toolbar_place();
}

void root_cb(lv_event_t *e) {
    const lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    p.x = clampi(p.x, 0, kW);
    p.y = clampi(p.y, 0, kH);
    if (code == LV_EVENT_PRESSED) {
        U.p0 = p;
        if (U.has_sel && p.x >= U.x0 && p.x < U.x1 && p.y >= U.y0 && p.y < U.y1) {
            U.drag = Drag::Move; U.mx0 = U.x0; U.my0 = U.y0;
        } else {
            U.drag = Drag::New; U.has_sel = false;
            U.x0 = U.x1 = p.x; U.y0 = U.y1 = p.y;
            if (U.bar) lv_obj_add_flag(U.bar, LV_OBJ_FLAG_HIDDEN);
        }
        if (U.hint) { lv_obj_delete(U.hint); U.hint = nullptr; }
    } else if (code == LV_EVENT_PRESSING) {
        if (U.drag == Drag::New) {
            U.x0 = p.x < U.p0.x ? p.x : U.p0.x; U.x1 = p.x < U.p0.x ? U.p0.x : p.x;
            U.y0 = p.y < U.p0.y ? p.y : U.p0.y; U.y1 = p.y < U.p0.y ? U.p0.y : p.y;
        } else if (U.drag == Drag::Move) {
            const int w = U.x1 - U.x0, h = U.y1 - U.y0;
            U.x0 = clampi(U.mx0 + p.x - U.p0.x, 0, kW - w); U.x1 = U.x0 + w;
            U.y0 = clampi(U.my0 + p.y - U.p0.y, 0, kH - h); U.y1 = U.y0 + h;
        }
        redraw();
    } else if (code == LV_EVENT_RELEASED) {
        if (U.drag == Drag::New && (U.x1 - U.x0 < kMinSel || U.y1 - U.y0 < kMinSel)) {
            U.x0 = 0; U.y0 = 0; U.x1 = kW; U.y1 = kH;          // a tap: the whole screen
        }
        U.has_sel = true;
        U.drag = Drag::None;
        toolbar_show();
        lv_obj_clear_flag(U.bar, LV_OBJ_FLAG_HIDDEN);
        redraw();
    }
}

void open_overlay() {
    if (U.root) return;
    if (lv_display_get_rotation(lv_display_get_default()) != LV_DISPLAY_ROTATION_0) {
        // The frozen copy is the physical landscape panel; mapping a rotated selection back is not
        // done yet: take the whole screen instead of a wrong region.
        nv_capture_start(NV_CAPTURE_FULL, 0);
        return;
    }
    U = {};
    U.frame = nv_hal_screen_freeze();
    if (!U.frame) { nv_toast(NV_NOTE_WARN, TR("Screenshot non disponibile (memoria)", "Screenshot unavailable (memory)")); return; }
    U.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    U.dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    U.dsc.header.w = kW;
    U.dsc.header.h = kH;
    U.dsc.header.stride = kW * 2;
    U.dsc.data_size = (uint32_t)kW * kH * 2;
    U.dsc.data = (const uint8_t *)U.frame;

    const NvTheme *t = nv_theme_get();
    U.root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(U.root);
    lv_obj_set_size(U.root, kW, kH);
    lv_obj_add_flag(U.root, LV_OBJ_FLAG_CLICKABLE);         // a full-size barrier: input stops here
    lv_obj_clear_flag(U.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(U.root, root_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(U.root, root_cb, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(U.root, root_cb, LV_EVENT_RELEASED, nullptr);

    U.img = lv_image_create(U.root);
    lv_image_set_src(U.img, &U.dsc);
    lv_obj_clear_flag(U.img, LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < 4; i++) {
        U.dim[i] = plain(U.root);
        lv_obj_set_style_bg_color(U.dim[i], lv_color_black(), 0);
        lv_obj_set_style_bg_opa(U.dim[i], LV_OPA_60, 0);   // visible even over dark apps
    }
    U.sel = plain(U.root);
    lv_obj_set_style_border_width(U.sel, 2, 0);
    lv_obj_set_style_border_color(U.sel, t->accent, 0);
    U.size = lv_label_create(U.root);
    lv_obj_set_style_text_font(U.size, &nv_font_14, 0);
    lv_obj_set_style_text_color(U.size, lv_color_white(), 0);
    lv_obj_set_style_bg_color(U.size, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(U.size, LV_OPA_70, 0);
    lv_obj_set_style_pad_hor(U.size, 6, 0);
    lv_obj_set_style_pad_ver(U.size, 2, 0);
    lv_obj_set_style_radius(U.size, 4, 0);
    U.hint = lv_label_create(U.root);
    lv_label_set_text(U.hint, TR("Trascina per selezionare un'area  " LV_SYMBOL_BULLET "  tocca per tutto lo schermo  " LV_SYMBOL_BULLET "  Esc annulla",
                                 "Drag to select an area  " LV_SYMBOL_BULLET "  tap for the whole screen  " LV_SYMBOL_BULLET "  Esc cancels"));
    lv_obj_set_style_text_font(U.hint, &nv_font_14, 0);
    lv_obj_set_style_text_color(U.hint, lv_color_white(), 0);
    lv_obj_set_style_bg_color(U.hint, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(U.hint, LV_OPA_70, 0);
    lv_obj_set_style_pad_hor(U.hint, 14, 0);
    lv_obj_set_style_pad_ver(U.hint, 8, 0);
    lv_obj_set_style_radius(U.hint, 16, 0);
    lv_obj_align(U.hint, LV_ALIGN_TOP_MID, 0, 16);
    redraw();
}

void start_timer_cb(lv_timer_t *tm) {
    const nv_capture_mode_t mode = (nv_capture_mode_t)(intptr_t)lv_timer_get_user_data(tm);
    lv_timer_delete(tm);
    nv_capture_start(mode, 0);
}

}  // namespace

void nv_capture_start(nv_capture_mode_t mode, uint32_t delay_ms) {
    if (delay_ms) {
        lv_timer_t *t = lv_timer_create(start_timer_cb, delay_ms, (void *)(intptr_t)mode);
        lv_timer_set_repeat_count(t, 1);
        return;
    }
    if (mode == NV_CAPTURE_REGION) { open_overlay(); return; }
    // Whole screen, no overlay: straight to the clipboard.
    uint16_t *frame = nv_hal_screen_freeze();
    if (!frame) { nv_toast(NV_NOTE_WARN, TR("Screenshot non disponibile (memoria)", "Screenshot unavailable (memory)")); return; }
    Job *j = (Job *)calloc(1, sizeof *j);
    if (!j) { heap_caps_free(frame); return; }
    *j = {frame, kW, kH, Act::Copy};
    if (!nv_bgwork_submit(job_run, j)) { heap_caps_free(frame); free(j); }
}

bool nv_capture_active(void) { return U.root != nullptr; }

bool nv_capture_escape(void) {
    if (!U.root) return false;
    close_overlay();
    return true;
}

bool nv_capture_key(uint8_t u, uint8_t mods) {
    if (!U.root) return false;
    const bool ctrl = mods & 0x11;
    switch (u) {
        case 0x29: close_overlay(); return true;                                  // Esc
        case 0x28: case 0x58: finish(Act::Copy); return true;                     // Enter
        case 0x06: if (ctrl) { finish(Act::Copy); return true; } break;           // Ctrl+C
        case 0x16: if (ctrl) { finish(Act::Save); return true; } break;           // Ctrl+S
        default: break;
    }
    return true;                                     // the overlay is modal: nothing leaks to the app
}
