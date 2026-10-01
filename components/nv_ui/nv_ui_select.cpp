// Desktop selection for lists and grids (nv_ui_select.h): click / Ctrl / Shift / double click,
// rubber-band drag with edge auto-scroll, and the system context-menu event.
#include "nv_ui_select.h"
#include "nv_hid_host.h"
#include "nv_theme.h"
#include "nv_mem_attr.h"
#include "misc/lv_event_private.h"

#include <stdlib.h>

namespace {

constexpr int32_t  kDragStart = 6;     // px the mouse moves before a press becomes a band
constexpr uint32_t kDoubleMs = 450;
constexpr int32_t  kEdge = 24, kEdgeStep = 14;

struct Sel {
    lv_obj_t *cont, *band;             // container being pressed, band rectangle (top layer)
    lv_point_t p0;                     // band start, in the container's content coordinates
    bool       pressing, banding, consumed, scroll_flags;
    lv_obj_t  *last; uint32_t last_ms; // previous plain click (double click)
    lv_obj_t  *anchor;                 // Shift+click range start
    uint32_t   ctx_code;
    lv_style_t st_hover, st_sel;
    bool       styles;
};
NV_PSRAM_BSS Sel G;

void item_ev(lv_event_t *e);

bool is_item(lv_obj_t *o) {
    const uint32_t n = lv_obj_get_event_count(o);
    for (uint32_t i = 0; i < n; i++)
        if (lv_event_dsc_get_cb(lv_obj_get_event_dsc(o, i)) == item_ev) return true;
    return false;
}

uint8_t mods(void) {
    uint8_t k[7] = {};
    return nv_hid_host_kbd_state(k) >= 0 ? k[0] : 0;
}
bool ctrl_held(void) { return mods() & 0x11; }

void changed(lv_obj_t *cont) { if (cont && lv_obj_is_valid(cont)) lv_obj_send_event(cont, LV_EVENT_VALUE_CHANGED, nullptr); }

void select_only(lv_obj_t *cont, lv_obj_t *keep) {
    const uint32_t n = lv_obj_get_child_count(cont);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(cont, i);
        if (c == keep) lv_obj_add_state(c, LV_STATE_CHECKED);
        else if (lv_obj_has_state(c, LV_STATE_CHECKED) && is_item(c)) lv_obj_remove_state(c, LV_STATE_CHECKED);
    }
}

lv_point_t pointer(void) {
    lv_point_t p = {0, 0};
    if (lv_indev_t *in = lv_indev_active()) lv_indev_get_point(in, &p);
    return p;
}

void styles_init(void) {
    const NvTheme *th = nv_theme_get();
    if (!G.styles) { lv_style_init(&G.st_hover); lv_style_init(&G.st_sel); G.styles = true; }
    lv_style_set_bg_color(&G.st_hover, th->surface2);
    lv_style_set_bg_opa(&G.st_hover, LV_OPA_COVER);
    lv_style_set_bg_color(&G.st_sel, lv_color_mix(th->accent, th->surface, 56));
    lv_style_set_bg_opa(&G.st_sel, LV_OPA_COVER);
    lv_style_set_border_color(&G.st_sel, th->accent);
    lv_style_set_border_width(&G.st_sel, 1);
    lv_style_set_border_side(&G.st_sel, LV_BORDER_SIDE_FULL);
}

// While a mouse button is held in a list it does not drag-scroll (that is the band's gesture);
// the wheel and the edge auto-scroll still move it.
void hold_scroll(lv_obj_t *cont, bool hold) {
    if (hold) {
        G.scroll_flags = lv_obj_has_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(cont, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN));
    } else if (lv_obj_is_valid(cont)) {
        lv_obj_add_flag(cont, LV_OBJ_FLAG_SCROLL_CHAIN);
        if (G.scroll_flags) lv_obj_add_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    }
}

