// pl_mpeg (the firmware's fork in components/nv_vplayer) built for the PC. The allocator mimics the
// device's memory ceiling instead of the PC's gigabytes: the P4 has ~20 MB of PSRAM for a clip, so a
// header announcing a 4095x4095 picture fails its frame allocations there. Without the ceiling a
// fuzzer on the PC would never exercise those failure paths.
#include <malloc.h>
#include <stdlib.h>

#define HOST_PLM_BUDGET ((size_t)20 << 20)
static size_t s_plm_used;

static void *host_plm_malloc(size_t n) {
    if (n > HOST_PLM_BUDGET - s_plm_used) return NULL;
    void *p = malloc(n);
    if (p) s_plm_used += malloc_usable_size(p);
    return p;
}
static void host_plm_free(void *p) {
    if (!p) return;
    s_plm_used -= malloc_usable_size(p);
    free(p);
}
static void *host_plm_realloc(void *p, size_t n) {
    const size_t old = p ? malloc_usable_size(p) : 0;
    if (n > old && n - old > HOST_PLM_BUDGET - s_plm_used) return NULL;
    void *q = realloc(p, n);
    if (q) s_plm_used = s_plm_used - old + malloc_usable_size(q);
    return q;
}

// The device build is not __LINUX__: its file buffers are 4 KB, not the 128 KB a PC build picks.
#define PLM_BUFFER_DEFAULT_SIZE (4 * 1024)
#define PLM_BUFFER_HIGHWATER    ((PLM_BUFFER_DEFAULT_SIZE * 3) / 4)
#define PLM_MALLOC(sz)      host_plm_malloc(sz)
#define PLM_FREE(p)         host_plm_free(p)
#define PLM_REALLOC(p, sz)  host_plm_realloc((p), (sz))
#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"
