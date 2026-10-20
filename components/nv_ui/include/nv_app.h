// nv_app — NucleoOS Anima application interface + registry.
// An app is a pure descriptor: identity, launcher icon, RAM budget, a build() that populates a
// content container and an optional close() that tears it down. SystemUI iterates the registry to draw the launcher and to
// open/close apps (solo-mode) — it never hard-codes any specific app. Apps live in nv_apps/,
// one file each, and self-register via nv_apps_register_all() at startup.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*nv_app_build_fn)(lv_obj_t *content);

// Why the open app's content is about to go (nv_app_close_fn).
typedef enum {
    NV_APP_CLOSE = 0,   // closed for real (Home, X, Back at the root): release everything
    NV_APP_SWITCH,      // left for another app; it stays a task (save state with nv_ui_state_save)
    NV_APP_REBUILD,     // language / theme / rotation change or a new intent: build() runs again at
                        // once on the same content, so work in flight (a run, a playback) may go on
} nv_app_teardown_t;

// Called on the LVGL thread BEFORE the content is deleted (close, switch) or cleaned (rebuild),
// while every widget of the app is still alive and the app is still nv_ui_current_app(). Stop
// timers, background jobs and engines here. LV_EVENT_DELETE handlers on the app's widgets still
// run afterwards and keep doing per-widget cleanup only.
typedef void (*nv_app_close_fn)(nv_app_teardown_t why);

typedef struct {
    const char *id;                  // stable identifier
    const char *name;                // launcher label + default app title
    const lv_image_dsc_t *icon;      // launcher icon
    size_t ram_budget;               // RAM the Memory Broker reserves before launch
    nv_app_build_fn build;           // populate the content area; NULL => "coming soon"
    int name_id;                     // nv_str_id_t of the translated name, or -1 to show `.name` as is.
                                     // 0 is NV_STR_APP_SETTINGS: an app that leaves it out shows as "Settings".
    const void *user;                // opaque per-app context (WASM apps carry their record here)
    unsigned flags;                  // NV_APP_FLAG_*
    nv_app_close_fn close;           // optional teardown hook (see nv_app_close_fn); NULL = none
} NvApp;

#define NV_APP_FLAG_GAME 1u          // a game (Start > Games)
// Registered (search, Open with, the Terminal, Apps) but never a launcher icon: the home grid, its
// folders and the classic Start > All apps leave it out. Terminal tools have no window to open.
#define NV_APP_FLAG_NO_HOME 2u

static inline bool nv_app_on_home(const NvApp *a) { return a && !(a->flags & NV_APP_FLAG_NO_HOME); }

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

// The open app's screen generation: a new value on every build and every teardown (close, switch,
// rebuild), so a background job started for one screen can tell that screen is gone. Take it when
// the job is queued; check nv_ui_app_live(token) on the LVGL thread (or under the port lock) before
// touching any widget with the result. Replaces per-app "bumped on teardown" counters.
typedef uint32_t nv_app_token_t;
nv_app_token_t nv_ui_app_token(void);
bool nv_ui_app_live(nv_app_token_t token);

// True only inside build() when it runs right after close(NV_APP_REBUILD) on the same content:
// the app keeps its state and work in flight and just recreates its widgets. False on a fresh open
// (and on a switch back to a suspended task, which restores with nv_ui_state_load). LVGL thread.
bool nv_ui_app_rebuilding(void);

// Open an app by descriptor (same path the launcher uses: Memory Broker + solo-mode). Safe to
// call from an app (e.g. the Apps store launching a tile) — it tears down the caller first.
void nv_ui_open_app(const NvApp *app);

// Find a registered app by id (NULL if none). Lets the store resolve a scanned app to its tile.
const NvApp *nv_ui_find_app(const char *id);

// The launcher page the app's tile is on right now (in a folder: the folder's page), -1 when the
// launcher shows no tile for it. LVGL thread or the port lock. /api/ui/apps reports it ("home").
int nv_ui_home_page(const char *id);

#ifdef __cplusplus
}
#endif
