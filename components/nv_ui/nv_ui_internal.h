// nv_ui_internal.h — private seam between the shell core (nv_ui.cpp) and the classic desktop
// shell (nv_ui_classic.cpp). Not a public API: nothing outside components/nv_ui includes it.
// Everything here is LVGL-thread only.
#pragma once

#include <stdint.h>

#include "lvgl.h"
#include "nv_app.h"
#include "nv_theme.h"
#include "nv_wifi.h"

// Shell services the classic desktop uses (implemented in nv_ui.cpp).
namespace nvui {
const lv_image_dsc_t *icon(const NvApp *a, int px);   // app icon, high-quality downscale (cached)
const char *label(const NvApp *a);                    // localized app name
int  recents(const NvApp **out, int max);             // most recent first
int  most_used(const NvApp **out, int max);           // launch counters, highest first
void back(void);                                      // in-app back, else close the app
void open_shade(void);                                // notifications + quick settings
void close_shade(void);
bool fullscreen(void);                                // the open app covers the whole panel
void sleep_now(void);
void lock(void);
bool asleep(void);
void wallpaper(lv_obj_t *o);                          // theme gradient + wallpaper.jpg behind
void minimize(void);                                  // hide the open app, keep it running
void restore(void);                                   // show the minimized app again
bool minimized(void);
const lv_image_dsc_t *thumb(const NvApp *a);           // last-screen preview (Recents cache) or NULL

// Status shared by the tablet status bar and the classic taskbar tray.
void clock_text(char *buf, size_t n);                 // "HH:MM" / "hh:MM AM" per the 12/24 h setting
nv_wifi_state_t wifi_state(void);                     // NV_WIFI_DISABLED while the radio is off
// Wi-Fi glyph colour: green = online (SNTP synced), accent = linked / scanning / connecting,
// red = failed, dim = off. `th` is the palette of the surface drawing it.
lv_color_t wifi_color(const NvTheme *th, nv_wifi_state_t st);
void storage_icons(lv_obj_t *sd, lv_obj_t *usb);      // show each glyph only while mounted (NULL-safe)
}  // namespace nvui

// Notification presenter (nv_notify.cpp): the classic desktop shows popups over the taskbar.
namespace nvnotify {
void set_desktop(bool on);
}  // namespace nvnotify

// File-name index of the SD card for the Start menu search (nv_ui_filesearch.cpp).
namespace nvsearch {
void refresh(void);                                   // rebuild in the background if stale
bool ready(void);
int  find(const char *q, const char **out, int max);  // full paths, prefix matches first
}  // namespace nvsearch

// The classic desktop shell (implemented in nv_ui_classic.cpp).
namespace nvclassic {
constexpr int32_t kTaskH  = 44;   // taskbar height (touch-sized)
constexpr int32_t kTitleH = 36;   // window title bar height

void enable(bool on);            // build / tear down desktop + taskbar
void rebuild(void);              // theme / language / rotation / app list changed
// Title bar content for an open app inside `hdr` (already sized); returns the title label.
lv_obj_t *frame_header(lv_obj_t *hdr, const NvApp *a);
void on_app_changed(void);       // an app opened: taskbar buttons
void on_app_closed(const NvApp *a, bool switching);   // closed, or left for another app
void task_activate(int n);       // Win+1..9: switch to the n-th taskbar task
void set_fullscreen(bool on);
bool fs_bar_visible(void);       // the pop-down title bar is over a fullscreen app    // a fullscreen app covers the taskbar
bool start_toggle(void);         // Win key / Start button
lv_color_t icon_color(void);     // colour for themed icons: the desktop palette's accent, else the theme's
bool escape(void);               // Esc: close a context menu or the Start menu; false = none open
bool context_at(lv_point_t p);
void edit_menu(lv_point_t p, lv_obj_t *ta);   // Cut / Copy / Paste / Select all on a text field   // right click at p: open the desktop's context menu if it's ours
}  // namespace nvclassic
