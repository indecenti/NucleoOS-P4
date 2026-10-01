// nv_ui_focus — keyboard navigation for the whole UI (USB / Bluetooth keyboards).
//
// Nothing to register: on every navigation key the focus engine finds the topmost visible layer
// (lock screen, Recents, shade, search, folder, the open app, the launcher...), collects every
// visible, enabled, clickable object in it that reacts to a click / value change (plus LVGL's
// focusable widgets: button, switch, slider, checkbox, dropdown, textarea, roller...), and moves a
// visible focus ring between them:
//
//   Tab / Shift+Tab   next / previous in tree order
//   arrows            nearest object in that direction (sliders take Left/Right, rollers and open
//                     dropdowns Up/Down)
//   Enter / Space     click the focused object
//   Esc               back (closes shade, Recents, search, folder, sub-page, then the app)
//
// Global shortcuts (nv_ui.cpp): Win = home, Alt+Tab = Recents (let go of Alt to open the pick),
// Alt+F4 = close app, Win+E Files, Win+I Settings, Win+L lock, Win+D home, Ctrl+Alt+Del System
// Monitor, PrtSc screenshot. Text fields keep their keys (typing, cursor, Enter); Tab leaves them.
//
// So an app gets keyboard support for free as long as its controls are clickable objects with a
// CLICKED / VALUE_CHANGED handler. The calls below only refine that. The rules every app and
// system screen follows: docs/ENGINEERING_RULES.md section 11.
//
// Also: Menu key / Shift+F10 = long press on the focused control (context actions), PgUp / PgDn /
// Home / End scroll, arrows scroll a page that has nothing to focus (logs, previews, lists of
// plain rows), a slider moves 5 % per arrow as a short drag (its "on release" handlers run), Esc
// in a layer with an in-app scrim closes that scrim first, and a screen rebuilt under the focus
// gets it back on the control now at the same place.
//
// LVGL-thread only. Uses LV_OBJ_FLAG_USER_1..4 (reserved: nothing else may use them).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NV_FOCUS_FLAG_SKIP    LV_OBJ_FLAG_USER_1   // object + subtree: never focused, never a layer
#define NV_FOCUS_FLAG_STYLED  LV_OBJ_FLAG_USER_2   // internal: focus ring style attached
#define NV_FOCUS_FLAG_PREFER  LV_OBJ_FLAG_USER_3   // first focus when its layer is entered
#define NV_FOCUS_FLAG_INCLUDE LV_OBJ_FLAG_USER_4   // focusable whatever its size / handlers

// Extra navigation keys for nv_focus_handle (outside LVGL's LV_KEY_* range).
#define NV_FOCUS_KEY_PGUP 0x0F0001u
#define NV_FOCUS_KEY_PGDN 0x0F0002u

// Keep `obj` and everything inside it out of keyboard navigation (decorative click catchers,
// drag surfaces, backgrounds that only dismiss).
void nv_focus_skip(lv_obj_t *obj);

// Make `obj` the first focus of its layer (the primary action / first field). Also makes a plain
// object focusable even without a click handler (it must still be clickable). Set it again on the
// equivalent control when the screen rebuilds.
void nv_focus_prefer(lv_obj_t *obj);

// Force `obj` into navigation even though the engine would not pick it (clickable object bigger
// than 60 % of the screen, or whose action lives in a handler on an ancestor). Not a first focus.
void nv_focus_include(lv_obj_t *obj);

// Per-app key handler: called for every key press (and auto-repeat) while the app is in front and
// no text field is focused, BEFORE navigation. `key` is an LV_KEY_* for Enter/Esc/arrows/Tab/
// Backspace/Delete/Home/End, else the Unicode code point typed under the active layout (0 = none).
// `usage` / `mods` are the raw HID usage and boot modifier byte. Return true to consume the key.
// Cleared automatically when the app closes or rebuilds. Use it for app shortcuts (calculator
// digits, media keys, game-like screens); never to replace Tab / Esc handling.
typedef bool (*nv_ui_key_cb)(uint32_t key, uint8_t usage, uint8_t mods);
void nv_ui_set_key_handler(nv_ui_key_cb cb);

// ---- shell side (nv_ui.cpp) ----
void nv_focus_init(void);
// One navigation key: LV_KEY_NEXT / PREV / UP / DOWN / LEFT / RIGHT / ENTER / HOME / END or
// NV_FOCUS_KEY_PGUP / PGDN. False when there is nothing to focus or scroll.
bool nv_focus_handle(uint32_t lv_key);
// Esc, before the shell's back: closes an open dropdown or the in-app scrim (modal) on top. False
// when there is none (the shell then goes back).
bool nv_focus_escape(void);
// Long press on the focused control (context actions). False without a focus.
bool nv_focus_long_press(void);
// Give `obj` the keyboard focus now (ring shown; a text field binds the IME, which keeps the
// on-screen keyboard down while a physical keyboard is connected).
void nv_focus_set(lv_obj_t *obj);
// Drop the focus ring (a touch / mouse press took over).
void nv_focus_clear(void);
// The object with the keyboard focus now, or NULL.
lv_obj_t *nv_focus_current(void);

#ifdef __cplusplus
}
#endif
