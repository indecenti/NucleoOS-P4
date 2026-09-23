// nv_open — NucleoOS file associations: "Open", "Open with", default apps and file intents.
//
// THE SYSTEM STANDARD for opening a file. No app hard-codes which app handles a file type: it
// calls nv_open_file(path) and the OS resolves it in three steps.
//
//   1. Type     The extension maps to a MIME type and a kind through ONE system table
//               (nv_open_mime / nv_open_kind). Unknown extensions are application/octet-stream.
//               Apps may add types at boot (nv_open_register_type).
//   2. Handler  Apps register NvOpenHandler descriptors saying which MIME patterns they accept.
//               OPENERS launch their app with an intent (view / play / edit). ACTIONS are verbs
//               run in place on the file (e.g. "Set as wallpaper") and never replace the app.
//   3. Choice   The user's default for that MIME wins. Otherwise a single opener — or one whose
//               priority beats every other — opens directly, and a tie raises the system
//               "Open with" sheet (with "Remember my choice").
//
// Intent lifecycle, as seen by the launched app:
//   - nv_open_intent() returns the intent that launched the FOREGROUND app, NULL otherwise. It
//     stays valid across build() re-runs (theme / language refresh) until the app closes, so an
//     app simply checks it at the top of build().
//   - Back at the app's root returns to the app that asked (Files comes back at the same folder,
//     via an NV_INTENT_RESUME intent). An app whose intent view has its own in-app Back calls
//     nv_open_finish() when that view is done. Home and Recents never return: they close as usual.
//   - nv_open_intent_drop(): the user moved on inside the app (e.g. browsed the library), so Back
//     behaves normally again.
//   - Opening a file with the app that is already in the foreground re-delivers the intent in
//     place (the app's build() re-runs) instead of closing and relaunching it.
//
// Threading: LVGL thread only, except nv_open_file_async() (any task) and the pure type queries
// (nv_open_mime, nv_open_kind*, nv_open_mime_match), which read tables filled at boot.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NV_OPEN_PATH_MAX 256   // absolute POSIX path, NUL included
#define NV_OPEN_MIME_MAX 48
#define NV_OPEN_ID_MAX   40    // handler ids and app ids

// ------------------------------------------------------------------------------ file types
typedef enum {
    NV_FILE_OTHER = 0,   // unknown / binary
    NV_FILE_DIR,
    NV_FILE_TEXT,        // plain text, markdown, source, config, csv, json, xml...
    NV_FILE_IMAGE,
    NV_FILE_AUDIO,
    NV_FILE_VIDEO,
    NV_FILE_APP,         // installable / runnable app package (.wasm)
    NV_FILE_ARCHIVE,
    NV_FILE_KIND_COUNT
} nv_file_kind_t;

// MIME type of a file name or path, by extension (case-insensitive). Never NULL:
// "application/octet-stream" when unknown.
const char *nv_open_mime(const char *name);
// Kind of a MIME type (table lookup, then the "text/", "image/", ... prefix).
nv_file_kind_t nv_open_kind_of_mime(const char *mime);
// Kind of a file name or path (by extension). Directories are the caller's knowledge: pass
// NV_FILE_DIR yourself after a stat().
nv_file_kind_t nv_open_kind(const char *name);
// LV_SYMBOL_* glyph for a kind (file-manager rows, chooser fallbacks).
const char *nv_open_kind_symbol(nv_file_kind_t kind);
// Translated display name of a kind ("Image", "Immagine", ...).
const char *nv_open_kind_label(nv_file_kind_t kind);
// True when `mime` matches any pattern of a space-separated list: "type/sub", "type/*" or "*/*".
bool nv_open_mime_match(const char *patterns, const char *mime);

// Teach the OS a new extension (e.g. declared by an installed app). Boot-time only (before other
// tasks query types). A known extension keeps its built-in type. False when invalid / table full.
bool nv_open_register_type(const char *ext, const char *mime, nv_file_kind_t kind);

// Enumerate the type table (built-in + registered), e.g. for the Default apps settings page.
// Several extensions may share one MIME (jpg, jpeg). Returns false past the end.
int  nv_open_type_count(void);
bool nv_open_type_at(int index, const char **ext, const char **mime, nv_file_kind_t *kind);

// ------------------------------------------------------------------------------ handlers
typedef enum {
    NV_OPEN_OPENER = 0,   // launches app_id with an intent for the file
    NV_OPEN_ACTION = 1,   // runs `run` in place (the current app stays)
} nv_open_role_t;

typedef bool (*nv_open_action_fn)(const char *path, void *ctx);

typedef struct {
    const char *id;          // unique and stable (persisted in the defaults): "<app>.<verb>"
    const char *app_id;      // OPENER: app launched with the intent. ACTION: owner app or NULL
    const char *types;       // space-separated MIME patterns: "text/plain text/markdown", "audio/*"
    int         label_id;    // nv_str_id_t for chooser rows / action buttons; -1 = the app's name
    const char *label;       // fallback when label_id < 0 and app_id has no registered app
    const char *symbol;      // LV_SYMBOL_* for chooser rows / action buttons (NULL = kind glyph)
    uint8_t     role;        // nv_open_role_t
    int8_t      priority;    // opener tie-break: built-in viewers 50, secondary views 10, stores 0
    nv_open_action_fn run;   // ACTION only: perform the verb (LVGL thread); false = failed
    void       *ctx;         // passed to run
} NvOpenHandler;

