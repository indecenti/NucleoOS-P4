// nv_ui_host — the app-host API an app may call while it is open (in-app navigation).
// Lets an app retitle its header and install an in-app "back" handler so it can push
// sub-pages (e.g. Settings: category list <-> category detail) without the back button
// closing the whole app.
#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Change the title shown in the current app's header bar.
void nv_ui_set_title(const char *text);

// Set the in-app back handler. NULL means the Back button closes the app.
// A non-NULL handler is invoked by Back instead (e.g. to pop a sub-page).
void nv_ui_set_back(void (*handler)(void));

// The content area of the currently open app (for clearing/rebuilding during in-app nav).
lv_obj_t *nv_ui_app_content(void);

#ifdef __cplusplus
}
#endif
