// nv_gesture — centralized gesture standard implementation. See include/nv_gesture.h for the
// contract. Strips are invisible CLICKABLE screen children with GESTURE_BUBBLE cleared: LVGL
// targets a gesture at the object where the PRESS STARTED, so a strip firing == the swipe
// really began on that screen edge. No press-point bookkeeping needed.

#include "nv_gesture.h"
#include "nv_log.h"

#include <cstdint>  // intptr_t (edge id <-> user_data packing)

namespace {

constexpr const char *TAG = "nv_gesture";

// Strip thickness. 24px keeps content touches safe while a bezel-origin finger still lands
// inside (the panel sits behind cover glass with a real bezel).
constexpr int kEdgePx   = 24;
// The LEFT strip starts below the status bar so status-bar taps/swipes keep their own handlers.
constexpr int kStatusH  = 34;   // mirror of nv_ui kStatusH (chrome geometry is fixed OS-wide)

lv_obj_t *s_scr = nullptr;
lv_obj_t *s_strip[NV_GESTURE_EDGE_COUNT] = {};
nv_gesture_edge_cb_t s_cb[NV_GESTURE_EDGE_COUNT] = {};
void *s_user[NV_GESTURE_EDGE_COUNT] = {};
bool s_enabled[NV_GESTURE_EDGE_COUNT] = {true, true, true, true};

void strip_gesture_cb(lv_event_t *e) {
    const auto edge = (nv_gesture_edge_t)(intptr_t)lv_event_get_user_data(e);
    lv_indev_t *ind = lv_indev_active();
    if (!ind || !s_cb[edge]) return;
    s_cb[edge](lv_indev_get_gesture_dir(ind), s_user[edge]);
}

void strip_apply_geometry(nv_gesture_edge_t edge, lv_obj_t *o) {
    switch (edge) {
        case NV_GESTURE_EDGE_TOP:
            lv_obj_set_size(o, LV_HOR_RES, kEdgePx);
            lv_obj_align(o, LV_ALIGN_TOP_MID, 0, 0);
            break;
        case NV_GESTURE_EDGE_BOTTOM:
            lv_obj_set_size(o, LV_HOR_RES, kEdgePx);
            lv_obj_align(o, LV_ALIGN_BOTTOM_MID, 0, 0);
            break;
        case NV_GESTURE_EDGE_LEFT:
            // End above the BOTTOM strip so the two never fight over the bottom-left corner:
            // the BOTTOM band owns swipe-up-home there; back is the left edge above it.
            lv_obj_set_size(o, kEdgePx, LV_VER_RES - kStatusH - kEdgePx);
            lv_obj_align(o, LV_ALIGN_TOP_LEFT, 0, kStatusH);
            break;
        default:  // NV_GESTURE_EDGE_RIGHT
            lv_obj_set_size(o, kEdgePx, LV_VER_RES - kStatusH);
            lv_obj_align(o, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
            break;
    }
}

lv_obj_t *make_strip(nv_gesture_edge_t edge) {
    lv_obj_t *o = lv_obj_create(s_scr);
    lv_obj_remove_style_all(o);
    strip_apply_geometry(edge, o);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);           // invisible
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);              // must be pressable to own the gesture
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_GESTURE_BUBBLE);      // consume, never bubble
    lv_obj_add_event_cb(o, strip_gesture_cb, LV_EVENT_GESTURE, (void *)(intptr_t)edge);
    return o;
}

// nv_gesture_bind() binding: small heap record freed with the object.
struct DirBinding {
    lv_dir_t dir;
    nv_gesture_dir_cb_t cb;
    void *user;
};

void bind_gesture_cb(lv_event_t *e) {
    auto *b = (DirBinding *)lv_event_get_user_data(e);
    lv_indev_t *ind = lv_indev_active();
    if (b && ind && lv_indev_get_gesture_dir(ind) == b->dir) b->cb(b->user);
}

void bind_delete_cb(lv_event_t *e) { lv_free(lv_event_get_user_data(e)); }

}  // namespace

extern "C" {

void nv_gesture_init(lv_obj_t *scr) {
    s_scr = scr;
    for (int i = 0; i < NV_GESTURE_EDGE_COUNT; i++) {
        s_strip[i] = nullptr;  // a screen rebuild deletes children; forget stale pointers
        s_enabled[i] = true;
    }
}

void nv_gesture_set_edge(nv_gesture_edge_t edge, nv_gesture_edge_cb_t cb, void *user) {
    if (!s_scr || edge >= NV_GESTURE_EDGE_COUNT) return;
    s_cb[edge] = cb;
    s_user[edge] = user;
    if (!cb) {  // unassigned edge must not steal touches: remove the strip entirely
        if (s_strip[edge]) { lv_obj_delete(s_strip[edge]); s_strip[edge] = nullptr; }
        return;
    }
    if (!s_strip[edge]) s_strip[edge] = make_strip(edge);
    if (s_enabled[edge]) lv_obj_remove_flag(s_strip[edge], LV_OBJ_FLAG_HIDDEN);
    else                 lv_obj_add_flag(s_strip[edge], LV_OBJ_FLAG_HIDDEN);
}

void nv_gesture_set_edge_enabled(nv_gesture_edge_t edge, bool enabled) {
    if (edge >= NV_GESTURE_EDGE_COUNT) return;
    s_enabled[edge] = enabled;
    if (!s_strip[edge]) return;
    if (enabled) lv_obj_remove_flag(s_strip[edge], LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag(s_strip[edge], LV_OBJ_FLAG_HIDDEN);
}

void nv_gesture_relayout(void) {
    // Re-derive every live strip's geometry from the CURRENT LV_HOR_RES/LV_VER_RES
    // (they change when the display rotates).
    for (int i = 0; i < NV_GESTURE_EDGE_COUNT; i++)
        if (s_strip[i]) strip_apply_geometry((nv_gesture_edge_t)i, s_strip[i]);
}

void nv_gesture_raise(void) {
    // Fixed order: LEFT/BOTTOM/RIGHT first, TOP last so the top strip wins the shared corners
    // (mirrors the old "top catcher above the back strip" invariant).
    static const nv_gesture_edge_t order[] = {NV_GESTURE_EDGE_LEFT, NV_GESTURE_EDGE_BOTTOM,
                                              NV_GESTURE_EDGE_RIGHT, NV_GESTURE_EDGE_TOP};
    for (nv_gesture_edge_t e : order)
        if (s_strip[e]) lv_obj_move_foreground(s_strip[e]);
}

void nv_gesture_bind(lv_obj_t *obj, lv_dir_t dir, nv_gesture_dir_cb_t cb, void *user) {
    if (!obj || !cb) return;
    auto *b = (DirBinding *)lv_malloc(sizeof(DirBinding));
    if (!b) { NV_LOGE(TAG, "bind alloc failed"); return; }
    b->dir = dir;
    b->cb = cb;
    b->user = user;
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE);  // consume here, never leak upward
    lv_obj_add_event_cb(obj, bind_gesture_cb, LV_EVENT_GESTURE, b);
    lv_obj_add_event_cb(obj, bind_delete_cb, LV_EVENT_DELETE, b);
}

void nv_gesture_isolate(lv_obj_t *plane) {
    if (plane) lv_obj_remove_flag(plane, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

}  // extern "C"
