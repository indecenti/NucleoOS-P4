// nv_appstore — install WASM apps from a remote HTTP catalog into /sdcard/apps.
//
// The device already runs apps discovered under /sdcard/apps/<id>/ (see nv_wasm). Until now the
// only way to get an app onto the card was the sandboxed web-FS push (dev loop) or an embedded
// self-install (first-party games). This component adds the missing consumer path: a *remote store*.
//
// A store is any HTTP(S) host that exposes:
//     GET  {base}/store-<lang>.json             catalog in one language: {"apps":[{id,name,...}, ...]}
//     GET  {base}/store.json?lang=&region=&api= the same from a live server, asked only on a 404
//     GET  {base}/apps/<id>/manifest.json       one app's manifest (same schema nv_wasm validates)
//     GET  {base}/apps/<id>/app.wasm            the module (+ optional app.aot, icon.z, icon.argb)
//     GET  {base}/apps/<id>/files.json          assets list {"files":[{"p":"img/x.565","n":bytes}]}
//     GET  {base}/apps/<id>/<img|snd|models>/<name>   each asset it lists
//
// Dependencies: a catalog row may carry "requires": {"<id>": "<min version>"} (the manifest's) and
// "kind": "library". install() resolves them first from the same catalog: every package that is
// missing or older than required is installed before the app, depth first; a system component
// (nv_wasm_sys_component) that is too old stops the install with "update the system".
// The default store is static files on GitHub Pages (indecenti/nucleoos-p4-store, published by
// tools/dist.py); server/appstore/appstore_server.py serves the same layout live for local tests.
// A static catalog holds every region, so the region setting only filters on a live server.
//
// Everything network-facing is ASYNC: refresh() and install() hand the blocking HTTP off to a
// worker task and flip a state machine; the UI polls it from an LVGL timer (nv_ota's model). The
// downloaded module is written straight to /sdcard/apps/<id>/ — unlike the web FS this reaches the
// real app dir, so a store install produces a first-class app (games included) after the next scan.
//
// Trust model: WAMR sandboxes the guest and every host import is gated on manifest permissions, so a
// hostile module cannot escape; the store URL is user-configured (Settings), i.e. trusted. On top of
// that we validate the wasm magic + a hard size cap before the bytes ever hit the card.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Largest catalog we hold in memory (a PSRAM snapshot; the WASM-4 gallery alone is ~150 carts).
#define NV_STORE_MAX 512
#define NV_STORE_DEPS_MAX 4
#define NV_STORE_VARIANTS_MAX 6   // "variants" per package (store page chips)   // same as the manifest's "requires" (nv_wasm NV_WASM_DEPS_MAX)

typedef enum {
    NV_STORE_IDLE = 0,   // nothing in flight; refresh()/install() allowed
    NV_STORE_FETCHING,   // downloading + parsing the catalog
    NV_STORE_READY,      // catalog snapshot available (see count()/get())
    NV_STORE_INSTALLING, // downloading one app onto the SD card (see progress())
    NV_STORE_ERROR,      // last op failed (see message()); catalog snapshot may still be valid
} nv_store_state_t;

// One catalog row. `name`/`desc`/`category_name` arrive already localized (the device asks the store
// for ?lang=<locale>); other fields are cross-checked against the local /sdcard/apps.
typedef struct {
    char     id[32];
    char     name[48];         // localized display name
    char     version[16];
    char     author[40];
    char     desc[256];        // localized description (a few lines)
    char     license[32];      // e.g. "CC BY-NC-SA 4.0" ("" = not stated)
    char     source[96];       // the app's home page (credits), "" = none
    char     category[24];     // machine id ("games", "education", …)
    char     category_name[28];// localized category label ("Giochi", "Istruzione", …)
    char     subcategory[24];  // optional sub-category id within the category ("gameboy", "ha", …), "" = none
    char     subcategory_name[28]; // its localized label ("Game Boy", "Home Assistant", …)
    uint32_t abi;        // required host ABI (so the UI can flag apps this OS is too old to run)
    uint32_t size;       // app.wasm bytes advertised by the catalog (display only)
    uint32_t icon_z;     // bytes of the compressed icon offered (0 = none): nv_appstore_icons_want
    uint32_t aot_size;   // app.aot bytes offered next to it (0 = none): installed too, runs native
    uint16_t rating10;   // store rating × 10 (0..50; 0 = unrated)
    bool     featured;   // editorially promoted
    bool     is_game;    // abi>=2 with the gfx permission
    bool     has_icon;   // an icon.argb is offered
    bool     installed;  // an app with this id already lives in /sdcard/apps
    bool     update;     // catalog version is newer than the installed one
    bool     library;    // "kind":"library": a package other apps require (no tile, never run)
    bool     engine;     // ABI v14 "engine": runs another package's module, ships none itself
    bool     has_doc;    // "doc": the store serves a guide at {store url}/docs/<id>.html
    bool     console;    // "console": terminal program (no window, runs in the Terminal)
    uint8_t  n_deps;     // "requires": system components or packages, minimum versions
    struct { char id[32]; char version[12]; } deps[NV_STORE_DEPS_MAX];
    uint16_t files;      // asset files offered in apps/<id>/files.json (img/ snd/ models/)
    uint32_t perms;      // "perms": sensitive manifest permissions (NV_WPERM_*), shown before install
    uint32_t downloads;  // installs counted by the store's anonymous counter (0 = none yet)
    uint32_t added;      // day it first reached the store, YYYYMMDD (0 = unknown)
    uint32_t updated;    // day its current version replaced an older one, YYYYMMDD (0 = never)
    char     notes[160]; // "what's new" in this version, localized ("" = none)
    uint8_t  shots;      // store screenshots offered (nv_appstore_shots_want), 0 = none
    // "variants": editions of one package the owner picks on the app page (a game's languages).
    // The chosen id lands in /sdcard/apps/<id>/data/variant; the app reads it (/appdata/variant).
    uint8_t  n_var;
    struct { char id[9]; char name[21]; char lang[3]; uint32_t size; } var[NV_STORE_VARIANTS_MAX];
} nv_store_entry_t;

