// host: every heap is malloc
#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_INTERNAL 2
#define MALLOC_CAP_DMA 4
static inline void *heap_caps_malloc(size_t n, unsigned caps) { (void)caps; return malloc(n); }
static inline void *heap_caps_calloc(size_t n, size_t s, unsigned caps) { (void)caps; return calloc(n, s); }
static inline void *heap_caps_aligned_alloc(size_t a, size_t n, unsigned caps) {
    (void)caps;
    return aligned_alloc(a, (n + a - 1) / a * a);
}
static inline void heap_caps_free(void *p) { free(p); }
static inline size_t heap_caps_get_largest_free_block(unsigned caps) { (void)caps; return (size_t)1 << 30; }
