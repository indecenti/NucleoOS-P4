// nv_ime — NucleoOS Anima shared on-screen keyboard (IME), a SystemUI-level service.
// One hidden lv_keyboard lives on the active screen, above the launcher/shade/app plane.
// Any textarea is wired via nv_ime_bind(): tapping it slides the keyboard up and binds it;
// tapping elsewhere, or the keyboard's checkmark/close, slides it back down. The IME is
// theme-aware (colored from nv_theme_get()) and language-aware (QWERTZ for DE, AZERTY for
// FR, else QWERTY) — SystemUI calls nv_ime_retheme()/nv_ime_relayout() on theme/lang change.
#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Per-field input class. Drives which key plane opens on focus and how the field is configured
// (password masking, accepted characters, single-line) — set once at bind time.
typedef enum {
    NV_IME_TEXT = 0,   // letters, auto-capitalize the first character
    NV_IME_EMAIL,      // one-line, no auto-capitalize
    NV_IME_URL,        // one-line, no auto-capitalize
    NV_IME_NUMBER,     // one-line numeric keypad (digits + . , - +)
    NV_IME_PIN,        // one-line numeric keypad, digits only, password-masked
    NV_IME_PASSWORD,   // letters, one-line, password-masked, no auto-capitalize
} nv_ime_type_t;

// What the keyboard's return/OK key does on this field.
typedef enum {
    NV_IME_RET_DEFAULT = 0, // dismiss the keyboard (multi-line fields still get \n via the ⏎ key)
    NV_IME_RET_DONE,        // dismiss the keyboard
    NV_IME_RET_NEXT,        // move focus to the next bound field in the same container; keep up
    NV_IME_RET_GO,          // fire the submit callback, then dismiss
    NV_IME_RET_SEARCH,      // fire the submit callback, then dismiss
    NV_IME_RET_SEND,        // fire the submit callback, then dismiss
} nv_ime_return_t;

// Fired when the return key acts on a GO/SEARCH/SEND field. `ta` is the active textarea.
typedef void (*nv_ime_submit_cb_t)(lv_obj_t *ta, void *user);

// Create the single shared keyboard (hidden, docked bottom, off-screen). Call once from
// nv_ui_start() AFTER the launcher/shade are built so the keyboard is top-most.
void nv_ime_init(lv_obj_t *screen);

// Wire a textarea so tapping it auto-shows the keyboard bound to it, and typing edits it.
// Equivalent to nv_ime_bind_ex(textarea, NV_IME_TEXT, NV_IME_RET_DEFAULT). Honors any extra
// password / one-line / accepted-chars configuration the caller sets on the textarea.
// Registers an LV_EVENT_DELETE guard so a deleted active field never dangles the keyboard.
void nv_ime_bind(lv_obj_t *textarea);

// Wire a textarea with an input class and return-key action. Configures the field (masking,
// accepted chars, single-line) from `type` and remembers both so the right key plane opens on
// focus and the return key behaves as `ret`. Safe to call once per field.
void nv_ime_bind_ex(lv_obj_t *textarea, nv_ime_type_t type, nv_ime_return_t ret);

// Register a callback invoked when a GO/SEARCH/SEND return key is pressed (e.g. run a search).
// Global (one active form at a time); pass NULL to clear.
void nv_ime_set_submit_cb(nv_ime_submit_cb_t cb, void *user);

// Slide the keyboard away and unbind it (safe to call anytime, e.g. before an app closes).
void nv_ime_hide(void);

// Re-apply theme tokens to the keyboard (call on NV_EV_THEME_CHANGED).
void nv_ime_retheme(void);

// Re-apply the language-specific key map for the active nv_i18n language (call on
// NV_EV_LANG_CHANGED).
void nv_ime_relayout(void);

// ── Remote input injection (KeyDeck) ─────────────────────────────────────────────────
// Special keys a remote keyboard can deliver besides literal text.
typedef enum {
    NV_IME_RK_ENTER = 0,  // one-line / GO / SEARCH / SEND / NEXT: act as the return key;
                          // multi-line DEFAULT field: insert '\n'
    NV_IME_RK_ESC,        // dismiss the keyboard (like the close key)
    NV_IME_RK_BACKSPACE,  // delete before the cursor
    NV_IME_RK_DELETE,     // delete after the cursor (forward delete)
    NV_IME_RK_TAB,        // move to the next bound field in the same container
    NV_IME_RK_LEFT,
    NV_IME_RK_RIGHT,
    NV_IME_RK_UP,
    NV_IME_RK_DOWN,
} nv_ime_remote_key_t;

// Insert literal UTF-8 text / apply a special key on the IME's focused textarea, exactly
// as if typed on the on-screen keyboard (same return-key semantics, same key tick).
// Returns false when no field is focused (or the IME isn't up) — callers can surface
// that to the remote so the user knows to tap a field first.
// LVGL-thread only: from another task, wrap in lvgl_port_lock()/unlock().
bool nv_ime_inject_text(const char *utf8);
bool nv_ime_inject_key(nv_ime_remote_key_t key);

#ifdef __cplusplus
}
#endif
