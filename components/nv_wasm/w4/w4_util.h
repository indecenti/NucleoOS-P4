// w4_util.h — the little-endian accessors the vendored WASM-4 sources expect (their util.c also
// carries xmalloc helpers we don't need). Byte-wise, so unaligned guest pointers are fine.
#pragma once

#include <stdint.h>
#include <string.h>

static inline uint16_t w4_read16LE (const void* ptr) {
    const uint8_t* b = (const uint8_t*)ptr;
    return (uint16_t)(b[0] | (b[1] << 8));
}

static inline uint32_t w4_read32LE (const void* ptr) {
    const uint8_t* b = (const uint8_t*)ptr;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static inline double w4_readf64LE (const void* ptr) {
    double d;
    memcpy(&d, ptr, sizeof d);   // RISC-V is little-endian
    return d;
}

static inline void w4_write16LE (void* ptr, uint16_t value) {
    uint8_t* b = (uint8_t*)ptr;
    b[0] = (uint8_t)value;
    b[1] = (uint8_t)(value >> 8);
}

static inline void w4_write32LE (void* ptr, uint32_t value) {
    uint8_t* b = (uint8_t*)ptr;
    b[0] = (uint8_t)value;
    b[1] = (uint8_t)(value >> 8);
    b[2] = (uint8_t)(value >> 16);
    b[3] = (uint8_t)(value >> 24);
}
