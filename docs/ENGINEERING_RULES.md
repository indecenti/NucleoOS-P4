# NucleoOS Anima — engineering rules (ESP32-P4)

Invariants every change must respect. They come from real crashes and audits; the "why" is
recorded so nobody relaxes them by accident. Keep this file short and true.

## 1. Memory tiers

- **Internal SRAM is the scarce tier** (~200-240 KB free at runtime with Wi-Fi, LVGL and the web
  server up). PSRAM (32 MB) is plentiful. Every design question is "does this need SRAM?".
- **Static storage:** a `static` array lands in internal `.bss` by default. Tag cold/bulky statics
  with `NV_PSRAM_BSS` (`nv_kernel/include/nv_mem_attr.h`, portable: empty on the host) so they move
  to PSRAM. Allowed only for data touched from task context — never ISR-shared data, DMA
  descriptors, or buffers used while the flash cache is disabled. Fine for `fread()` targets
  (SDMMC bounces) and NVS get/set (the flash driver bounces non-internal buffers).
- **Prefer lazy allocation per open/close** for app state (allocate in `build()`, free on
  `LV_EVENT_DELETE`) over permanent statics; PSRAM heap via `heap_caps_malloc(MALLOC_CAP_SPIRAM)`.
  A plain `malloc()` under 16 KB lands in INTERNAL SRAM (`SPIRAM_MALLOC_ALWAYSINTERNAL`).
- Measure with the linker map: `python <scratch>/mapram2.py build/nucleos-anima.map ".dram1.bss"`
  (the P4 internal bss section is `.dram1.bss`, PSRAM bss is `.ext_ram.bss`).

## 2. Task stacks and flash access (the crash nobody sees coming)

- Any internal-flash access — **reads included**: `nvs_*`, `esp_partition_read/write`,
  `esp_flash_*`, OTA — disables the cache and **asserts that the running task's stack is in DRAM**
  (`spi_flash/cache_utils.c`). A task with a PSRAM stack aborts the moment it touches flash.
- Therefore: `xTaskCreateWithCaps(..., MALLOC_CAP_SPIRAM)` only for forever-running tasks that
  never touch flash (nv_bgwork, nv_lowmem, sd_mon, audio feeders, TTS, ANIMA workers, WASM sound).
  Never for self-deleting tasks (`vTaskDelete(NULL)` cannot free a caps stack).
- `nv_config_*` is the one sanctioned exception: it detects a PSRAM stack and proxies the NVS
  operation to an internal-stack helper task (`nv_config.cpp`). Everything else that needs NVS
  from a worker must marshal to the LVGL thread or a dedicated internal-stack task
  (e.g. `nv_backup` export task, OTA mark-valid task).
- Memory-mapped reads of knowledge partitions (`esp_partition_mmap`) go through the cache and are
  fine from any stack.

## 3. LVGL thread discipline

- LVGL objects/timers are touched only on the LVGL task, or under `lvgl_port_lock()` from another
  task. Post work instead of running it under a foreign-held lock when it involves an app's
  teardown/relaunch (WASM abort handshake, Recents thumbnail, SD writes):
  `nv_ui_open_app_id_async()`, `nv_ui_go_home_async()`, `lv_async_call()`.
- esp_timer callbacks run on the esp_timer task (3.5 KB stack) which also drives the LVGL tick:
  no SD/flash/blocking work there — spawn a task.
- Every app hangs its cleanup on the root `LV_EVENT_DELETE`: delete timers, cancel
  `lv_async_call`s, bump generation counters for in-flight bgwork jobs, free per-open buffers.
- Renderer traps: no `shadow`, `transform_*`, `opa` layers or `clip_corner` (P4 SW renderer hangs).

## 4. Audio

- One PCM stream owns the DAC (`nv_audio_pcm_begin_as`). MUSIC waits for it; SFX/VOICE give up
  after 400 ms. A failed `begin` means the stream mutex is NOT yours: never `pcm_end`/`flush`
  what you did not begin (`nv_audio_pcm_owner()` tells who owns the sink).

## 5. SD card

- Use `nv_sd_fopen/nv_sd_fclose` (or `nv_sd_session_begin/end` around `opendir` loops) for any
  file held open longer than an instant; the hot-unmount drain only protects counted sessions.
- Never `remove(path); rename(tmp, path)` blindly: check the writer's errors first and on a rename
  failure KEEP the temp file (it is the only good copy). See `commit_tmp()` in nv_anima.
- FATFS: LFN on, `max_files` 16, rename refuses to overwrite.

## 6. Untrusted input

- Every parser fed from the SD or the LAN (`/api/media/play`, `/api/video/play`, WASM imports,
  store/OTA manifests) computes chunk strides in 64-bit, requires monotonic progress, and bounds
  every table by the file size. Clamp every guest-supplied loop bound in WASM host imports —
  native loops are outside the opcode meter and the terminate flag.
- The web API has no auth: it is a LAN-trust model. `settings.nvb` (Wi-Fi creds) is never served;
  `teacher.json` is (the browser copilot needs it) — treat the LAN as trusted.

## 7. Opening files

- Never hard-code which app opens a file type. Open with `nv_open_file()` / `nv_open_with()` and
  register what your app handles as an `NvOpenHandler` (docs/FILE_ASSOCIATIONS.md). A handler's
  MIME list must match what the decoder really supports, and its id is persisted in the user's
  defaults: never rename it.
- An app opened on a file reads `nv_open_intent()` at the top of `build()` (it survives rebuilds);
  leaving an in-app intent view calls `nv_open_finish()`, not a hand-rolled "go back".

## 8. Config and persistence

- `sdkconfig` is fully reproducible from `sdkconfig.defaults*` (verified: 0 drift). Put every
  durable Kconfig choice in the defaults, never only in menuconfig.
- NVS keys ≤ 15 chars; launcher order/folders persist registry INDICES (registration order in
  `nv_apps.cpp` is therefore persisted state — do not reorder it).
