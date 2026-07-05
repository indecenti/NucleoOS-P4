// nv_apps — the set of native NucleoOS Anima applications.
// Each app lives in its own .cpp, owns a static NvApp descriptor, and is registered here.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Register every native app into the SystemUI registry. Call once, before nv_ui_start().
void nv_apps_register_all(void);

#ifdef __cplusplus
}
#endif
