// nv_memory_broker — NucleoOS Anima RAM manager (the heart of the OS).
// Before launching a RAM-heavy app: suspend non-essential services (keeping the ones the app
// needs), verify enough free RAM, and refuse if it won't fit. On close: restore what was
// suspended. A low-memory watchdog publishes NV_EV_LOW_MEMORY under pressure.
#pragma once
#include <stddef.h>
#include "nv_service_mgr.h"

#ifdef __cplusplus
extern "C" {
#endif

void nv_mem_broker_init(void);

// Free heap by tier.
size_t nv_mem_free_internal(void);  // hot/small allocations (SRAM)
size_t nv_mem_free_psram(void);     // big buffers (32MB OCT PSRAM)
size_t nv_mem_largest_internal(void);

// Reserve RAM for a foreground app: suspend non-essential services except those in `keep`,
// then check that (free internal + free PSRAM) >= `budget`. Returns true if the budget is met.
// Always pair a true OR false result with nv_mem_release() once the app exits (release is safe
// to call even after a false request — it just resumes whatever was suspended).
bool nv_mem_request(size_t budget, const nv_service_id_t *keep, int keep_n);

// Resume services suspended by the last nv_mem_request().
void nv_mem_release(void);

// Cache reclaim: components holding big REBUILDABLE PSRAM caches (ANIMA L1 file mirrors, WASM
// canvas/image caches, ...) register a flusher. When nv_mem_request() falls short of the budget
// it runs the flushers (registration order) and re-checks after each, so a cache-fat device can
// still open the camera's 4×4 MB frame pool. A flusher returns the bytes it actually freed and
// MUST be safe from any task (fail-closed: return 0 when its owner is busy, never corrupt).
typedef size_t (*nv_mem_reclaimer_fn)(void *arg);
void nv_mem_reclaimer_add(const char *name, nv_mem_reclaimer_fn fn, void *arg);

// Low-memory watchdog threshold on free internal heap (bytes). Default 48 KB.
void nv_mem_set_low_threshold(size_t bytes);

#ifdef __cplusplus
}
#endif
