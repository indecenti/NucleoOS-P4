// Host stand-in for esp_heap_caps.h (PSRAM = malloc, sealed_stubs.c).
#pragma once
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#ifdef __cplusplus
extern "C" {
#endif
void *heap_caps_malloc(size_t n, uint32_t caps);
void heap_caps_free(void *p);
#ifdef __cplusplus
}
#endif
