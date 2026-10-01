// nv_ui_focus — keyboard focus engine. See nv_ui_focus.h.
//
// Nearly stateless by design: every key re-derives the layer and its focusable objects from the
// live object tree, so apps, overlays and rebuilt screens need no registration and nothing can go
// stale. A key press is rare next to a frame, so walking a few hundred objects per key is cheap.
// The only memory kept is where the focus was, so a screen that rebuilds itself under the focus
// (tabs, lists, confirm buttons) gets it back on the control that took the old one's place.
#include "nv_ui_focus.h"

#include "nv_ime.h"
#include "nv_theme.h"
#include "nv_mem_attr.h"   // NV_PSRAM_BSS: key-rate data, internal SRAM is at its budget

#include "lvgl.h"
#include "lvgl_private.h"   // lv_event_dsc_t::filter (which events a handler listens to)

#include <stdlib.h>

namespace {

lv_group_t  *s_group = nullptr;   // one group, re-synced to the current layer on every key
lv_indev_t  *s_kp = nullptr;      // keypad indev: Enter goes through LVGL's own click path
lv_obj_t    *s_ring_obj = nullptr; // object showing the focus ring (FOCUS_KEY state)
NV_PSRAM_BSS lv_style_t s_ring;
bool         s_ring_init = false;

// Where the focus was when its object disappeared (a rebuild): the next key resumes next to it.
lv_obj_t    *s_last_layer = nullptr;
lv_point_t   s_last_pt = {0, 0};
bool         s_lost = false;

// Keypad feed: key events queued by nv_focus_handle, read back by LVGL right away.
struct KpEv { uint32_t key; bool pressed; };
NV_PSRAM_BSS KpEv s_kpq[8];
unsigned s_kpq_r = 0, s_kpq_w = 0;

void kp_read_cb(lv_indev_t *, lv_indev_data_t *data) {
    static uint32_t last = 0;
    if (s_kpq_r == s_kpq_w) {                   // idle: stay released on the last key
        data->key = last;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    const KpEv ev = s_kpq[s_kpq_r++ % 8];
    last = ev.key;
    data->key = ev.key;
    data->state = ev.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->continue_reading = s_kpq_r != s_kpq_w;
}

void kp_send(uint32_t key) {
    if (s_kpq_w - s_kpq_r > 6) return;          // full: a key is lost, never a stuck press
    s_kpq[s_kpq_w++ % 8] = {key, true};
    s_kpq[s_kpq_w++ % 8] = {key, false};
    lv_indev_read(s_kp);
}

// ---- what can take focus

bool hidden_or_skipped(lv_obj_t *o) {
    return lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) || lv_obj_has_flag(o, NV_FOCUS_FLAG_SKIP);
}

bool is_action_code(uint32_t filter) {
    switch ((lv_event_code_t)(filter & ~(uint32_t)LV_EVENT_PREPROCESS)) {
        case LV_EVENT_ALL:
        case LV_EVENT_PRESSED:
        case LV_EVENT_CLICKED:
        case LV_EVENT_SHORT_CLICKED:
        case LV_EVENT_SINGLE_CLICKED:
        case LV_EVENT_RELEASED:
        case LV_EVENT_LONG_PRESSED:
        case LV_EVENT_VALUE_CHANGED:
            return true;
        default:
            return false;
    }
}

// Does a handler on `o` act on a click / press / value change?
bool has_action_handler(lv_obj_t *o) {
    const uint32_t n = lv_obj_get_event_count(o);
    for (uint32_t i = 0; i < n; i++) {
        lv_event_dsc_t *d = lv_obj_get_event_dsc(o, i);
        if (d && is_action_code(d->filter)) return true;
    }
    return false;
}

// Big enough to be a background, a scrim or a page plane rather than a control.
bool backdrop_sized(lv_obj_t *o) {
    const int64_t scr = (int64_t)LV_HOR_RES * LV_VER_RES;
    return (int64_t)lv_obj_get_width(o) * lv_obj_get_height(o) * 10 > scr * 6;
}

bool focusable(lv_obj_t *o) {
    if (!lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE) || lv_obj_has_state(o, LV_STATE_DISABLED))
        return false;
    if (lv_obj_has_flag(o, NV_FOCUS_FLAG_PREFER) || lv_obj_has_flag(o, NV_FOCUS_FLAG_INCLUDE))
        return true;
    if (lv_obj_get_width(o) < 4 || lv_obj_get_height(o) < 4) return false;
    // LVGL's own input widgets always count (a full-page text editor is still a text field);
    // plain objects only when they are control-sized.
    if (lv_obj_is_group_def(o)) return true;
    return !backdrop_sized(o) && has_action_handler(o);
}

