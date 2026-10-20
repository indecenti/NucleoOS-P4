// nv_dl — the system downloader: big files from the Internet to the SD card in the background, for
// whoever asks (an app through ABI 18, the shell's `dl`), verified piece by piece and resumed where it
// stopped after a dropped connection, a mirror gone bad or a restart. docs/ZIM_READER_PLAN.md §4f.
//
// One download runs at a time; the others wait in order. Jobs live on the card (/sdcard/data/dl), so a
// queue survives a restart. A Metalink 4 (.meta4, what Kiwix serves) gives the size, the SHA-1 of every
// 4 MiB piece and the mirrors: a piece reaches the record only once its hash matches, a wrong piece
// sends the download to the next mirror. A plain URL needs the size and SHA-256 from the caller (the
// whole file is checked at the end). Only https, only public addresses. The finished file appears
// under its name only after the check; until then it is "<name>.part".
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nv_dl_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NV_DL_QUEUED = 0,
    NV_DL_PAUSED,
    NV_DL_DONE,
    NV_DL_FAILED,
    NV_DL_RUNNING,        // never stored: QUEUED while the worker has it
} nv_dl_state_t;

enum {
    NV_DL_EARG = -2,      // bad URL, path, size or hash
    NV_DL_EFULL = -3,     // too many downloads listed (NV_DL_MAX): finished ones must be removed
    NV_DL_EEXISTS = -4,   // the file is there already, or another download writes it
    NV_DL_ESD = -5,       // no card
    NV_DL_EDENIED = -6,   // a secret file (or a folder holding one), or a system folder
};

#define NV_DL_MAX 16

typedef struct {
    uint32_t id;
    char     owner[NVDL_OWNER_MAX];
    char     title[NVDL_TITLE_MAX];
    char     dest[NVDL_PATH_MAX];
    int      state;                 // nv_dl_state_t
    uint64_t size;                  // 0 until the .meta4 has been read
    uint64_t done;                  // bytes on the card, the piece being fetched included
    uint64_t verified;              // of which checked (a restart resumes from here)
    uint32_t bps;                   // recent speed while running, bytes/s
    char     phase[12];             // while running: "meta4", "fetch", "check"
    char     error[NVDL_ERR_MAX];   // why it failed / is waiting ("" = nothing to say)
} nv_dl_info_t;

typedef struct {
    const char *owner;      // app id ("shell" from the Terminal)
    const char *title;      // what the notification and the lists call it
    const char *dest;       // physical path of the finished file ("/sdcard/home/zim/x.zim")
    const char *meta4;      // a Metalink 4 URL ... (https)
    const char *url;        // ... or the file itself (https), with:
    uint64_t    size;       //     its exact size
    const char *sha256;     //     and its SHA-256 (64 hex digits)
} nv_dl_req_t;

// Starts the worker (call once at boot, after the card and Wi-Fi). It loads the queue from the card and
// waits, like the content service, for the card, a confirmed OTA image and a network.
void nv_dl_start(void);

// Queues a download. Returns its id (> 0) or NV_DL_E*; `err` (may be NULL) says why in a few words.
int  nv_dl_add(const nv_dl_req_t *req, char *err, size_t err_n);
bool nv_dl_get(uint32_t id, nv_dl_info_t *out);
// The downloads of `owner` (NULL = all), oldest first. Returns how many were written to out.
int  nv_dl_list(const char *owner, nv_dl_info_t *out, int max);
// Pauses a queued or running download; `pause` false resumes a paused one or retries a failed one.
bool nv_dl_pause(uint32_t id, bool pause);
// Stops a download and deletes what it wrote, or, for a finished one, forgets it (the file stays).
bool nv_dl_cancel(uint32_t id);
// Bytes a new download may still use on the card: free space less the room an OTA update needs and
// what the listed downloads have yet to fetch. -1 without a card. The card is asked at most every 30 s
// (the first time can take a second on a big FAT32 card).
int64_t nv_dl_room(void);
// Bumps on every change (progress included): a screen redraws when it moves.
uint32_t nv_dl_generation(void);
// What the worker waits for: "" (nothing / working), "sd", "safe", "ota", "net", "retry" (a
// download failed for a moment and waits a little before the next try).
const char *nv_dl_waiting(void);

// Called on the worker when a download finishes or fails (the notification lives in the UI layer).
void nv_dl_set_hook(void (*hook)(const nv_dl_info_t *info));

#ifdef __cplusplus
}
#endif