// A store category as the catalog curates it (store.json "categories", in the store's order): the
// localized name and one-line description, its colour (0xRRGGBB, 0 = none), how many apps it holds
// and the names of the three that lead it. Only categories with visible apps are listed.
#define NV_STORE_CATS_MAX 24
typedef struct {
    char     id[24];
    char     name[28];
    char     desc[112];
    char     top[3][48];
    uint32_t color;
    uint16_t count;
} nv_store_category_t;
int  nv_appstore_category_count(void);
bool nv_appstore_category_get(int i, nv_store_category_t *out);

// Base store URL, no trailing slash (default "https://indecenti.github.io/nucleoos-p4-store", a
// local one looks like "http://192.168.1.20:8090"). Backed by nv_config "store_url"; get() falls
// back to the compiled-in default when unset or empty.
void nv_appstore_get_url(char *out, size_t n);
void nv_appstore_set_url(const char *url);

// Storefront region (ISO-ish code like "IT", "US", "EU", or "*"/"" for worldwide). Backed by
// nv_config "store_region"; the device sends it as ?region= so the store can geolocate the catalog.
void nv_appstore_get_region(char *out, size_t n);
void nv_appstore_set_region(const char *region);

// Anonymous install counter (server/stats/README.md): after a store install or update the device
// sends GET https://nucleoos.indexhub.it/stats/{i|u}/<app id>, nothing else (no device id, no
// query), and only for installs from the public store. The totals become the catalog's
// "downloads" ("Most downloaded"). Only with the owner's opt-in: the same consent as nv_telemetry
// (setup wizard, Settings > Security); these two are shorthands for it.
bool nv_appstore_stats_enabled(void);
void nv_appstore_set_stats_enabled(bool on);

nv_store_state_t nv_appstore_state(void);
const char      *nv_appstore_message(void);   // human status / error text ("" when none)
int              nv_appstore_progress(void);   // 0..100 while INSTALLING (else last value)

// Kick off an async catalog fetch of {url}/store.json. No-op while FETCHING/INSTALLING. Poll state:
// FETCHING -> READY (snapshot ready) or ERROR (message() explains).
void nv_appstore_refresh(void);

// Read the last fetched catalog. Safe from the LVGL thread (copies under the lock). count() is the
// number of rows; get(i,out) fills out and returns false when i is out of range.
int  nv_appstore_count(void);
bool nv_appstore_get(int i, nv_store_entry_t *out);

// Kick off an async install/update of catalog id `id` (download -> /sdcard/apps/<id>/), with the
// packages it requires. No-op and returns false when busy or the id is not in the current catalog.
// Poll state: INSTALLING -> READY (installed flag now set; a rescan/reboot surfaces the launcher
// tile) or ERROR; message() names a dependency while it downloads.
bool nv_appstore_install(const char *id);

// System apps (nv_wasm_is_system_app: Lua, JavaScript, SQLite): a background task installs the
// ones missing from the card from the signed store — 90 s after boot, then every 5 min until
// they're all there (a few hours at most), starting a job only while the store is idle. Called once
// at boot; a no-op while the task runs.
void nv_appstore_system_start(void);
// Same, with the edition to install (a "variants" id; nullptr/"" = none): written to
// /sdcard/apps/<id>/data/variant once the package is in place.
bool nv_appstore_install_variant(const char *id, const char *variant);

// The edition an installed package uses ("" when none / not installed), and switching it: only the
// file changes, the app fetches what the new edition needs itself. False on an SD error.
void nv_appstore_variant_get(const char *id, char *out, size_t n);
bool nv_appstore_variant_set(const char *id, const char *variant);

// id currently being installed ("" when not INSTALLING).
const char *nv_appstore_installing_id(void);

// Store icons: 80x80 ARGB8888, served raw-deflate compressed (icon.z, ~1-2 KB). want() queues the
// ids the UI is about to show (entries with icon_z > 0; others are ignored) for a background
// download into a small compressed cache; get() inflates a fetched one into `argb`
// (NV_STORE_ICON_BYTES) and returns false while it isn't there (yet). An install also saves it as
// /sdcard/apps/<id>/icon.z, which the launcher tile uses.
#define NV_STORE_ICON_PX    80
#define NV_STORE_ICON_BYTES (NV_STORE_ICON_PX * NV_STORE_ICON_PX * 4)
void nv_appstore_icons_want(const char *const *ids, int n);
bool nv_appstore_icon_get(const char *id, uint8_t *argb);

// Store screenshots: catalog "shots" = n, served at {store}/shots/<id>/<k>.jpg (k = 1..n, baseline
// JPEG up to 512x300). They are for the app page only, never installed. want() fetches every
// screenshot of `id` in the background, forgetting another app's; get() hands out screenshot k
// (1-based): 1 = ready (*jpg / *len stay valid until the next want() of another id — decode them
// right away), 0 = still coming, -1 = failed or not offered.
#define NV_STORE_SHOTS_MAX 6
void nv_appstore_shots_want(const char *id);
int  nv_appstore_shot_get(const char *id, int k, const uint8_t **jpg, size_t *len);

// Inflate a raw-deflate icon (icon.z) into NV_STORE_ICON_BYTES of ARGB8888. False if corrupt.
bool nv_appstore_icon_inflate(const uint8_t *z, size_t len, uint8_t *argb);

#ifdef __cplusplus
}
#endif
