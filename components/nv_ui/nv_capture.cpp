// nv_capture — see include/nv_capture.h.
//
// The overlay lives on lv_layer_top (like the lock screen and Recents), not as an app: opening an
// app would tear down the very screen being captured. It shows the FROZEN frame (an RGB565 image of
// the copy nv_hal_screen_freeze() took, turned to the orientation the user sees), dims it with four
// rectangles around the selection, and draws the selection border, its size and a Lightshot-style
// toolbar: annotation tools (pen, arrow, rectangle, highlighter, colour, undo) and the actions.
// Annotations are previewed as LVGL objects and burnt into the pixels when the shot is taken.
// Saving (JPEG encode + SD write) runs on the background worker; the overlay is gone before it starts.
#include "nv_capture.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cmath>
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
constexpr int kMinSel = 8;                  // a smaller drag is a tap: the whole screen
constexpr int kMaxShapes = 48;
constexpr int kPenPts = 1024;               // points of one freehand stroke

enum class Act { Copy, Save, Anima };
enum class Drag { None, New, Move, Draw };
enum Tool : uint8_t { T_NONE = 0, T_PEN, T_ARROW, T_RECT, T_MARK };

const uint32_t kColors[] = {0xE53935, 0xFDD835, 0x43A047, 0x1E88E5, 0xFFFFFF};
constexpr int kNColors = sizeof kColors / sizeof kColors[0];

struct Shape {
    Tool tool;
    uint32_t color;
    int n;                                  // points used
    lv_point_precise_t *pts;                // PSRAM: pen strokes, or the 5-point arrow polyline
    lv_obj_t *obj;                          // preview (an lv_line, or a bordered box for the rectangle)
    int x0, y0, x1, y1;                     // rectangle / arrow end points (screen space)
};

struct Ui {
    lv_obj_t *root, *img, *dim[4], *sel, *size, *hint, *bar, *swatch;
    lv_obj_t *tool_btn[5];
    lv_image_dsc_t dsc;
    uint16_t *frame;                        // the frozen screen, LOGICAL orientation, w x h
    int w, h;
    int x0, y0, x1, y1;                     // selection, normalized (x0<=x1), x1/y1 exclusive
    bool has_sel;
    Drag drag;
    lv_point_t p0;                          // press point
    int mx0, my0;                           // selection origin when a move started
    Tool tool;
    int color;
    Shape shapes[kMaxShapes];
    int nshapes;
};
Ui U;

bool en() { return nv_i18n_get_lang() != NV_LANG_IT; }
#define TR(it, e) (en() ? (e) : (it))

int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// ---- the frozen frame, in the orientation the user sees
// The panel is always a physical 1024x600 landscape framebuffer; with the UI rotated, a logical pixel
// (x, y) lives at the physical one nv_disp maps it to (to_physical in nv_disp.cpp).
uint16_t *freeze_logical(int *w, int *h) {
    uint16_t *phys = nv_hal_screen_freeze();
    if (!phys) return nullptr;
    lv_display_t *d = lv_display_get_default();
    const lv_display_rotation_t rot = lv_display_get_rotation(d);
    const int lw = lv_display_get_horizontal_resolution(d), lh = lv_display_get_vertical_resolution(d);
    *w = lw; *h = lh;
    if (rot == LV_DISPLAY_ROTATION_0) return phys;
    uint16_t *log = (uint16_t *)heap_caps_aligned_alloc(64, (size_t)lw * lh * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!log) { heap_caps_free(phys); return nullptr; }
    const int PW = NV_LCD_H_RES;
    for (int y = 0; y < lh; y++) {
        uint16_t *row = log + (size_t)y * lw;
        for (int x = 0; x < lw; x++) {
            int px, py;
            switch (rot) {
                case LV_DISPLAY_ROTATION_90:  px = y;           py = lw - 1 - x; break;
                case LV_DISPLAY_ROTATION_180: px = lw - 1 - x;  py = lh - 1 - y; break;
                default:                      px = lh - 1 - y;  py = x;          break;   // 270
            }
            row[x] = phys[(size_t)py * PW + px];
        }
    }
    heap_caps_free(phys);
    return log;
}

