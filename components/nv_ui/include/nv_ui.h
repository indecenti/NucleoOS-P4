// nv_ui — NucleoOS Anima SystemUI (Phase 2 slice): status bar + launcher + gestures.
// Always-resident thin shell drawn over the app plane. Call after the HAL/LVGL are up.
#pragma once
#include <stddef.h>   // size_t (nv_ui_input_debug)
#include <stdbool.h>

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
// Re-run the foreground app's build() in place (same path as a theme/language refresh). Used by
// nv_open to deliver a new intent to an app that is already open. No-op at home. LVGL-thread only.
void nv_ui_rebuild_app(void);

// Drop the cached SD wallpaper (/sdcard/wallpaper.jpg) and rebuild the launcher so a new or
// removed file shows immediately. LVGL-thread only.
void nv_ui_wallpaper_reload(void);

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
// Async variant for callers NOT on the LVGL thread (e.g. the web /api/ui/open endpoint): posts the
// open to the UI thread so the app teardown+relaunch runs there (like a real tap) instead of under a
// foreign task holding lvgl_port_lock — which can deadlock UI+httpd on a WASM relaunch. Returns true
// if the request was enqueued (the switch happens shortly after, on the UI thread).
bool nv_ui_open_app_id_async(const char *id);
// Return to the home launcher (closes the foreground app + any open shade/recents/search).
void nv_ui_go_home(void);
// Async variant for callers NOT on the LVGL thread (the web /api/ui/home endpoint): the app teardown
// (WASM abort handshake, Recents thumbnail, SD writes) runs on the UI thread. True if enqueued.
bool nv_ui_go_home_async(void);
// Inject a synthetic pointer tap at absolute screen coordinates (0..1023, 0..599). Drives rails,
// tabs, chips and buttons remotely; the release is auto-scheduled so it resolves to a click.
void nv_ui_tap(int x, int y);
// Inject a synthetic drag from (x0,y0) to (x1,y1) over `ms` (30..3000), then release. Exercises
// swipes remotely (launcher paging, gestures). Ignored while a previous drag is still running.
void nv_ui_swipe(int x0, int y0, int x1, int y1, int ms);
// Id of the foreground app, or "" at home. Lets automation confirm a transition landed.
const char *nv_ui_current_app_id(void);
// Diagnostics: text dump of the UI input state (indev internals, gesture flags, overlays, touch
// cache, recent indev events) into out[n]; returns bytes written. LVGL thread / lock held.
size_t nv_ui_input_debug(char *out, size_t n);

#ifdef __cplusplus
}
#endif
