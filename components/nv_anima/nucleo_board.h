// nucleo_board.h — NucleoV2 shim for the Anima engine (ported from the Cardputer firmware).
// The engine consumes only the SD mount prefix and the HLOG diagnostic macro from the original
// board header; pins/peripherals live in nv_hal and are not the engine's business.
#pragma once

#include "esp_log.h"

// VFS prefix where the knowledge pack lives: /sdcard/data/anima/... (same layout the web
// companion engine expects, so native and browser tiers share one pack).
#define NUCLEO_SD_MOUNT "/sdcard"

// High-frequency diagnostic tracing. Define NUCLEO_HEAPLOG=0 to compile every HLOG() to nothing;
// essential one-liners in the engine stay ESP_LOGI and are unaffected.
#ifndef NUCLEO_HEAPLOG
#define NUCLEO_HEAPLOG 1
#endif
#if NUCLEO_HEAPLOG
#define HLOG(tag, ...) ESP_LOGI(tag, __VA_ARGS__)
#else
#define HLOG(tag, ...) ((void)0)
#endif