void band_update(void) {
    lv_obj_t *c = G.cont;
    lv_area_t ca;
    lv_obj_get_coords(c, &ca);
    lv_point_t p = pointer();
    // Edge auto-scroll: hold the pointer near (or past) the top / bottom edge.
    int32_t dy = 0;
    if (p.y < ca.y1 + kEdge && lv_obj_get_scroll_top(c) > 0) dy = kEdgeStep;
    else if (p.y > ca.y2 - kEdge && lv_obj_get_scroll_bottom(c) > 0) dy = -kEdgeStep;
    if (dy) lv_obj_scroll_by(c, 0, dy, LV_ANIM_OFF);
    const int32_t sx = lv_obj_get_scroll_x(c), sy = lv_obj_get_scroll_y(c);
    // Band in screen coordinates, clipped to the container.
    lv_area_t b;
    b.x1 = LV_MAX(LV_MIN(ca.x1 + G.p0.x - sx, p.x), ca.x1);
    b.y1 = LV_MAX(LV_MIN(ca.y1 + G.p0.y - sy, p.y), ca.y1);
    b.x2 = LV_MIN(LV_MAX(ca.x1 + G.p0.x - sx, p.x), ca.x2);
    b.y2 = LV_MIN(LV_MAX(ca.y1 + G.p0.y - sy, p.y), ca.y2);
    lv_obj_set_pos(G.band, b.x1, b.y1);
    lv_obj_set_size(G.band, LV_MAX(1, b.x2 - b.x1 + 1), LV_MAX(1, b.y2 - b.y1 + 1));
    // The start corner may have scrolled out of view: hit-test against the unclipped rectangle.
    lv_area_t h = {LV_MIN(ca.x1 + G.p0.x - sx, p.x), LV_MIN(ca.y1 + G.p0.y - sy, p.y),
                   LV_MAX(ca.x1 + G.p0.x - sx, p.x), LV_MAX(ca.y1 + G.p0.y - sy, p.y)};
    bool any = false;
    const uint32_t n = lv_obj_get_child_count(c);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *it = lv_obj_get_child(c, i);
        if (!is_item(it)) continue;
        lv_area_t a;
        lv_obj_get_coords(it, &a);
        const bool on = (a.x1 <= h.x2 && a.x2 >= h.x1 && a.y1 <= h.y2 && a.y2 >= h.y1) || lv_obj_has_state(it, LV_STATE_USER_1);
        if (on != lv_obj_has_state(it, LV_STATE_CHECKED)) {
            if (on) lv_obj_add_state(it, LV_STATE_CHECKED);
            else lv_obj_remove_state(it, LV_STATE_CHECKED);
            any = true;
        }
    }
    if (any) changed(c);
}