// A modal inside a layer: a clickable, handled object covering its parent (an in-app scrim that
// closes on tap). Everything painted before it is underneath and out of reach.
bool is_barrier(lv_obj_t *o) {
    lv_obj_t *p = lv_obj_get_parent(o);
    if (!p || !lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE) || !has_action_handler(o)) return false;
    return lv_obj_get_width(o) >= lv_obj_get_width(p) && lv_obj_get_height(o) >= lv_obj_get_height(p);
}

// Depth-first, tree order = reading order for flex / grid layouts.
struct List { lv_obj_t **v; uint32_t n, cap; lv_obj_t *barrier; };

void list_push(List &l, lv_obj_t *o) {
    if (l.n == l.cap) {
        const uint32_t cap = l.cap ? l.cap * 2 : 64;
        lv_obj_t **nv = (lv_obj_t **)lv_realloc(l.v, cap * sizeof *nv);   // LVGL pool (PSRAM)
        if (!nv) return;
        l.v = nv;
        l.cap = cap;
    }
    l.v[l.n++] = o;
}

void harvest(lv_obj_t *o, List &l, bool is_root) {
    if (hidden_or_skipped(o)) return;
    if (!is_root && is_barrier(o)) { l.n = 0; l.barrier = o; }
    if (focusable(o)) list_push(l, o);
    const uint32_t n = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < n; i++) harvest(lv_obj_get_child(o, (int32_t)i), l, false);
}

bool has_focusable(lv_obj_t *o) {
    if (hidden_or_skipped(o)) return false;
    if (focusable(o) || lv_obj_has_flag(o, NV_FOCUS_FLAG_PREFER)) return true;
    const uint32_t n = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < n; i++)
        if (has_focusable(lv_obj_get_child(o, (int32_t)i))) return true;
    return false;
}

// The layer keys act on: the topmost visible child of the top layer, else of the screen, that has
// anything focusable (the open app plane, an overlay, the launcher...). The on-screen keyboard
// never counts: while it is up, keys belong to the text field.
lv_obj_t *find_layer(void) {
    lv_obj_t *roots[2] = {lv_layer_top(), lv_screen_active()};
    lv_obj_t *kb = nv_ime_keyboard_obj();
    for (lv_obj_t *root : roots) {
        if (!root) continue;
        for (int32_t i = (int32_t)lv_obj_get_child_count(root) - 1; i >= 0; i--) {
            lv_obj_t *c = lv_obj_get_child(root, i);
            if (c == kb || hidden_or_skipped(c)) continue;
            if (has_focusable(c)) return c;
        }
    }
    return nullptr;
}

// ---- focus ring

void ring_style_refresh(void) {
    if (!s_ring_init) {
        lv_style_init(&s_ring);
        lv_style_set_outline_width(&s_ring, 3);
        lv_style_set_outline_pad(&s_ring, 2);
        lv_style_set_outline_opa(&s_ring, LV_OPA_COVER);
        s_ring_init = true;
    }
    static lv_color_t last = {};
    static bool set = false;
    const lv_color_t c = nv_theme_get()->accent;       // follows theme / accent changes
    if (!set || !lv_color_eq(last, c)) {
        lv_style_set_outline_color(&s_ring, c);
        lv_obj_report_style_change(&s_ring);
        last = c;
        set = true;
    }
}

