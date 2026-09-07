// nv_bgwork — the OS background worker: one low-priority task draining a small queue of
// fire-and-forget jobs, so slow best-effort I/O (SD writes, thumbnail builds) never runs on the
// LVGL thread. Jobs that must update the UI post their result back with lv_async_call and guard
// against teardown with a generation counter — the worker itself never touches LVGL objects.
//
// Contract: best-effort. Submit never blocks; a full queue drops the job (returns false) — a
// lost thumbnail is fine, a hitched close animation is not. Jobs must free their own arg.
//
// FLASH RULE: the worker's stack lives in PSRAM. Any internal-flash access — writes AND reads:
// nvs_*, esp_partition_read/write, esp_flash_*, OTA — disables the cache and asserts that the
// running task's stack is in internal DRAM (spi_flash/cache_utils.c), i.e. it aborts. Jobs must
// not touch flash at all. nv_config_* is the one exception: it detects a PSRAM stack and proxies
// the NVS operation to an internal-stack helper (see nv_config.cpp). Memory-mapped reads of the
// knowledge partitions are fine (they go through the cache like any other cached address).
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*nv_bgwork_fn)(void *arg);

// Queue fn(arg) on the background worker (task + queue created lazily on first use).
// Returns false if the queue is full or the worker couldn't start — the caller keeps ownership
// of arg in that case (free it or fall back to doing the work inline).
bool nv_bgwork_submit(nv_bgwork_fn fn, void *arg);

#ifdef __cplusplus
}
#endif
