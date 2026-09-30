// nv_apps — the set of native NucleoOS Anima applications.
// Each app lives in its own .cpp, owns a static NvApp descriptor, and is registered here.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Register every native app into the SystemUI registry. Call once, before nv_ui_start().
void nv_apps_register_all(void);

// The first-boot setup wizard (setup_app.cpp). After nv_ui_start(), under the LVGL lock.
// `first_boot`: the device never ran NucleoOS before (no "last_ver": new, or factory reset) -> the
// whole wizard; otherwise only the statistics question, once, if it was never answered.
void nv_setup_maybe_start(bool first_boot);
// The whole wizard again, on demand (Settings > About). LVGL thread; closes the calling app.
void nv_setup_run_again(void);

#ifdef __cplusplus
}
#endif
