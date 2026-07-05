// nv_ui — NucleoOS Anima SystemUI (Phase 2 slice): status bar + launcher + gestures.
// Always-resident thin shell drawn over the app plane. Call after the HAL/LVGL are up.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Build the status bar + launcher on the active screen and install the gesture handler.
void nv_ui_start(void);

// True while the notification shade is open (so the IME can avoid raising over it).
bool nv_ui_shade_is_open(void);

// Enable/disable the shade open gesture (top-edge + status-bar swipe-down). The video player turns
// it off while running so a swipe can't pull the notification shade down over the film. Re-enable
// on exit. Idempotent; disabling also closes the shade if it happens to be open.
void nv_ui_set_shade_gesture_enabled(bool on);

// Show a transient bottom-anchored toast (snackbar) that slides up, holds ~2s, slides away.
// Layer-free (position animation only). Safe to call from any LVGL-thread handler; a new call
// replaces any visible toast.
void nv_ui_toast(const char *msg);

// Route the Back gesture to an app handler (e.g. a fullscreen game pops its own screen) instead of
// closing the app; pass nullptr to restore default (Back closes). nv_ui_close_app lets the app
// request its own close (return to launcher). LVGL-thread only.
void nv_ui_set_back_handler(void (*fn)(void));
void nv_ui_close_app(void);

// Fullscreen app plane for games: hide the status bar / header / home pill and stretch the app
// content to the whole 1024x600 panel. Call nv_ui_app_fullscreen(true) from a game's build();
// pass false on teardown to restore the chrome. LVGL-thread only.
void nv_ui_app_fullscreen(bool on);

// Lock screen. nv_ui_lock() raises the full-screen lock overlay (wallpaper + clock; PIN pad
// when a PIN is set, else an Unlock button). Idempotent. LVGL-thread only.
void nv_ui_lock(void);
bool nv_ui_is_locked(void);
// Open the numeric keypad to define/replace the 4-digit unlock PIN (Settings uses this).
void nv_ui_set_pin_flow(void);

// ---- Remote UI automation (headless driving for screenshots / UI testing) ----
// All are LVGL-thread only: callers off the LVGL task (e.g. the web server) must hold the
// esp_lvgl_port lock across the call.
//
// Open a registered app by id (same launcher path: Memory Broker + solo-mode). Tears down any
// currently open app first. Returns true if the app is now the foreground app.
bool nv_ui_open_app_id(const char *id);
// Return to the home launcher (closes the foreground app + any open shade/recents/search).
void nv_ui_go_home(void);
// Inject a synthetic pointer tap at absolute screen coordinates (0..1023, 0..599). Drives rails,
// tabs, chips and buttons remotely; the release is auto-scheduled so it resolves to a click.
void nv_ui_tap(int x, int y);
// Id of the foreground app, or "" at home. Lets automation confirm a transition landed.
const char *nv_ui_current_app_id(void);

#ifdef __cplusplus
}
#endif