void band_begin(void) {
    const NvTheme *th = nv_theme_get();
    G.banding = true;
    const bool add = ctrl_held();
    const uint32_t n = lv_obj_get_child_count(G.cont);
    for (uint32_t i = 0; i < n; i++) {   // Ctrl adds to the selection; otherwise it starts over
        lv_obj_t *it = lv_obj_get_child(G.cont, i);
        if (!is_item(it)) continue;
        if (add && lv_obj_has_state(it, LV_STATE_CHECKED)) lv_obj_add_state(it, LV_STATE_USER_1);
        else lv_obj_remove_state(it, (lv_state_t)(LV_STATE_USER_1 | LV_STATE_CHECKED));
    }
    G.band = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(G.band);
    lv_obj_remove_flag(G.band, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_add_flag(G.band, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_style_bg_color(G.band, th->accent, 0);
    lv_obj_set_style_bg_opa(G.band, LV_OPA_20, 0);          // plain blend: no layer needed
    lv_obj_set_style_border_color(G.band, th->accent, 0);
    lv_obj_set_style_border_width(G.band, 1, 0);
    band_update();
}

void press_end(void) {
    if (!G.pressing) return;
    G.pressing = false;
    if (G.band) { lv_obj_delete(G.band); G.band = nullptr; }
    if (G.banding) {
        G.consumed = true;            // the CLICKED that may follow is not a click
        const uint32_t n = lv_obj_get_child_count(G.cont);
        for (uint32_t i = 0; i < n; i++) lv_obj_remove_state(lv_obj_get_child(G.cont, i), LV_STATE_USER_1);
    }
    G.banding = false;
    hold_scroll(G.cont, false);
}

void pointer_ev(lv_obj_t *cont, lv_event_code_t code, bool on_bg) {
    switch (code) {
        case LV_EVENT_PRESSED: {
            G.consumed = false;
            if (!nv_sel_mouse()) return;
            press_end();
            G.cont = cont;
            G.pressing = true;
            G.banding = false;
            lv_area_t ca;
            lv_obj_get_coords(cont, &ca);
            const lv_point_t p = pointer();
            G.p0 = {p.x - ca.x1 + lv_obj_get_scroll_x(cont), p.y - ca.y1 + lv_obj_get_scroll_y(cont)};
            hold_scroll(cont, true);
            break;
        }
        case LV_EVENT_PRESSING: {
            if (!G.pressing || G.cont != cont) return;
            if (!G.banding) {
                lv_area_t ca;
                lv_obj_get_coords(cont, &ca);
                const lv_point_t p = pointer();
                const int32_t dx = p.x - (ca.x1 + G.p0.x - lv_obj_get_scroll_x(cont));
                const int32_t dy = p.y - (ca.y1 + G.p0.y - lv_obj_get_scroll_y(cont));
                if (abs(dx) + abs(dy) >= kDragStart) band_begin();
            } else {
                band_update();
            }
            break;
        }
        case LV_EVENT_RELEASED:
        case LV_EVENT_PRESS_LOST:
            if (!G.pressing || G.cont != cont) return;
            // A plain click on the empty area clears the selection (like a desktop).
            if (!G.banding && on_bg && code == LV_EVENT_RELEASED && !ctrl_held()) {
                select_only(cont, nullptr);
                G.anchor = G.last = nullptr;
                changed(cont);
            }
            press_end();
            break;
        default:
            break;
    }
}

void item_ev(lv_event_t *e) {
    lv_obj_t *it = lv_event_get_current_target_obj(e);
    if (lv_obj_t *c = lv_obj_get_parent(it)) pointer_ev(c, lv_event_get_code(e), false);
}
void cont_ev(lv_event_t *e) {
    lv_obj_t *c = lv_event_get_current_target_obj(e);
    if (lv_event_get_target_obj(e) == c) pointer_ev(c, lv_event_get_code(e), true);
}
void cont_delete_ev(lv_event_t *e) {
    lv_obj_t *c = lv_event_get_current_target_obj(e);
    if (G.cont == c) { if (G.band) { lv_obj_delete(G.band); G.band = nullptr; } G.pressing = G.banding = false; G.cont = nullptr; }
    G.anchor = G.last = nullptr;
}

}  // namespace

extern "C" {

bool nv_sel_mouse(void) {
    lv_indev_t *in = lv_indev_active();
    return in && in == (lv_indev_t *)nv_hid_host_mouse_indev();
}

void nv_sel_attach(lv_obj_t *cont) {
    if (!cont) return;
    styles_init();
    lv_obj_add_flag(cont, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cont, cont_ev, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(cont, cont_ev, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(cont, cont_ev, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(cont, cont_ev, LV_EVENT_PRESS_LOST, nullptr);
    lv_obj_add_event_cb(cont, cont_delete_ev, LV_EVENT_DELETE, nullptr);
}

void nv_sel_item(lv_obj_t *item) {
    if (!item) return;
    if (!G.styles) styles_init();
    lv_obj_add_style(item, &G.st_hover, LV_STATE_HOVERED);
    lv_obj_add_style(item, &G.st_sel, LV_STATE_CHECKED);
    lv_obj_add_event_cb(item, item_ev, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(item, item_ev, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(item, item_ev, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(item, item_ev, LV_EVENT_PRESS_LOST, nullptr);
}

bool nv_sel_click(lv_event_t *e) {
    lv_obj_t *it = lv_event_get_current_target_obj(e);
    lv_obj_t *cont = lv_obj_get_parent(it);
    if (G.consumed) { G.consumed = false; return true; }   // the end of a band drag
    if (!nv_sel_mouse() || !cont) return false;            // finger / keyboard: open
    const uint8_t m = mods();
    const uint32_t now = lv_tick_get();
    if (m & 0x11) {                                        // Ctrl: toggle
        if (lv_obj_has_state(it, LV_STATE_CHECKED)) lv_obj_remove_state(it, LV_STATE_CHECKED);
        else lv_obj_add_state(it, LV_STATE_CHECKED);
        G.anchor = it;
        G.last = nullptr;
    } else if ((m & 0x22) && G.anchor && lv_obj_is_valid(G.anchor) && lv_obj_get_parent(G.anchor) == cont) {
        const int32_t a = lv_obj_get_index(G.anchor), b = lv_obj_get_index(it);
        const int32_t lo = LV_MIN(a, b), hi = LV_MAX(a, b);
        const uint32_t n = lv_obj_get_child_count(cont);
        for (uint32_t i = 0; i < n; i++) {
            lv_obj_t *c = lv_obj_get_child(cont, i);
            if (!is_item(c)) continue;
            if ((int32_t)i >= lo && (int32_t)i <= hi) lv_obj_add_state(c, LV_STATE_CHECKED);
            else lv_obj_remove_state(c, LV_STATE_CHECKED);
        }
        G.last = nullptr;
    } else {
        if (it == G.last && now - G.last_ms < kDoubleMs) { G.last = nullptr; return false; }   // open
        select_only(cont, it);
        G.anchor = G.last = it;
        G.last_ms = now;
    }
    changed(cont);
    return true;
}

int nv_sel_count(lv_obj_t *cont) {
    int k = 0;
    const uint32_t n = cont ? lv_obj_get_child_count(cont) : 0;
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(cont, i);
        if (lv_obj_has_state(c, LV_STATE_CHECKED) && is_item(c)) k++;
    }
    return k;
}

lv_obj_t *nv_sel_nth(lv_obj_t *cont, int want) {
    const uint32_t n = cont ? lv_obj_get_child_count(cont) : 0;
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(cont, i);
        if (lv_obj_has_state(c, LV_STATE_CHECKED) && is_item(c) && want-- == 0) return c;
    }
    return nullptr;
}

void nv_sel_clear(lv_obj_t *cont) {
    if (!cont) return;
    select_only(cont, nullptr);
    changed(cont);
}

void nv_sel_all(lv_obj_t *cont) {
    const uint32_t n = cont ? lv_obj_get_child_count(cont) : 0;
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(cont, i);
        if (is_item(c)) lv_obj_add_state(c, LV_STATE_CHECKED);
    }
    if (cont) changed(cont);
}

uint32_t nv_ui_event_context(void) {
    if (!G.ctx_code) G.ctx_code = lv_event_register_id();
    return G.ctx_code;
}

bool nv_sel_context_at(lv_obj_t *hit, lv_point_t p) {
    if (!G.ctx_code) return false;
    for (lv_obj_t *o = hit; o; o = lv_obj_get_parent(o)) {
        const uint32_t n = lv_obj_get_event_count(o);
        bool has = false;
        for (uint32_t i = 0; i < n && !has; i++) {
            lv_event_dsc_t *d = lv_obj_get_event_dsc(o, i);
            has = d && (d->filter & ~(uint32_t)LV_EVENT_PREPROCESS) == G.ctx_code;
        }
        if (!has) continue;
        // Right click on an unselected item: it becomes the selection (desktop rule).
        if (is_item(o) && !lv_obj_has_state(o, LV_STATE_CHECKED)) {
            lv_obj_t *cont = lv_obj_get_parent(o);
            select_only(cont, o);
            G.anchor = o;
            changed(cont);
        }
        lv_obj_send_event(o, (lv_event_code_t)G.ctx_code, &p);
        return true;
    }
    return false;
}

}  // extern "C"
