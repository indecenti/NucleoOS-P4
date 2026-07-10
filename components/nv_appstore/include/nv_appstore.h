// nv_appstore — install WASM apps from a remote HTTP catalog into /sdcard/apps.
//
// The device already runs apps discovered under /sdcard/apps/<id>/ (see nv_wasm). Until now the
// only way to get an app onto the card was the sandboxed web-FS push (dev loop) or an embedded
// self-install (first-party games). This component adds the missing consumer path: a *remote store*.
//
// A store is any HTTP(S) server that exposes:
//     GET  {base}/store.json                    catalog: {"apps":[{id,name,version,abi,...}, ...]}
//     GET  {base}/apps/<id>/manifest.json       one app's manifest (same schema nv_wasm validates)
//     GET  {base}/apps/<id>/app.wasm            the module
//     GET  {base}/apps/<id>/icon.argb           optional 80x80 ARGB8888 launcher icon
// The reference server lives in server/appstore/ (Python, stdlib only).
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

// Largest catalog we hold in memory (matches the launcher's WASM-app budget).
#define NV_STORE_MAX 32

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
    char     desc[128];        // localized one-line description
    char     category[24];     // machine id ("games", "education", …)
    char     category_name[28];// localized category label ("Giochi", "Istruzione", …)
    uint32_t abi;        // required host ABI (so the UI can flag apps this OS is too old to run)
    uint32_t size;       // app.wasm bytes advertised by the catalog (display only)
    uint16_t rating10;   // store rating × 10 (0..50; 0 = unrated)
    bool     featured;   // editorially promoted
    bool     is_game;    // abi>=2 with the gfx permission
    bool     has_icon;   // an icon.argb is offered
    bool     installed;  // an app with this id already lives in /sdcard/apps
    bool     update;     // catalog version is newer than the installed one
} nv_store_entry_t;

// Base store URL, no trailing slash (e.g. "http://192.168.0.216:8090"). Backed by nv_config
// "store_url"; get() falls back to the compiled-in default when unset.
void nv_appstore_get_url(char *out, size_t n);
void nv_appstore_set_url(const char *url);

// Storefront region (ISO-ish code like "IT", "US", "EU", or "*"/"" for worldwide). Backed by
// nv_config "store_region"; the device sends it as ?region= so the store can geolocate the catalog.
void nv_appstore_get_region(char *out, size_t n);
void nv_appstore_set_region(const char *region);

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

// Kick off an async install/update of catalog id `id` (download -> /sdcard/apps/<id>/). No-op and
// returns false when busy or the id is not in the current catalog. Poll state: INSTALLING ->
// READY (installed flag now set; a rescan/reboot surfaces the launcher tile) or ERROR.
bool nv_appstore_install(const char *id);

// id currently being installed ("" when not INSTALLING).
const char *nv_appstore_installing_id(void);

#ifdef __cplusplus
}
#endif
