// nv_sd — microSD (SDMMC 4-bit) mount + hot-plug for NucleoOS Anima.
//
// The board has no card-detect GPIO, so presence is tracked by a low-priority monitor task:
// while unmounted it retries the mount (so a card inserted after boot comes online on its own);
// while mounted it pings the card (CMD13) and unmounts after two consecutive misses (debounced
// against transient bus contention). Every mount/unmount bumps a generation counter so the UI
// can cheaply detect changes by polling. All state transitions are serialized under a mutex.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// Attempt an initial mount and start the hot-plug monitor. Returns true if a card is mounted.
// Non-fatal: with no card it returns false and the monitor keeps polling. Idempotent.
bool nv_sd_mount(void);

// Unmount now (also stops nothing — the monitor will simply try to remount if a card is present).
void nv_sd_unmount(void);

bool nv_sd_is_mounted(void);

// Bumps on every mount/unmount transition — poll this to react to insert/remove.
uint32_t nv_sd_generation(void);

// Total / free bytes of the mounted card (either out-param may be NULL). False if unmounted.
bool nv_sd_info(uint64_t *total_bytes, uint64_t *free_bytes);

// The VFS base path ("/sdcard").
const char *nv_sd_mount_point(void);

// --- Removal-safe file sessions ------------------------------------------------------------------
// The card can be pulled (or the SDMMC bus can blip) at ANY instant. The hot-plug monitor must not
// free the FATFS volume out from under an in-flight fread/fwrite, or a decode/stream loop holding a
// FILE* dereferences a torn/freed card object -> crash. So bracket every card access with a session:
// while any session is open the monitor DEFERS the unmount until the handles drain (bounded wait).
//
// Prefer the fopen/fclose wrappers — they bind the session to the handle lifetime, so EVERY exit
// path (including early-return fcloses) releases it automatically. Sessions do NOT block card I/O;
// they only make removal wait its turn. `nv_sd_generation()` still bumps only on a COMPLETED
// remount, so a loop that outlives a card swap can poll it and bail.

// Open a file on the card as a removal-safe session. Returns NULL if the card is gone or a removal
// is already draining (treat identically to a failed fopen). Pair with nv_sd_fclose — never a bare
// fclose, or the session leaks and the card can never unmount.
FILE *nv_sd_fopen(const char *path, const char *mode);

// Close a handle from nv_sd_fopen: ends the session, then fcloses. Returns fclose()'s result;
// NULL is a no-op returning 0.
int nv_sd_fclose(FILE *f);

// Low-level refcount, for accesses that can't use the fopen/fclose pair (opendir loops, mmap, a
// multi-file transaction). begin() returns false when the card is gone / a removal is draining — do
// NOT touch the card if so. EVERY successful begin() must be balanced by exactly one end(). Cheap
// (one short mutex); safe to nest as long as calls balance. Do not hold a session across a blocking
// wait on another task that might itself need to touch the card.
bool nv_sd_session_begin(void);
void nv_sd_session_end(void);

#ifdef __cplusplus
}
#endif
