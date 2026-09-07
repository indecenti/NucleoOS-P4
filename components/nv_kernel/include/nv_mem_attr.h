// nv_mem_attr — placement attributes for cold static storage.
//
// Internal SRAM on the ESP32-P4 is the scarce tier (~200 KB free at runtime with Wi-Fi, LVGL and
// the web server up), while the 32 MB PSRAM is plentiful. Zero-initialised statics land in
// internal .bss by default, so every "static char buf[4096]" quietly eats the hot tier. Tag such
// storage with NV_PSRAM_BSS to relocate it to PSRAM (.ext_ram.bss) whenever the target allows it.
//
// Rules for what may be tagged:
//   - ONLY data touched from task context (no ISR, no DMA descriptor, no code that runs while the
//     flash cache is disabled — e.g. buffers handed to spi_flash/NVS write paths).
//   - Cold or bulky data: request/reply scratch, parser state, decoded tables, app item lists.
//   - Access is a few % slower than SRAM (cached PSRAM) — fine for anything that is not a
//     per-pixel or per-sample inner loop.
//
// Requires CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y (set in sdkconfig.defaults); without it
// EXT_RAM_BSS_ATTR is empty and the storage silently stays in internal SRAM, which is the safe
// fallback. Expands to nothing on non-ESP builds so shared/portable C (nv_anima, nv_tts) keeps
// compiling on the host.
#pragma once

#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#include "esp_attr.h"
#if defined(CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY) && defined(EXT_RAM_BSS_ATTR)
#define NV_PSRAM_BSS EXT_RAM_BSS_ATTR
#else
#define NV_PSRAM_BSS
#endif
#else
#define NV_PSRAM_BSS
#endif
