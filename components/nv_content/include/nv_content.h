// nv_content — the system content service: what the OS needs on the microSD (the web companion,
// ANIMA's offline data) and keeping it there. docs/CONTENT_PACKS_PLAN.md.
//
// The store offers the packs (nv_appstore: content/index-v1.json + signed content/<id>/pack.sig); this
// service decides what to do and when:
//   * a queue of packs to install, kept on the card (/sdcard/nucleos/content/queue): it survives a
//     reboot and resumes where the download stopped;
//   * it works only when that can't hurt a firmware update: card mounted, not in safe mode, the running
//     image confirmed (its probation must reach the update server undisturbed), no update in progress,
//     the store idle, a network up. Otherwise it waits and says why (nv_content_waiting());
//   * a failed install retries after 1, 5, 15, then 60 minutes;
//   * the ONLY thing it installs without being asked: an installed pack older than this firmware
//     requires (kRequired in nv_content.cpp), e.g. the web companion after an OTA. A web companion
//     changed on the device (POST /api/web/put) is left alone.
// Listeners hear about a folder swap (before/after) so the web cache and ANIMA let go of old files.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "nv_appstore.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NV_CONTENT_MISSING = 0,     // offered, not installed
    NV_CONTENT_OK,              // installed, current
    NV_CONTENT_UPDATE,          // installed, the store has a newer version
    NV_CONTENT_QUEUED,          // waiting its turn (or for the network / the store / a retry)
    NV_CONTENT_INSTALLING,      // downloading or unpacking (see progress)
    NV_CONTENT_DAMAGED,         // the last check found files that differ from the signed list
    NV_CONTENT_UNAVAILABLE,     // not offered by the store (or the list isn't loaded yet)
} nv_content_state_t;

typedef struct {
    nv_content_entry_t e;       // what the store offers (id, version, size, name, description)
    char  installed[16];        // installed version, "" when none
    nv_content_state_t state;
    bool  recommended;          // for the UI language
    bool  required;             // this firmware requires it (kRequired)
    int   progress;             // 0..100 while INSTALLING
} nv_content_pack_t;

// Start the service task (app_main, not in safe mode).
void nv_content_start(void);

// Packs the store offers (and installed packs it no longer lists), in index order.
int  nv_content_count(void);
bool nv_content_get(int i, nv_content_pack_t *out);
bool nv_content_find(const char *id, nv_content_pack_t *out);

// Queue packs (persisted). install(): one id; install_recommended(): every recommended pack that is
// missing or outdated, returns how many were queued. cancel(): drop it from the queue (and its partial
// downloads). verify(): re-check an installed pack against its signed list (DAMAGED when it fails;
// install() then repairs it, downloading only what differs).
bool nv_content_install(const char *id);
int  nv_content_install_recommended(void);
bool nv_content_cancel(const char *id);
bool nv_content_verify(const char *id);

// Bytes the recommended-but-missing packs would download (what the setup wizard shows).
uint64_t nv_content_recommended_bytes(void);
// Recommended packs not installed (0 = everything recommended is there).
int  nv_content_recommended_missing(void);

// Why the queue isn't moving right now: "" (working or nothing to do), "sd", "safe", "ota", "net",
// "store" (busy or unreachable), "space" (not enough room: the OTA reserve is kept), "retry".
const char *nv_content_waiting(void);
// Last outcome in plain words ("Installed ...", "Not enough space on the SD card", ...).
void nv_content_message(char *out, size_t n);
// Bumps on any change (UI polls it).
uint32_t nv_content_generation(void);
// The store's list was loaded at least once this boot.
bool nv_content_index_loaded(void);
// Ask for a fresh list now (the service also refreshes it every 12 h).
void nv_content_refresh(void);

// Folder swaps and finished installs (see nv_appstore_set_content_hook). Up to 4 listeners, called on
// the store worker task: keep them short.
typedef void (*nv_content_listener_t)(const char *dest, const char *name, bool before);
void nv_content_add_listener(nv_content_listener_t fn);

#ifdef __cplusplus
}
#endif
