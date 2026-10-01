// nv_apps — the set of native NucleoOS Anima applications.
// Each app lives in its own .cpp, owns a static NvApp descriptor, and is registered here.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Register every native app into the SystemUI registry. Call once, before nv_ui_start().
void nv_apps_register_all(void);
// A store install finished elsewhere (shell `store install`, ANIMA): give the app its launcher tile
// now. LVGL thread, or hold the esp_lvgl_port lock.
void nv_apps_store_installed(const char *id);
// Background check of the store for updates of the installed apps, announced as one notification
// (apps_app.cpp). After nv_ui_start(), under the LVGL lock; once.
void nv_apps_store_watch_start(void);

// The first-boot setup wizard (setup_app.cpp). After nv_ui_start(), under the LVGL lock.
// `first_boot`: the device never ran NucleoOS before (no "last_ver": new, or factory reset) -> the
// whole wizard; otherwise only the statistics question, once, if it was never answered.
void nv_setup_maybe_start(bool first_boot);
// The whole wizard again, on demand (Settings > About). LVGL thread; closes the calling app.
void nv_setup_run_again(void);

#ifdef __cplusplus
}
#endif
