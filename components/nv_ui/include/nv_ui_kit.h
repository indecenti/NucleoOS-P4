// nv_ui_kit — reusable SystemUI widgets shared by apps (consistent NucleoOS look).
#pragma once
#include "lvgl.h"
#include "nv_ime.h"    // nv_ime_type_t / nv_ime_return_t for the textarea factory
#include "nv_ui_scale.h"  // NV_RAD_*, NV_SP_*, NV_TOUCH_MIN for apps

#ifdef __cplusplus
extern "C" {
#endif

// A full-height scrollable vertical column (padding + row gap). Use as an app's root.
lv_obj_t *nv_kit_scroll_column(lv_obj_t *parent);

// A themed button with a consistent look, a >=44px touch target, rounded corners, and
// layer-free pressed feedback. `primary` fills it with the brand color (on_primary text);
// otherwise it is a neutral control (surface3). Returns the button; the caption label is added
// for you. Use this instead of hand-rolling lv_button_create in apps.
lv_obj_t *nv_kit_button(lv_obj_t *parent, const char *label, bool primary);

// A muted full-width info label.
lv_obj_t *nv_kit_info(lv_obj_t *parent);

// A settings row card "[label ............ (control)]" — add the control to the returned card.
lv_obj_t *nv_kit_row(lv_obj_t *parent, const char *label);

// Labelled slider row; returns the slider (attach your own VALUE_CHANGED cb).
lv_obj_t *nv_kit_slider_row(lv_obj_t *col, const char *name, int value, int lo, int hi,
                            lv_event_cb_t cb);

// Labelled switch row; returns the switch (attach your own VALUE_CHANGED cb).
lv_obj_t *nv_kit_switch_row(lv_obj_t *col, const char *name, bool on, lv_event_cb_t cb);

// A themed textarea wired to the shared IME (auto on-screen keyboard on tap). Sets the
// placeholder and single/multi-line mode; the caller may further configure password/
// accepted-chars on the returned object before use. Binds as NV_IME_TEXT / NV_IME_RET_DEFAULT.
lv_obj_t *nv_kit_textarea(lv_obj_t *parent, const char *placeholder, bool one_line);

// Same, but binds the IME with an input class and return-key action. Use for email/URL/number/
// PIN/password fields and for forms where the return key should advance (NV_IME_RET_NEXT) or
// submit (NV_IME_RET_GO/SEARCH/SEND). The class already implies one-line for
// email/URL/number/PIN/password, so `one_line` there is moot.
lv_obj_t *nv_kit_textarea_ex(lv_obj_t *parent, const char *placeholder, bool one_line,
                             nv_ime_type_t type, nv_ime_return_t ret);

#ifdef __cplusplus
}
#endif