lv_point_t center_of(lv_obj_t *o) {
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    return {(a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2};
}

void ring_obj_deleted(lv_event_t *e) {
    if (lv_event_get_target_obj(e) != s_ring_obj) return;
    s_last_pt = center_of(s_ring_obj);          // still valid during DELETE
    s_lost = true;
    s_ring_obj = nullptr;
}

void ring_attach(lv_obj_t *o) {
    if (lv_obj_has_flag(o, NV_FOCUS_FLAG_STYLED)) return;
    lv_obj_add_style(o, &s_ring, LV_STATE_FOCUS_KEY);
    lv_obj_add_event_cb(o, ring_obj_deleted, LV_EVENT_DELETE, nullptr);
    lv_obj_add_flag(o, NV_FOCUS_FLAG_STYLED);
}

void focus_obj(lv_obj_t *o, lv_obj_t *layer) {
    if (s_ring_obj && s_ring_obj != o) lv_obj_remove_state(s_ring_obj, LV_STATE_FOCUS_KEY);
    ring_attach(o);
    lv_obj_add_flag(o, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_group_focus_obj(o);                      // FOCUSED event (text fields bind the IME) + scroll
    lv_obj_add_state(o, LV_STATE_FOCUS_KEY);
    s_ring_obj = o;
    s_last_layer = layer;
    s_lost = false;
}

// ---- geometry

bool on_screen(lv_obj_t *o) {
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    return a.x2 >= 0 && a.y2 >= 0 && a.x1 < LV_HOR_RES && a.y1 < LV_VER_RES;
}

// Nearest object in direction `key` from area `c`: distance along the axis plus twice the sideways
// offset, which is zero when the two overlap across the axis (rows of a list, cells of a grid).
lv_obj_t *spatial_pass(const List &l, lv_obj_t *cur, uint32_t key, bool visible_only) {
    lv_area_t c;
    lv_obj_get_coords(cur, &c);
    const int32_t cx = (c.x1 + c.x2) / 2, cy = (c.y1 + c.y2) / 2;
    lv_obj_t *best = nullptr;
    int64_t best_cost = INT64_MAX;
    for (uint32_t i = 0; i < l.n; i++) {
        lv_obj_t *o = l.v[i];
        if (o == cur || (visible_only && !on_screen(o))) continue;
        lv_area_t a;
        lv_obj_get_coords(o, &a);
        const int32_t ox = (a.x1 + a.x2) / 2, oy = (a.y1 + a.y2) / 2;
        int32_t along, side;
        bool overlap;
        switch (key) {
            case LV_KEY_DOWN:  along = a.y1 - c.y2; if (oy <= cy) continue; overlap = a.x1 <= c.x2 && a.x2 >= c.x1; side = ox - cx; break;
            case LV_KEY_UP:    along = c.y1 - a.y2; if (oy >= cy) continue; overlap = a.x1 <= c.x2 && a.x2 >= c.x1; side = ox - cx; break;
            case LV_KEY_RIGHT: along = a.x1 - c.x2; if (ox <= cx) continue; overlap = a.y1 <= c.y2 && a.y2 >= c.y1; side = oy - cy; break;
            default:           along = c.x1 - a.x2; if (ox >= cx) continue; overlap = a.y1 <= c.y2 && a.y2 >= c.y1; side = oy - cy; break;
        }
        if (along < 0) along = 0;
        const int64_t cost = (int64_t)along + (overlap ? 0 : 2 * (int64_t)LV_ABS(side));
        if (cost < best_cost) { best_cost = cost; best = o; }
    }
    return best;
}

// Visible targets first (a launcher page off to the side is not "below"); only when nothing on
// screen lies that way, one that a scroll will bring in.
lv_obj_t *spatial(const List &l, lv_obj_t *cur, uint32_t key) {
    lv_obj_t *o = spatial_pass(l, cur, key, true);
    return o ? o : spatial_pass(l, cur, key, false);
}

lv_obj_t *nearest_to(const List &l, lv_point_t p) {
    lv_obj_t *best = nullptr;
    int64_t best_d = INT64_MAX;
    for (uint32_t i = 0; i < l.n; i++) {
        const lv_point_t c = center_of(l.v[i]);
        const int64_t dx = c.x - p.x, dy = c.y - p.y, d = dx * dx + dy * dy;
        if (d < best_d) { best_d = d; best = l.v[i]; }
    }
    return best;
}

bool is_text_field(lv_obj_t *o) { return lv_obj_check_type(o, &lv_textarea_class); }

// Where focus lands when a layer is entered: the app's preferred control, else the first
// visible control that is not a text field (a field would swallow the arrows), else anything.
lv_obj_t *first_focus(const List &l) {
    for (uint32_t i = 0; i < l.n; i++) if (lv_obj_has_flag(l.v[i], NV_FOCUS_FLAG_PREFER)) return l.v[i];
    for (uint32_t i = 0; i < l.n; i++) if (on_screen(l.v[i]) && !is_text_field(l.v[i])) return l.v[i];
    for (uint32_t i = 0; i < l.n; i++) if (on_screen(l.v[i])) return l.v[i];
    return l.n ? l.v[0] : nullptr;
}

// ---- sliders: a key is a small drag (PRESSED, move, RELEASED), so every "save / seek on
// release" handler written for touch works for the keyboard too. Steps are 5 % of the range.
void slider_step(lv_obj_t *o, uint32_t key) {
    const int32_t mn = lv_slider_get_min_value(o), mx = lv_slider_get_max_value(o);
    int32_t step = (mx - mn) / 20;
    if (step < 1) step = 1;
    const bool up = key == LV_KEY_RIGHT || key == LV_KEY_UP;
    int32_t v = lv_slider_get_value(o) + (up ? step : -step);
    if (v < mn) v = mn;
    if (v > mx) v = mx;
    lv_obj_send_event(o, LV_EVENT_PRESSED, nullptr);
    lv_slider_set_value(o, v, LV_ANIM_OFF);
    lv_obj_send_event(o, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_send_event(o, LV_EVENT_RELEASED, nullptr);
}

// Keys a widget uses itself instead of moving focus.
bool widget_takes(lv_obj_t *o, uint32_t key) {
    const bool lr = key == LV_KEY_LEFT || key == LV_KEY_RIGHT;
    const bool ud = key == LV_KEY_UP || key == LV_KEY_DOWN;
    if (lv_obj_check_type(o, &lv_slider_class)) return lr;
#if LV_USE_ARC
    if (lv_obj_check_type(o, &lv_arc_class)) return lr;
#endif
#if LV_USE_ROLLER
    if (lv_obj_check_type(o, &lv_roller_class)) return ud;
#endif
#if LV_USE_DROPDOWN
    if (lv_obj_check_type(o, &lv_dropdown_class)) return ud && lv_dropdown_is_open(o);
#endif
    return false;
}

// ---- scrolling when there is nothing to move to (a log, a text preview, a process list)

lv_obj_t *scroll_target_in(lv_obj_t *o) {
    if (hidden_or_skipped(o)) return nullptr;
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_SCROLLABLE) &&
        (lv_obj_get_scroll_top(o) > 0 || lv_obj_get_scroll_bottom(o) > 0)) return o;
    const uint32_t n = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < n; i++)
        if (lv_obj_t *t = scroll_target_in(lv_obj_get_child(o, (int32_t)i))) return t;
    return nullptr;
}