// Register a handler (static lifetime; same id replaces the previous descriptor). Call from an
// app's *_register() function, next to its NvApp. False when invalid or the table is full.
bool nv_open_register(const NvOpenHandler *h);
// Drop every handler owned by an app (app uninstall). Returns how many were removed.
int  nv_open_unregister_app(const char *app_id);
const NvOpenHandler *nv_open_find(const char *handler_id);

// Handlers of `role` that accept a MIME (or a path). Openers come best-first (user default,
// then priority, then registration order) and only for apps that are still installed.
int nv_open_handlers_mime(const char *mime, nv_open_role_t role, const NvOpenHandler **out, int max);
int nv_open_handlers(const char *path, nv_open_role_t role, const NvOpenHandler **out, int max);
// The opener Open uses without asking (user default, sole opener, or a clear priority winner);
// NULL when nothing can open it or the choice is ambiguous (Open then shows the chooser).
const NvOpenHandler *nv_open_preferred_mime(const char *mime);
const NvOpenHandler *nv_open_preferred(const char *path);
// Display label / glyph of a handler (translated app name for openers without a label).
const char *nv_open_handler_label(const NvOpenHandler *h);
const char *nv_open_handler_symbol(const NvOpenHandler *h, const char *mime);

// ------------------------------------------------------------------------------ opening
// Open a file (or a folder, which opens Files there). Uses the preferred opener, else shows the
// "Open with" sheet. The launch itself is deferred to the next LVGL loop, so it is safe to call
// from any event handler (the caller's widgets are torn down afterwards, never under it).
// False, with a toast, when the file is missing or nothing can open it.
bool nv_open_file(const char *path);
// Always show the "Open with" sheet (openers only). False when nothing can open it.
bool nv_open_with(const char *path);
// Run one specific handler on a file (opener: deferred launch; action: runs now).
bool nv_open_run(const NvOpenHandler *h, const char *path);
bool nv_open_run_id(const char *handler_id, const char *path);
// Open Files at a folder, or at a file's folder with that file highlighted.
bool nv_open_reveal(const char *path);
// Any-task entry (web API, ANIMA worker): posts nv_open_run_id (handler_id set) or nv_open_file
// (handler_id NULL) to the LVGL thread. True when the request was queued.
bool nv_open_file_async(const char *path, const char *handler_id);

// ------------------------------------------------------------------------------ default apps
// The user's default handler id for a MIME ("" when none). Persisted in nv_config "open_defs".
void nv_open_get_default(const char *mime, char *out_id, size_t n);
// Set (handler_id) or clear (NULL / "") the default for a MIME.
void nv_open_set_default(const char *mime, const char *handler_id);
void nv_open_clear_defaults(void);
// Show the chooser in "pick default" mode for a MIME (with an "Ask every time" row). `done`
// runs on the LVGL thread after a pick (not on cancel), e.g. to refresh a settings page.
void nv_open_pick_default(const char *mime, void (*done)(void *ctx), void *ctx);

// ------------------------------------------------------------------------------ intents
typedef enum {
    NV_INTENT_OPEN   = 0,   // open `path` (handler says how)
    NV_INTENT_RESUME = 1,   // the app is being returned to after the file it opened was closed
    NV_INTENT_REVEAL = 2,   // show `path` (file or folder) in a file browser
} nv_intent_verb_t;

typedef struct {
    char    path[NV_OPEN_PATH_MAX];
    char    mime[NV_OPEN_MIME_MAX];
    char    handler[NV_OPEN_ID_MAX];   // handler id that launched the app ("" for RESUME/REVEAL)
    char    caller[NV_OPEN_ID_MAX];    // app to return to on Back ("" = none)
    uint8_t verb;                      // nv_intent_verb_t
} NvIntent;

// Intent of the foreground app, NULL when it was opened normally (see the lifecycle above).
const NvIntent *nv_open_intent(void);
// The intent view is done: return to the caller (restored) or close the app. Deferred.
void nv_open_finish(void);
// Forget the foreground app's intent: Back and Home behave normally from now on.
void nv_open_intent_drop(void);

// ------------------------------------------------------------------------------ SystemUI hooks
// Called by nv_ui only.
// Back pressed. `at_root` = the app has no in-app Back handler. True when consumed (the chooser
// was dismissed, or the app is returning to its caller).
bool nv_open_on_back(bool at_root);
// The foreground app `app_id` is closing: its intent (and any open sheet) goes away.
void nv_open_on_app_closed(const char *app_id);
// Dismiss the chooser without acting (UI rebuild, lock screen).
void nv_open_dismiss(void);

#ifdef __cplusplus
}
#endif
