// nv_gesture — NucleoOS centralized gesture standard (MANDATORY for system + apps).
//
// THE STANDARD
// ------------
// System gestures activate ONLY when the touch STARTS on a screen-edge strip (bezel swipe,
// Android-style). A swipe that begins inside content is content interaction (scroll, drag,
// app gesture) and must NEVER trigger a system action.
//
//   TOP edge    swipe down -> open notification shade; swipe up -> close it
//   LEFT edge   swipe right -> back (in-app only; strip disabled on the launcher)
//   BOTTOM/RIGHT reserved (no strip created until a callback is assigned)
//
// RULES FOR APPS (enforced, not advisory):
//  1. Apps NEVER attach raw LV_EVENT_GESTURE handlers to the screen or their plane.
//     For content gestures (e.g. swipe-down closes a photo viewer) use nv_gesture_bind():
//     it consumes the gesture on that object (clears GESTURE_BUBBLE) so it can't leak out.
//  2. The SystemUI calls nv_gesture_isolate() on the app plane, so even a raw handler on a
//     default-flagged child stops bubbling at the plane boundary — an in-app swipe can never
//     reach the screen and close the app underneath the user.
//  3. Edge strips are kept top-most by the SystemUI via nv_gesture_raise() after every
//     z-order change (app open, shade open, IME init, UI rebuild).
//
// All calls must run on the LVGL thread (inside lvgl_port_lock or an LVGL event/timer).
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NV_GESTURE_EDGE_TOP = 0,   // horizontal strip along the top edge
    NV_GESTURE_EDGE_LEFT,      // vertical strip along the left edge (below the status bar)
    NV_GESTURE_EDGE_BOTTOM,    // horizontal strip along the bottom edge (reserved)
    NV_GESTURE_EDGE_RIGHT,     // vertical strip along the right edge (reserved)
    NV_GESTURE_EDGE_COUNT
} nv_gesture_edge_t;

// dir = LVGL gesture direction of the swipe (LV_DIR_TOP/BOTTOM/LEFT/RIGHT).
// The callback decides which directions it acts on; others are simply ignored (still consumed).
typedef void (*nv_gesture_edge_cb_t)(lv_dir_t dir, void *user);

// One-time setup on the active screen. Creates no strips by itself.
void nv_gesture_init(lv_obj_t *scr);

// Assign/replace the handler for an edge. Lazily creates the invisible strip on first assign;
// cb == NULL deletes the strip (so an unassigned edge never steals touches from content).
void nv_gesture_set_edge(nv_gesture_edge_t edge, nv_gesture_edge_cb_t cb, void *user);

// Temporarily show/hide an assigned edge strip (e.g. LEFT back strip only while an app is open).
void nv_gesture_set_edge_enabled(nv_gesture_edge_t edge, bool enabled);

// Re-raise all strips to the screen foreground. Call after any z-order change.
void nv_gesture_raise(void);

// Re-apply strip geometry from the current display resolution. Call after a display
// rotation (LV_HOR_RES/LV_VER_RES swap; the strips were sized at creation time).
void nv_gesture_relayout(void);

// APP CONTENT GESTURES — the only sanctioned way for an app to react to a swipe on its own
// content. Fires cb when a gesture in `dir` happens on `obj` (press started on obj/children).
// Marks obj CLICKABLE and clears GESTURE_BUBBLE on it so the gesture is consumed there.
typedef void (*nv_gesture_dir_cb_t)(void *user);
void nv_gesture_bind(lv_obj_t *obj, lv_dir_t dir, nv_gesture_dir_cb_t cb, void *user);

// Containment: clear GESTURE_BUBBLE on a plane so no descendant gesture can escape past it.
// The SystemUI applies this to every app plane; apps may also use it on sub-planes.
void nv_gesture_isolate(lv_obj_t *plane);

#ifdef __cplusplus
}
#endif
