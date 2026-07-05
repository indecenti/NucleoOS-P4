// nv_app — NucleoOS Anima application interface + registry.
// An app is a pure descriptor: identity, launcher icon, RAM budget, and a build() that
// populates a content container. SystemUI iterates the registry to draw the launcher and to
// open/close apps (solo-mode) — it never hard-codes any specific app. Apps live in nv_apps/,
// one file each, and self-register via nv_apps_register_all() at startup.
#pragma once
#include <stddef.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*nv_app_build_fn)(lv_obj_t *content);

typedef struct {
    const char *id;                  // stable identifier
    const char *name;                // launcher label + default app title
    const lv_image_dsc_t *icon;      // launcher icon
    size_t ram_budget;               // RAM the Memory Broker reserves before launch
    nv_app_build_fn build;           // populate the content area; NULL => "coming soon"
    int name_id;                     // nv_str_id_t for translated name; 0/NV_STR_APP_SETTINGS handled by launcher — use -1 or 0 sentinel
    const void *user;                // opaque per-app context (WASM apps carry their record here)
} NvApp;

// Register an app (the descriptor must have static lifetime).
void nv_app_register(const NvApp *app);
int nv_app_count(void);
const NvApp *nv_app_at(int index);

// Remove an app from the registry by id and rebuild the launcher live (tile disappears without a
// reboot). Fixes up the recents/dock so they never point at a stale slot. Returns 1 if one was
// removed, 0 if not found. Call on the LVGL thread. Used by the Apps store on uninstall.
int nv_app_unregister(const char *id);

// The app currently being opened / open (valid inside a build() call and while foreground).
// NULL at home. Lets a shared build() resolve its per-app context via NvApp.user.
const NvApp *nv_ui_current_app(void);

// Open an app by descriptor (same path the launcher uses: Memory Broker + solo-mode). Safe to
// call from an app (e.g. the Apps store launching a tile) — it tears down the caller first.
void nv_ui_open_app(const NvApp *app);

// Find a registered app by id (NULL if none). Lets the store resolve a scanned app to its tile.
const NvApp *nv_ui_find_app(const char *id);

#ifdef __cplusplus
}
#endif