lv_obj_t *scroll_target(lv_obj_t *from, lv_obj_t *layer) {
    for (lv_obj_t *p = from; p && p != layer; p = lv_obj_get_parent(p))
        if (lv_obj_has_flag(p, LV_OBJ_FLAG_SCROLLABLE) &&
            (lv_obj_get_scroll_top(p) > 0 || lv_obj_get_scroll_bottom(p) > 0)) return p;
    return layer ? scroll_target_in(layer) : nullptr;
}

bool scroll_by_key(lv_obj_t *t, uint32_t key) {
    if (!t) return false;
    const int32_t page = lv_obj_get_height(t) * 3 / 4, line = lv_obj_get_height(t) / 6;
    switch (key) {
        case LV_KEY_UP:   lv_obj_scroll_by(t, 0, line, LV_ANIM_ON); return true;
        case LV_KEY_DOWN: lv_obj_scroll_by(t, 0, -line, LV_ANIM_ON); return true;
        case NV_FOCUS_KEY_PGUP: lv_obj_scroll_by(t, 0, page, LV_ANIM_ON); return true;
        case NV_FOCUS_KEY_PGDN: lv_obj_scroll_by(t, 0, -page, LV_ANIM_ON); return true;
        case LV_KEY_HOME: lv_obj_scroll_to_y(t, 0, LV_ANIM_ON); return true;
        case LV_KEY_END:  lv_obj_scroll_to_y(t, LV_COORD_MAX, LV_ANIM_ON); return true;
        default: return false;
    }
}

}  // namespace

