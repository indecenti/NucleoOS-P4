// dmd_ui.h — the DMD app's own widgets, shared by its settings panel (dmd_panel.cpp) and the composition
// screen (dmd_comp.cpp): one look for both (black, cyan accent, dark cards). Plain LVGL, no system kit, so the
// simulator (tests/host/sim_dmd) builds them as they are. LVGL thread.
#pragma once

#include "dmd_core.h"
#include "lvgl.h"
#include "nv_fonts.h"

#if __has_include("nv_pins.h")
#include "nv_pins.h"
#endif
#ifndef NV_LCD_H_RES   // the host simulator builds without the board's headers: the same panel
#define NV_LCD_H_RES 1024
#define NV_LCD_V_RES 600
#endif

// ---- from dmd_app.cpp, for the composition screen's previews (LVGL thread). The previews follow the device:
// the fit the board really shows for fit `fit` with this rotation, a DMD of w x h, and the pictures it has
// (full: the fit's own picture with its window; art: the backglass for the backbox): missing pictures fall
// back as on the cabinet, a rotated screen shows the classic DMD for the layouts made of pictures.
int dmd_app_fit_shown(int fit, bool full, bool art, int w, int h);
// The geometry of fit for a w x h DMD (the composition's own snapshot, not the size on screen now).
bool dmd_app_fit_geom_wh(int fit, int dot, const dmd_rect_t *win, int w, int h, dmd_geom_t *g);
// The backbox: how tall the backglass above the DMD is (the DMD's top row), for a w x h DMD and those dots.
int dmd_app_art_h(int w, int h, int dot);
// The rows the board rebuilds for table gid's h-row DMD (*n < h: a 128x16 DMD inside 128x32). Only the
// running table's lit rows are known: any other table gets all its rows.
void dmd_app_rebuild_rows(int gid, int h, int *r0, int *n);

namespace dmdui {

constexpr int kScrW = NV_LCD_H_RES, kScrH = NV_LCD_V_RES;   // the physical panel

constexpr uint32_t kAccent = 0x2FE0FF, kInk = 0xFFFFFF, kDim = 0x7F929E, kCard = 0x0C1418, kEdge = 0x26343C,
                   kOn = 0x0E5A6C, kBg = 0x05090B;

inline lv_color_t hx(uint32_t c) { return lv_color_hex(c); }

inline lv_obj_t *box(lv_obj_t *parent) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

inline lv_obj_t *label(lv_obj_t *parent, const lv_font_t *f, uint32_t c, const char *t, int x, int y) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, hx(c), 0);
    lv_label_set_text(l, t);
    lv_obj_set_pos(l, x, y);
    return l;
}

// A button: w > 0 fixed (a long name gets the small font), w = 0 as wide as its name. Placed at x, y unless
// the parent lays it out (flex).
inline lv_obj_t *pill(lv_obj_t *parent, const char *t, int x, int y, int w, lv_event_cb_t cb, void *user) {
    lv_obj_t *b = box(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w > 0 ? w : LV_SIZE_CONTENT, 40);
    if (w <= 0) lv_obj_set_style_pad_hor(b, 18, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, hx(kCard), 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, hx(kEdge), 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &nv_font_20, 0);
    lv_obj_set_style_text_color(l, hx(kInk), 0);
    lv_label_set_text(l, t);
    if (w > 0) {
        lv_obj_update_layout(l);
        if (lv_obj_get_width(l) > w - 16) lv_obj_set_style_text_font(l, &nv_font_14, 0);   // a long name: smaller
    }
    lv_obj_set_width(l, LV_SIZE_CONTENT);
    lv_obj_center(l);
    return b;
}

// The chosen one of a row of pills: filled and outlined in the accent.
inline void pill_mark(lv_obj_t *p, bool on) {
    if (!p) return;
    lv_obj_set_style_bg_color(p, hx(on ? kOn : kCard), 0);
    lv_obj_set_style_border_color(p, hx(on ? kAccent : kEdge), 0);
}

inline void pills_mark(lv_obj_t **p, int n, int sel) {
    for (int i = 0; i < n; i++) pill_mark(p[i], i == sel);
}

// A pill that cannot be used now: no clicks (the focus engine skips it too), its name dimmed. No opacity:
// an opa layer hangs the P4's software renderer.
inline void pill_enable(lv_obj_t *p, bool on) {
    if (!p) return;
    if (on) lv_obj_remove_state(p, LV_STATE_DISABLED);
    else lv_obj_add_state(p, LV_STATE_DISABLED);
    if (lv_obj_get_child_count(p)) lv_obj_set_style_text_color(lv_obj_get_child(p, 0), hx(on ? kInk : kDim), 0);
}

}  // namespace dmdui
