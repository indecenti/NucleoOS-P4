// pl_mpeg implementation TU. The single biggest software-decode lever left is WHERE the frame
// planes live: pl_mpeg's motion-compensation churns the reference-frame pixels, and PSRAM (even octal
// @200 MHz) has far higher latency than the P4's internal SRAM. So we route the big plane allocations
// (>= ~40 KB = the luma planes) into INTERNAL SRAM up to a caller-set budget, and everything else
// (chroma, the demux ring, small state) to PSRAM. Freeing accounts the budget back. Careful: internal
// SRAM is scarce and shared with the Wi-Fi RX pool (see [[sram-famine-esp-hosted]]) — the budget is
// deliberately modest and only held while a clip is open.
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"   // esp_ptr_internal
#include <stddef.h>

static size_t s_int_budget = 0;   // remaining internal-SRAM budget for frame planes (bytes)
void nv_mpeg1_set_int_budget(size_t bytes) { s_int_budget = bytes; }

static void *plm_alloc(size_t sz) {
    if (sz >= 40000 && sz <= s_int_budget) {          // a luma plane -> put it in fast internal SRAM
        void *p = heap_caps_malloc(sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (p) { s_int_budget -= heap_caps_get_allocated_size(p); return p; }
    }
    return heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
static void plm_free_(void *p) {
    if (p && esp_ptr_internal(p)) s_int_budget += heap_caps_get_allocated_size(p);
    heap_caps_free(p);
}
static void *plm_realloc_(void *p, size_t sz) {   // growth paths (demux buffer) -> PSRAM
    if (p && esp_ptr_internal(p)) s_int_budget += heap_caps_get_allocated_size(p);
    return heap_caps_realloc(p, sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

#define PLM_MALLOC(sz)      plm_alloc((sz))
#define PLM_FREE(p)         plm_free_((p))
#define PLM_REALLOC(p, sz)  plm_realloc_((p), (sz))

#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"
