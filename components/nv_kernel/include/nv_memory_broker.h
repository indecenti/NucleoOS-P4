// nv_memory_broker — NucleoOS Anima RAM manager (the heart of the OS).
// Before launching an app: verify enough free RAM, dropping rebuildable caches (registered
// reclaimers) when it falls short, and refuse the launch if it still won't fit. A low-memory
// watchdog publishes NV_EV_LOW_MEMORY under pressure.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void nv_mem_broker_init(void);

// Free heap by tier.
size_t nv_mem_free_internal(void);  // hot/small allocations (SRAM)
size_t nv_mem_free_psram(void);     // big buffers (32MB OCT PSRAM)
size_t nv_mem_largest_internal(void);

// Check RAM for a foreground app: true when (free internal + free PSRAM) >= `budget`, after
// running the reclaimers if it fell short (all of them for a heavy request, which also wants the
// free PSRAM in one piece). False = refuse the launch. Nothing is held: there is no release.
bool nv_mem_request(size_t budget);

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
