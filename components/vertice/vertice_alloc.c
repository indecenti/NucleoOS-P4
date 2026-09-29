// vx_alloc.c — the heap every allocation inside vertice (Jet + the engine wrapper) lands in.
//
// Jet allocates through std::vector / new / malloc, and ESP-IDF serves plain allocations under
// CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (16 KB) from internal SRAM — the pool whose exhaustion kills
// the esp_hosted Wi-Fi RX path and fails DMA reservations (docs/ENGINEERING_RULES.md). A scene is
// hundreds of small vectors, so left alone it would eat that pool. The component's CMakeLists
// rewrites the library's references to new/delete/malloc/free & co. to the functions below
// (objcopy --redefine-syms, see jet_alloc.syms): everything vertice allocates comes from PSRAM, is
// counted, and is capped. Nothing outside vertice is affected.
//
// NOTE: this file is itself inside the renamed library, so it must never call malloc/free/new —
// those names ARE these functions after the rename. heap_caps_* only.
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

#define G3D_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

static const char *TAG = "vertice";
static atomic_size_t s_used;

size_t vx_mem_used(void) { return atomic_load(&s_used); }

static void *take(size_t n, size_t align) {
    if (n == 0) n = 1;
    void *p = align > 4 ? heap_caps_aligned_alloc(align, n, G3D_CAPS) : heap_caps_malloc(n, G3D_CAPS);
    if (p) atomic_fetch_add(&s_used, heap_caps_get_allocated_size(p));
    return p;
}

void *vx_malloc(size_t n) { return take(n, 0); }

void *vx_aligned_alloc(size_t align, size_t n) { return take(n, align); }

void *vx_calloc(size_t cnt, size_t n) {
    if (n && cnt > SIZE_MAX / n) return NULL;
    void *p = take(cnt * n, 0);
    if (p) memset(p, 0, cnt * n);
    return p;
}

void vx_free(void *p) {
    if (!p) return;
    atomic_fetch_sub(&s_used, heap_caps_get_allocated_size(p));
    heap_caps_free(p);
}

void *vx_realloc(void *p, size_t n) {
    if (!p) return take(n, 0);
    if (n == 0) { vx_free(p); return NULL; }
    const size_t old = heap_caps_get_allocated_size(p);
    void *q = heap_caps_realloc(p, n, G3D_CAPS);
    if (q) atomic_fetch_add(&s_used, heap_caps_get_allocated_size(q) - old);
    return q;
}

// operator new must not return NULL (Jet is built without exceptions, so the library's own code
// would dereference it). The engine keeps scenes under hard caps and checks PSRAM before it grows
// one (vertice.cpp), so reaching this is a bug or a PSRAM exhausted by something else: fail loudly.
static void *must(void *p, size_t n) {
    if (!p) {
        ESP_LOGE(TAG, "out of PSRAM (%u bytes, %u in use)", (unsigned)n, (unsigned)vx_mem_used());
        abort();
    }
    return p;
}
static void *vx_new(size_t n)                       { return must(take(n, 0), n); }
static void *vx_new_aligned(size_t n, size_t align) { return must(take(n, align), n); }
static void *vx_new_nothrow(size_t n)               { return take(n, 0); }
static void *vx_new_aligned_nothrow(size_t n, size_t align) { return take(n, align); }

// One distinct target per operator (objcopy --redefine-syms maps each old name to a unique new
// one). The trailing size / alignment / nothrow_t arguments of the delete forms are ignored.
void *vx_op_Znwj(size_t n) { return vx_new(n); }
void *vx_op_Znaj(size_t n) { return vx_new(n); }
void *vx_op_Znwj_nt(size_t n) { return vx_new_nothrow(n); }
void *vx_op_Znaj_nt(size_t n) { return vx_new_nothrow(n); }
void *vx_op_Znwj_al(size_t n, size_t al) { return vx_new_aligned(n, al); }
void *vx_op_Znaj_al(size_t n, size_t al) { return vx_new_aligned(n, al); }
void *vx_op_Znwj_al_nt(size_t n, size_t al) { return vx_new_aligned_nothrow(n, al); }
void *vx_op_Znaj_al_nt(size_t n, size_t al) { return vx_new_aligned_nothrow(n, al); }
void vx_op_ZdlPv(void *p) { vx_free(p); }
void vx_op_ZdaPv(void *p) { vx_free(p); }
void vx_op_ZdlPvj(void *p) { vx_free(p); }
void vx_op_ZdaPvj(void *p) { vx_free(p); }
void vx_op_ZdlPv_nt(void *p) { vx_free(p); }
void vx_op_ZdaPv_nt(void *p) { vx_free(p); }
void vx_op_ZdlPv_al(void *p) { vx_free(p); }
void vx_op_ZdaPv_al(void *p) { vx_free(p); }
void vx_op_ZdlPvj_al(void *p) { vx_free(p); }
void vx_op_ZdaPvj_al(void *p) { vx_free(p); }
void vx_op_ZdlPv_al_nt(void *p) { vx_free(p); }
void vx_op_ZdaPv_al_nt(void *p) { vx_free(p); }