void nv_focus_skip(lv_obj_t *obj)    { if (obj) lv_obj_add_flag(obj, NV_FOCUS_FLAG_SKIP); }
void nv_focus_prefer(lv_obj_t *obj)  { if (obj) lv_obj_add_flag(obj, NV_FOCUS_FLAG_PREFER); }
void nv_focus_include(lv_obj_t *obj) { if (obj) lv_obj_add_flag(obj, NV_FOCUS_FLAG_INCLUDE); }

void nv_focus_init(void) {
    if (s_group) return;
    s_group = lv_group_create();
    s_kp = lv_indev_create();
    lv_indev_set_type(s_kp, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(s_kp, kp_read_cb);
    lv_indev_set_group(s_kp, s_group);
    ring_style_refresh();
}

lv_obj_t *nv_focus_current(void) { return s_ring_obj; }

void nv_focus_set(lv_obj_t *obj) {
    if (!obj) return;
    if (!s_group) nv_focus_init();
    ring_style_refresh();
    if (lv_obj_get_group(obj) != s_group) lv_group_add_obj(s_group, obj);
    focus_obj(obj, find_layer());
}

void nv_focus_clear(void) {
    if (s_ring_obj) lv_obj_remove_state(s_ring_obj, LV_STATE_FOCUS_KEY);
    s_ring_obj = nullptr;
    s_lost = false;
}

bool nv_focus_escape(void) {
    lv_obj_t *cur = s_ring_obj;
#if LV_USE_DROPDOWN
    if (cur && lv_obj_check_type(cur, &lv_dropdown_class) && lv_dropdown_is_open(cur)) {
        lv_dropdown_close(cur);
        return true;
    }
#endif
    // An in-app modal (scrim covering its parent, dismiss on tap): Esc = tapping outside it.
    lv_obj_t *layer = find_layer();
    if (!layer) return false;
    List l = {};
    harvest(layer, l, true);
    lv_obj_t *barrier = l.barrier;
    lv_free(l.v);
    if (!barrier) return false;
    lv_obj_send_event(barrier, LV_EVENT_PRESSED, nullptr);
    lv_obj_send_event(barrier, LV_EVENT_RELEASED, nullptr);
    lv_obj_send_event(barrier, LV_EVENT_SHORT_CLICKED, nullptr);
    lv_obj_send_event(barrier, LV_EVENT_CLICKED, nullptr);
    return true;
}

bool nv_focus_long_press(void) {
    if (!s_ring_obj) return false;
    lv_obj_t *o = s_ring_obj;
    lv_obj_send_event(o, LV_EVENT_LONG_PRESSED, nullptr);
    // Close the "press" too: handlers that start a drag on long press (launcher tiles) end it on
    // RELEASED, and no finger will ever lift here.
    if (lv_obj_is_valid(o)) lv_obj_send_event(o, LV_EVENT_RELEASED, nullptr);
    return true;
}

bool nv_focus_handle(uint32_t key) {
    if (!s_group) nv_focus_init();
    lv_obj_t *layer = find_layer();
    if (!layer) return false;
    ring_style_refresh();

    List l = {};
    harvest(layer, l, true);
    if (!l.n) {                                 // nothing to focus: arrows / pages scroll it
        const bool ok = scroll_by_key(scroll_target(layer, layer), key);
        lv_free(l.v);
        return ok;
    }

    // Keep the group equal to this layer's controls (LVGL drops deleted objects by itself).
    for (int32_t i = (int32_t)lv_group_get_obj_count(s_group) - 1; i >= 0; i--) {
        lv_obj_t *o = lv_group_get_obj_by_index(s_group, (uint32_t)i);
        bool keep = false;
        for (uint32_t k = 0; k < l.n && !keep; k++) keep = l.v[k] == o;
        if (!keep && o != lv_group_get_focused(s_group)) lv_group_remove_obj(o);
    }
    for (uint32_t k = 0; k < l.n; k++)
        if (lv_obj_get_group(l.v[k]) != s_group) lv_group_add_obj(s_group, l.v[k]);

    // The current focus only counts if it is still one of this layer's controls. A text field
    // bound by a tap (or opened focused) counts as the focus too, so Tab leaves it.
    lv_obj_t *cur = s_ring_obj;
    if (!cur && nv_ime_bound()) cur = lv_keyboard_get_textarea(nv_ime_keyboard_obj());
    int32_t idx = -1;
    for (uint32_t k = 0; k < l.n; k++) if (l.v[k] == cur) { idx = (int32_t)k; break; }
    if (idx < 0) cur = nullptr;

    lv_obj_t *next = nullptr;
    if (!cur) {
        if (key == NV_FOCUS_KEY_PGUP || key == NV_FOCUS_KEY_PGDN || key == LV_KEY_HOME || key == LV_KEY_END) {
            const bool ok = scroll_by_key(scroll_target(layer, layer), key);
            lv_free(l.v);
            return ok;
        }
        // A rebuild under the focus: resume on what now sits where it was. Else the layer's
        // first focus. The first key only shows where the focus is.
        next = (s_lost && s_last_layer == layer) ? nearest_to(l, s_last_pt) : nullptr;
        if (!next) next = first_focus(l);
    } else {
        switch (key) {
            case LV_KEY_NEXT: next = l.v[(idx + 1) % (int32_t)l.n]; break;
            case LV_KEY_PREV: next = l.v[(idx + (int32_t)l.n - 1) % (int32_t)l.n]; break;
            case LV_KEY_ENTER:
                lv_group_focus_obj(cur);
                kp_send(LV_KEY_ENTER);          // PRESSED / RELEASED / CLICKED on the focused object
                break;
            case NV_FOCUS_KEY_PGUP: case NV_FOCUS_KEY_PGDN: case LV_KEY_HOME: case LV_KEY_END:
                scroll_by_key(scroll_target(cur, layer), key);
                break;
            default:
                if (widget_takes(cur, key)) {
                    if (lv_obj_check_type(cur, &lv_slider_class)) slider_step(cur, key);
                    else { lv_group_focus_obj(cur); kp_send(key); }
                } else {
                    next = spatial(l, cur, key);
                    // Nothing that way: scroll the list the focus is in (end of a long page).
                    if (!next && (key == LV_KEY_UP || key == LV_KEY_DOWN))
                        scroll_by_key(scroll_target(cur, layer), key);
                }
                break;
        }
    }
    if (next) focus_obj(next, layer);
    lv_free(l.v);
    return true;
}