// ---- annotations burnt into the pixels (software raster, coverage mask per shape)
inline uint16_t rgb565(uint32_t c) {
    return (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
}
inline uint16_t blend565(uint16_t a, uint16_t b, int alpha) {   // b over a, alpha 0..255
    const int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31, br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
    return (uint16_t)((((ar * (255 - alpha) + br * alpha) / 255) << 11) |
                      (((ag * (255 - alpha) + bg * alpha) / 255) << 5) | ((ab * (255 - alpha) + bb * alpha) / 255));
}

struct Mask { uint8_t *m; int w, h; };
void disc(Mask &k, int cx, int cy, int r) {
    for (int dy = -r; dy <= r; dy++) {
        const int y = cy + dy;
        if (y < 0 || y >= k.h) continue;
        for (int dx = -r; dx <= r; dx++) {
            const int x = cx + dx;
            if (x < 0 || x >= k.w || dx * dx + dy * dy > r * r + r) continue;
            k.m[(size_t)y * k.w + x] = 1;
        }
    }
}
void seg(Mask &k, int x0, int y0, int x1, int y1, int r) {
    const int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        disc(k, x0, y0, r);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
int stroke_w(Tool t) { return t == T_MARK ? 16 : t == T_PEN ? 4 : 4; }

void burn(uint16_t *fb, int fw, int fh) {
    if (!U.nshapes) return;
    Mask k = {(uint8_t *)heap_caps_malloc((size_t)fw * fh, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT), fw, fh};
    if (!k.m) return;
    for (int s = 0; s < U.nshapes; s++) {
        const Shape &sh = U.shapes[s];
        memset(k.m, 0, (size_t)fw * fh);
        const int r = stroke_w(sh.tool) / 2;
        if (sh.tool == T_RECT) {
            seg(k, sh.x0, sh.y0, sh.x1, sh.y0, r); seg(k, sh.x1, sh.y0, sh.x1, sh.y1, r);
            seg(k, sh.x1, sh.y1, sh.x0, sh.y1, r); seg(k, sh.x0, sh.y1, sh.x0, sh.y0, r);
        } else {
            for (int i = 1; i < sh.n; i++)
                seg(k, (int)sh.pts[i - 1].x, (int)sh.pts[i - 1].y, (int)sh.pts[i].x, (int)sh.pts[i].y, r);
            if (sh.n == 1) disc(k, (int)sh.pts[0].x, (int)sh.pts[0].y, r);
        }
        const uint16_t c = rgb565(sh.color);
        const int alpha = sh.tool == T_MARK ? 128 : 255;   // the highlighter lets the text show through (= the preview's 50 %)
        for (size_t i = 0; i < (size_t)fw * fh; i++)
            if (k.m[i]) fb[i] = alpha == 255 ? c : blend565(fb[i], c, alpha);
    }
    heap_caps_free(k.m);
}

void shapes_clear() {
    for (int i = 0; i < U.nshapes; i++) heap_caps_free(U.shapes[i].pts);   // objects go with the root
    U.nshapes = 0;
}

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
    shapes_clear();
    if (U.frame) { lv_image_cache_drop(&U.dsc); heap_caps_free(U.frame); U.frame = nullptr; }
    U.has_sel = false;
    U.drag = Drag::None;
    U.bar = nullptr;
}

void finish(Act act) {
    if (!U.root || !U.frame) return;
    int x0 = U.x0, y0 = U.y0, x1 = U.x1, y1 = U.y1;
    if (!U.has_sel) { x0 = 0; y0 = 0; x1 = U.w; y1 = U.h; }
    burn(U.frame, U.w, U.h);                             // annotations into the pixels
    int w = (x1 - x0) & ~7, h = y1 - y0;                // whole 8-px JPEG blocks
    if (w < 8) w = 8;
    if (x0 + w > U.w) x0 = U.w - w;
    uint16_t *crop = (uint16_t *)heap_caps_malloc((size_t)w * h * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!crop) { nv_toast(NV_NOTE_WARN, TR("Memoria insufficiente per lo screenshot", "Not enough memory for the screenshot")); close_overlay(); return; }
    for (int y = 0; y < h; y++) memcpy(crop + (size_t)y * w, U.frame + (size_t)(y0 + y) * U.w + x0, (size_t)w * 2);
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
    if (y + bh > U.h - 4) y = U.y0 - bh - 8;               // no room: above it
    if (y < 4) y = U.y1 - bh - 8;                          // nor there: inside, bottom
    lv_obj_set_pos(U.bar, clampi(x, 4, U.w - bw - 4), clampi(y, 4, U.h - bh - 4));
}

void redraw() {
    const bool s = U.has_sel || U.drag == Drag::New;
    const int x0 = s ? U.x0 : 0, y0 = s ? U.y0 : 0, x1 = s ? U.x1 : 0, y1 = s ? U.y1 : 0;
    if (!s) {                                                // nothing yet: all dimmed
        place_rect(U.dim[0], 0, 0, U.w, U.h);
        for (int i = 1; i < 4; i++) lv_obj_add_flag(U.dim[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(U.sel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(U.size, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    place_rect(U.dim[0], 0, 0, U.w, y0);                      // above
    place_rect(U.dim[1], 0, y1, U.w, U.h - y1);               // below
    place_rect(U.dim[2], 0, y0, x0, y1 - y0);                 // left
    place_rect(U.dim[3], x1, y0, U.w - x1, y1 - y0);          // right
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

// ---- toolbar: tool icons are drawn (lines / boxes), not font glyphs

lv_obj_t *bar_button(lv_event_cb_t cb, void *ud, bool primary) {
    const NvTheme *t = nv_theme_get();
    lv_obj_t *b = lv_obj_create(U.bar);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, primary ? LV_SIZE_CONTENT : 36, 36);
    lv_obj_set_style_pad_hor(b, primary ? 12 : 0, 0);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_bg_color(b, primary ? t->primary : t->text_strong, 0);
    lv_obj_set_style_bg_opa(b, primary ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(b, primary ? LV_OPA_COVER : LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(b, t->accent, LV_STATE_CHECKED);           // the active tool
    lv_obj_set_style_bg_opa(b, LV_OPA_40, LV_STATE_CHECKED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, 6, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

lv_obj_t *glyph_line(lv_obj_t *parent, const lv_point_precise_t *pts, int n, int width, lv_color_t c, lv_opa_t opa) {
    lv_obj_t *l = lv_line_create(parent);
    lv_line_set_points(l, pts, (uint32_t)n);
    lv_obj_set_style_line_width(l, width, 0);
    lv_obj_set_style_line_color(l, c, 0);
    lv_obj_set_style_line_opa(l, opa, 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

// 20x20 tool icons. Static point arrays: lv_line keeps the pointer.
const lv_point_precise_t kIcPen[] = {{3, 16}, {6, 11}, {10, 13}, {14, 6}, {17, 3}};
const lv_point_precise_t kIcArrow[] = {{3, 17}, {16, 4}, {9, 4}, {16, 4}, {16, 11}};
const lv_point_precise_t kIcMark[] = {{3, 10}, {17, 10}};
const lv_point_precise_t kIcUndo[] = {{16, 15}, {16, 9}, {13, 6}, {4, 6}, {8, 2}, {4, 6}, {8, 10}};

lv_obj_t *icon_box(lv_obj_t *b) {
    lv_obj_t *box = plain(b);
    lv_obj_set_size(box, 20, 20);
    return box;
}

void tool_refresh() {
    for (int i = 1; i <= 4; i++)
        if (U.tool_btn[i]) {
            if (U.tool == i) lv_obj_add_state(U.tool_btn[i], LV_STATE_CHECKED);
            else lv_obj_remove_state(U.tool_btn[i], LV_STATE_CHECKED);
        }
    if (U.swatch) lv_obj_set_style_bg_color(U.swatch, lv_color_hex(kColors[U.color]), 0);
}

void tool_cb(lv_event_t *e) {
    const Tool t = (Tool)(intptr_t)lv_event_get_user_data(e);
    U.tool = U.tool == t ? T_NONE : t;                     // tap the active one again: back to move
    tool_refresh();
}
void color_cb(lv_event_t *) { U.color = (U.color + 1) % kNColors; tool_refresh(); }
void undo() {
    if (!U.nshapes) return;
    Shape &s = U.shapes[--U.nshapes];
    if (s.obj) lv_obj_delete(s.obj);
    heap_caps_free(s.pts);
    s = {};
}
void undo_cb(lv_event_t *) { undo(); }
void act_cb(lv_event_t *e) {
    lv_async_call([](void *p) { finish((Act)(intptr_t)p); }, lv_event_get_user_data(e));
}

void separator() {
    lv_obj_t *s = plain(U.bar);
    lv_obj_set_size(s, 1, 24);
    lv_obj_set_style_bg_color(s, nv_theme_get()->divider, 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_margin_left(s, 4, 0);
    lv_obj_set_style_margin_right(s, 4, 0);
}

void toolbar_show() {
    if (U.bar) { toolbar_place(); return; }
    const NvTheme *t = nv_theme_get();
    const lv_color_t ink = t->text_strong;
    U.bar = lv_obj_create(U.root);
    lv_obj_remove_style_all(U.bar);
    lv_obj_set_size(U.bar, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(U.bar, t->surface, 0);
    lv_obj_set_style_bg_opa(U.bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(U.bar, 1, 0);
    lv_obj_set_style_border_color(U.bar, t->divider, 0);
    lv_obj_set_style_radius(U.bar, 10, 0);
    lv_obj_set_style_pad_all(U.bar, 4, 0);
    lv_obj_set_style_pad_column(U.bar, 2, 0);
    lv_obj_set_flex_flow(U.bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(U.bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(U.bar, LV_OBJ_FLAG_CLICKABLE);          // taps on its padding never reach the root
    lv_obj_clear_flag(U.bar, LV_OBJ_FLAG_SCROLLABLE);

    // Tools
    lv_obj_t *b = bar_button(tool_cb, (void *)(intptr_t)T_PEN, false);
    glyph_line(icon_box(b), kIcPen, 5, 2, ink, LV_OPA_COVER);
    U.tool_btn[T_PEN] = b;
    b = bar_button(tool_cb, (void *)(intptr_t)T_ARROW, false);
    glyph_line(icon_box(b), kIcArrow, 5, 2, ink, LV_OPA_COVER);
    U.tool_btn[T_ARROW] = b;
    b = bar_button(tool_cb, (void *)(intptr_t)T_RECT, false);
    lv_obj_t *rb = plain(icon_box(b));
    lv_obj_set_size(rb, 16, 12);
    lv_obj_align(rb, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_border_width(rb, 2, 0);
    lv_obj_set_style_border_color(rb, ink, 0);
    lv_obj_set_style_radius(rb, 2, 0);
    U.tool_btn[T_RECT] = b;
    b = bar_button(tool_cb, (void *)(intptr_t)T_MARK, false);
    glyph_line(icon_box(b), kIcMark, 2, 8, lv_color_hex(0xFDD835), LV_OPA_70);
    U.tool_btn[T_MARK] = b;
    b = bar_button(color_cb, nullptr, false);                // colour: a round swatch, tap to cycle
    U.swatch = plain(b);
    lv_obj_set_size(U.swatch, 18, 18);
    lv_obj_set_style_radius(U.swatch, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(U.swatch, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(U.swatch, 2, 0);
    lv_obj_set_style_border_color(U.swatch, t->surface3, 0);
    b = bar_button(undo_cb, nullptr, false);
    glyph_line(icon_box(b), kIcUndo, 7, 2, ink, LV_OPA_COVER);
    separator();

    // Actions
    b = bar_button(act_cb, (void *)(intptr_t)Act::Copy, true);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, LV_SYMBOL_COPY);
    lv_obj_set_style_text_color(l, t->on_primary, 0);
    l = lv_label_create(b);
    lv_label_set_text(l, TR("Copia", "Copy"));
    lv_obj_set_style_text_font(l, &nv_font_14, 0);
    lv_obj_set_style_text_color(l, t->on_primary, 0);
    b = bar_button(act_cb, (void *)(intptr_t)Act::Save, false);
    l = lv_label_create(b);
    lv_label_set_text(l, LV_SYMBOL_SAVE);
    lv_obj_set_style_text_color(l, ink, 0);
    b = bar_button(act_cb, (void *)(intptr_t)Act::Anima, false);
    l = lv_label_create(b);
    lv_label_set_text(l, LV_SYMBOL_IMAGE);
    lv_obj_set_style_text_color(l, ink, 0);
    b = bar_button([](lv_event_t *) { lv_async_call([](void *) { close_overlay(); }, nullptr); }, nullptr, false);
    l = lv_label_create(b);
    lv_label_set_text(l, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(l, t->text_dim, 0);
    tool_refresh();
    toolbar_place();
}

// ---- drawing

bool in_sel(lv_point_t p) { return U.has_sel && p.x >= U.x0 && p.x < U.x1 && p.y >= U.y0 && p.y < U.y1; }

void shape_begin(lv_point_t p) {
    if (U.nshapes >= kMaxShapes) return;
    Shape &s = U.shapes[U.nshapes];
    s = {};
    s.tool = U.tool;
    s.color = kColors[U.color];
    s.x0 = s.x1 = p.x; s.y0 = s.y1 = p.y;
    const lv_color_t c = lv_color_hex(s.color);
    if (s.tool == T_RECT) {
        s.obj = plain(U.root);
        lv_obj_set_style_border_width(s.obj, stroke_w(T_RECT), 0);
        lv_obj_set_style_border_color(s.obj, c, 0);
        place_rect(s.obj, p.x, p.y, 1, 1);
    } else {
        s.pts = (lv_point_precise_t *)heap_caps_malloc(sizeof(lv_point_precise_t) * (s.tool == T_PEN || s.tool == T_MARK ? kPenPts : 5),
                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s.pts) return;
        s.pts[0] = {(lv_value_precise_t)p.x, (lv_value_precise_t)p.y};
        s.n = 1;
        s.obj = glyph_line(U.root, s.pts, 1, stroke_w(s.tool), c, s.tool == T_MARK ? LV_OPA_50 : LV_OPA_COVER);
    }
    if (U.bar) lv_obj_move_foreground(U.bar);
    U.nshapes++;
}

// The arrow as one polyline: shaft, then back and forth over the tip for the two head strokes.
void arrow_points(Shape &s) {
    const float dx = (float)(s.x1 - s.x0), dy = (float)(s.y1 - s.y0);
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1) len = 1;
    const float ux = dx / len, uy = dy / len, hl = len < 60 ? len * 0.35f : 20.0f, hw = hl * 0.6f;
    const float bx = s.x1 - ux * hl, by = s.y1 - uy * hl;
    s.pts[0] = {(lv_value_precise_t)s.x0, (lv_value_precise_t)s.y0};
    s.pts[1] = {(lv_value_precise_t)s.x1, (lv_value_precise_t)s.y1};
    s.pts[2] = {(lv_value_precise_t)(bx - uy * hw), (lv_value_precise_t)(by + ux * hw)};
    s.pts[3] = {(lv_value_precise_t)s.x1, (lv_value_precise_t)s.y1};
    s.pts[4] = {(lv_value_precise_t)(bx + uy * hw), (lv_value_precise_t)(by - ux * hw)};
    s.n = 5;
}

void shape_move(lv_point_t p) {
    if (!U.nshapes) return;
    Shape &s = U.shapes[U.nshapes - 1];
    p.x = clampi(p.x, U.x0, U.x1 - 1);
    p.y = clampi(p.y, U.y0, U.y1 - 1);
    s.x1 = p.x; s.y1 = p.y;
    if (s.tool == T_RECT) {
        place_rect(s.obj, LV_MIN(s.x0, s.x1), LV_MIN(s.y0, s.y1), abs(s.x1 - s.x0) + 1, abs(s.y1 - s.y0) + 1);
    } else if (s.tool == T_ARROW && s.pts) {
        arrow_points(s);
        lv_line_set_points(s.obj, s.pts, (uint32_t)s.n);
    } else if (s.pts && s.n < kPenPts) {
        const lv_point_precise_t &last = s.pts[s.n - 1];
        if (abs((int)last.x - p.x) + abs((int)last.y - p.y) < 2) return;   // keep strokes lean
        s.pts[s.n++] = {(lv_value_precise_t)p.x, (lv_value_precise_t)p.y};
        lv_line_set_points(s.obj, s.pts, (uint32_t)s.n);
    }
}

void shape_end() {
    if (!U.nshapes) return;
    Shape &s = U.shapes[U.nshapes - 1];
    if (s.tool == T_RECT || s.tool == T_ARROW) {
        if (abs(s.x1 - s.x0) < 3 && abs(s.y1 - s.y0) < 3) { undo(); return; }   // a tap draws nothing
        if (s.tool == T_RECT) {                              // normalized corners for the raster
            const int ax = LV_MIN(s.x0, s.x1), bx = LV_MAX(s.x0, s.x1), ay = LV_MIN(s.y0, s.y1), by = LV_MAX(s.y0, s.y1);
            s.x0 = ax; s.x1 = bx; s.y0 = ay; s.y1 = by;
        }
    }
}

void root_cb(lv_event_t *e) {
    const lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    p.x = clampi(p.x, 0, U.w);
    p.y = clampi(p.y, 0, U.h);
    if (code == LV_EVENT_PRESSED) {
        U.p0 = p;
        if (U.hint) { lv_obj_delete(U.hint); U.hint = nullptr; }
        if (U.tool != T_NONE) {                              // a tool is active: draw inside the selection
            if (in_sel(p)) { U.drag = Drag::Draw; shape_begin(p); }
            else U.drag = Drag::None;
            return;
        }
        if (in_sel(p)) {
            U.drag = Drag::Move; U.mx0 = U.x0; U.my0 = U.y0;
        } else {
            if (U.nshapes) { U.drag = Drag::None; return; }  // annotations belong to this selection: keep it
            U.drag = Drag::New; U.has_sel = false;
            U.x0 = U.x1 = p.x; U.y0 = U.y1 = p.y;
            if (U.bar) lv_obj_add_flag(U.bar, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (code == LV_EVENT_PRESSING) {
        if (U.drag == Drag::Draw) { shape_move(p); return; }
        if (U.drag == Drag::New) {
            U.x0 = p.x < U.p0.x ? p.x : U.p0.x; U.x1 = p.x < U.p0.x ? U.p0.x : p.x;
            U.y0 = p.y < U.p0.y ? p.y : U.p0.y; U.y1 = p.y < U.p0.y ? U.p0.y : p.y;
        } else if (U.drag == Drag::Move) {
            if (U.nshapes) return;                           // annotated: the selection stays put
            const int w = U.x1 - U.x0, h = U.y1 - U.y0;
            U.x0 = clampi(U.mx0 + p.x - U.p0.x, 0, U.w - w); U.x1 = U.x0 + w;
            U.y0 = clampi(U.my0 + p.y - U.p0.y, 0, U.h - h); U.y1 = U.y0 + h;
        } else return;
        redraw();
    } else if (code == LV_EVENT_RELEASED) {
        if (U.drag == Drag::Draw) { shape_end(); U.drag = Drag::None; return; }
        if (U.drag == Drag::None) return;
        if (U.drag == Drag::New && (U.x1 - U.x0 < kMinSel || U.y1 - U.y0 < kMinSel)) {
            U.x0 = 0; U.y0 = 0; U.x1 = U.w; U.y1 = U.h;      // a tap: the whole screen
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
    U = {};
    U.frame = freeze_logical(&U.w, &U.h);
    if (!U.frame) { nv_toast(NV_NOTE_WARN, TR("Screenshot non disponibile (memoria)", "Screenshot unavailable (memory)")); return; }
    U.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    U.dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    U.dsc.header.w = (uint32_t)U.w;
    U.dsc.header.h = (uint32_t)U.h;
    U.dsc.header.stride = (uint32_t)U.w * 2;
    U.dsc.data_size = (uint32_t)U.w * U.h * 2;
    U.dsc.data = (const uint8_t *)U.frame;

    const NvTheme *t = nv_theme_get();
    U.root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(U.root);
    lv_obj_set_size(U.root, U.w, U.h);
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
    // Whole screen, no overlay: straight to the clipboard (in the orientation on screen).
    int w = 0, h = 0;
    uint16_t *frame = freeze_logical(&w, &h);
    if (!frame) { nv_toast(NV_NOTE_WARN, TR("Screenshot non disponibile (memoria)", "Screenshot unavailable (memory)")); return; }
    Job *j = (Job *)calloc(1, sizeof *j);
    if (!j) { heap_caps_free(frame); return; }
    *j = {frame, w, h, Act::Copy};
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
        case 0x1D: if (ctrl) { undo(); return true; } break;                      // Ctrl+Z
        default: break;
    }
    return true;                                     // the overlay is modal: nothing leaks to the app
}
