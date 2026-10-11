# Known issues / backlog (audit 2026-09-07, re-checked 2026-10-11)

Findings from the 8-agent code audit that were NOT fixed in the cleanup pass, with the reason.
Everything fixed is in the commit history; this is what remains. Severity as assessed by the
auditors (verified by reading the code; nothing hardware-tested). Re-checked against the code on
2026-10-11: entries fixed since were removed, partly fixed ones now list only what is left.

## Structural (design work, not one-liners)

- **Service manager / memory broker are not wired to real services.** Only `nv_ui`'s launch
  gate calls `nv_mem_request()` (`nv_ui.cpp` open_app), and nothing calls `nv_service_register`, so
  "suspend non-essential services before a heavy app" is a no-op today (only the cache reclaimers
  run). Needs per-app manifests with `required_services` + real suspend/resume hooks in
  nv_wifi/nv_audio/nv_web/camera, or shrink it to the broker.
  (`components/nv_kernel/nv_memory_broker.cpp`, `nv_service_mgr.cpp`)
- **No app close hook.** `NvApp` has `build()` only; every app improvises teardown on the root
  `LV_EVENT_DELETE` and hand-rolls generation counters for bgwork jobs. Add `close()`/`on_refresh()`
  to `NvApp`, called by `close_app()` / `ui_refresh_async()` (`components/nv_ui/nv_ui.cpp`).
- **WASM runner has no run identity.** `nv_wasm_exec_start` returns a bool and abort/collect take no
  token; the Apps runner, game view, Terminal and web hot-reload each collect/abort "the" run
  (`apps_app.cpp`, `terminal_app.cpp`, `nv_web.cpp`). A token from `exec_start` would make them safe.
- **ANIMA engine is not re-entrant**: ~45 file statics (`nucleo_anima.c`, `nucleo_anima_l1.c`)
  behind one try-lock that only the query callers honour; L1 load/unload is called from ~24 sites.
  Init is once-only and the mode/reset side doors take the gate, but a real engine context object
  is the fix.
- **Store catalog is unsigned.** Every package (apps, data and content packs) is signed and checked
  file by file since 54c7922, but `store.json` itself is not: a forged catalog can hide updates or
  list fake entries (their install then fails the package signature).
- **`teacher.json` (provider keys) is served by `/api/fs/read`** (sealed at rest only) because
  browser apps read it: the copilot, `sd/web/apps/dictation`, `spreadsheet`, `groq-chat`. Move
  browser-direct turns to device-exec, then deny the path (`nv_web.cpp` map_fs).
- **SD removal safety is opt-in per call site**: `nv_sd_fopen` / `nv_sd_session_*` cover ~150 uses
  (the recorder is fixed), ~200 bare `fopen/opendir` remain: camera video (`nv_camera.c`), OTA
  download (`nv_ota.cpp`), WASM assets/saves/module (`nv_wasm.cpp`), Recents thumbs (`nv_ui.cpp`).
  A VFS-level refcount would cover all.
- **PCM stream ownership**: music holds the sink for the whole track (even paused); TTS/SFX are
  dropped after 400 ms ("sink busy") instead of blocking, but "duck music for voice" is missing.
- **httpd still blocks on some ANIMA paths.** Turns run as jobs with a bounded wait since 48bae08
  and the online turn has a deadline, but `/api/anima/query` and `GET /api/anima` hold httpd up to
  45 s, the `/api/llm` relay waits `portMAX_DELAY`, and compaction does not share the turn deadline.
- **PPA blocking transactions can hang the OS.** Every PPA caller uses `PPA_TRANS_MODE_BLOCKING`,
  which waits on `xSemaphoreTake(..., portMAX_DELAY)` (`esp_driver_ppa/src/ppa_core.c:458`):
  `nv_vplayer.c` (SRM via `nv_2d_srm`), `nv_hal.cpp` (2), `nv_disp.cpp`, `nv_ui.cpp`,
  `gallery_jpeg_hw.cpp`, `ss_engine.cpp`. `nv_2d` bounds only the wait for the lock (2 s), not the
  transaction. A configuration the driver accepts but the hardware refuses = permanent UI freeze;
  this killed the H.264 I420 render path. Switch to NON_BLOCKING + a bounded wait so a bad frame
  is dropped instead of the OS. See `docs/H264_DECODE.md`.

## Medium

- **Shared-interrupt storms not seen on hardware yet (audit 2026-09-29, the proven ones are fixed:
  see ENGINEERING_RULES §9).** `nv_irqwatch` will name the source if one ever fires.
  - I2C0: IDF 5.5.2 `i2c_master.c:672` notes the controller can re-raise ACK_ERR endlessly with a
    stuck bus; its busy-waits are tick-bounded, and the tick is frozen by the storm itself. GT911
    uses an infinite timeout (esp_lcd panel IO). Fix would need an `esp_driver_i2c` override.
  - DW_GDMA: common-register interrupt (bit 16) and ECC status (`int_st1`) are propagated at reset
    and never cleared by the driver; no code path provokes them today.
- `nv_hal/nv_sd.cpp`: deferred unmount is retried every 1.5 s with a 3 s drain; cap the deferrals.
- `nv_hal/nv_wifi.cpp` `saved_store()`: commits NVS + publishes under the wifi mutex; snapshot and
  publish after unlock. `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=2304` is tight for `wifi_evt`
  (measure the high-water mark, bump to 4096 in the defaults).
- `nv_ui/nv_ui.cpp`: `open_recents` reads 6×36 KB thumbnails synchronously on the UI thread; folder
  rename can cut UTF-8 mid-sequence (`max_length` counts chars, the copy counts bytes).
- `nv_keydeck`: still runs widget/app code on its own task under the port lock (8 KB internal stack);
  marshal injections to the LVGL thread. `nv_cpu_load.cpp` duplicates `nv_sysmon_perf`.
- `nv_apps/camera_app.cpp`: photo save (`nv_camera_save_jpeg`) and video stop run synchronously on
  the LVGL thread (0.3-2 s freeze). Move to nv_bgwork with a generation guard.
- `nv_apps/music_app.cpp` `scan_dir`, `recorder_app.cpp` `build_list`: per-file duration probes on
  the LVGL thread at every open/tap; probe on nv_bgwork. Recorder lists the in-progress recording as
  actionable (rename/delete of an open FATFS file): needs the active path exposed by nv_audio.
- `nv_apps/gallery_app.cpp`: viewer decode (6 MB + 8 MB raster) can overlap a thumb build's raster
  (≈23 MB transient vs 12 MB budget); sync `scan_sdcard` fallback when the bgwork queue is full
  can race a stale scan job writing `s_items`.
- `nv_apps/settings_app.cpp`: Update page rebuilds the body and both textareas on every progress tick
  (typing wiped); `sens_scan` probes 112 I2C addresses on the LVGL thread; hard-coded IT strings in
  `video_app.cpp`.
- `nv_apps/diagnostics_app.cpp`: "Run WASM app" joins a pthread and `nv_crash_erase` erases flash on
  the LVGL thread (dev-only buttons).
- `nv_vplayer.c`: `free_resources` ignores the frame lease (`s_hold`), so `/api/video/stop` can free
  the ring under a blit (video_app itself joins `disp_task` first); `stop_audio_task_and_wait` gives
  up after 1 s (two `vpaudio` tasks possible); frame publish uses separate volatiles.
  `nv_vplayer_render/set_aspect/set_frame_cb` are dead (video_app blits via nv_hal; `set_aspect` only
  feeds the dead render). `pl_mpeg.h` fork: frame allocs are checked now, other `PLM_MALLOC`s are
  not, `plm_make_fast_vlc` uses raw `malloc` (6 KB internal); `pl_mpeg_phoboslab_orig.h` is an
  unused upstream copy.
- `nv_camera/nv_mp4.c`: `fwrite`/`fseek` results ignored, 32-bit `ftell` (>2 GB mdat wraps). The MP4
  path is inert on this chip revision but still built.
- `nv_usb`: `CFG_TUD_VENDOR_RX_BUFSIZE` 10×512 could shrink (internal TinyUSB FIFO); frame pool
  (1.8 MB PSRAM) is allocated at boot even with no PC attached.
- `nv_wasm.cpp`: bare `fopen` for assets/saves/manifest/module (use removal-safe sessions);
  `worker_stack_to_psram` declared twice.
- `nv_anima`: `s_l1_mode` read/written unlocked (`nucleo_anima_l1.c`); `setup_shim` returns a static
  `s_ip` across tasks; `units.txt` grows forever (append per learn, no dedupe); engine input caps
  160/256 vs the 512-B native input (truncated before matching); an oversize 200 reply (> 32 KB
  `HTTP_CAP`) still cools the provider for 15 s; ephemerality check on 79 bytes; `strncmp` prefix
  triggers in conv.c ("cosa ricordi della guerra fredda"); UTF-8 split at byte caps in conv titles;
  conv cap scan sees only 24 metas and orphan `.j` are never pruned; "watched task" WDT branches in
  `online.c` are dead code (nothing calls `esp_task_wdt_add`).

## Dead code worth deleting (kept for now, no runtime cost)

Removed: the ES7210 `mic_setup()` path, the simulated Wi-Fi backend, the 40 px logo, the ANIMA
`verify_claim` / `bench` / long-form transcription leftovers. The USB bus "diag" watcher is now
opt-in (`-DNV_USB_DIAG=1`). Still open:

- `nv_ui_set_back` is an inline alias of `nv_ui_set_back_handler`, kept for `anima_app.cpp`; drop it
  once that app is migrated. `nv_ui.cpp`: legacy `lord%d` launcher-order migration (drop once no v1
  install is left); stale swipe-up/BOTTOM comments. `nv_i18n`: ~29 unreferenced string ids (×5
  languages; the BT_KIND ids are used by arithmetic in settings_app).
- Hand-rolled `json_int/json_str` in nv_web_util next to cJSON.

## RAM ideas not yet taken

- Wallpaper keeps both orientations resident (2×1.2 MB PSRAM): free the inactive one on rotate.
- Notes `s_notes` (19 KB) and Files `s_ents` (14 KB) PSRAM tables never freed (per-open rule).
- `nvmedia` 12 KB internal stack → PSRAM: the prebuilt AAC/FLAC decoders run near real time on
  it, so move it only with the `pipe: out=` telemetry in hand (`nvvplay` moved; check the MPEG-1
  decode-time log).
- `ota_watch` (8 KB) could take a PSRAM stack if `nv_fwup_hash_partition` used
  `esp_partition_mmap` instead of `esp_partition_read` (the up-to-date path hashes the slot).

## GUI speed: the first frame (worth doing, needs a soak test)

Measured 2026-10-05 on 1.2.30 with the launch log line `'<app>' ready in N ms: build M ms`
(open_app to the first finished frame): apps show up in 133-401 ms, and the larger part is the
first frame, not the build (Diagnostics: build 23 ms, ready 133 ms; Settings: 113 / 370 ms). LVGL
draws in software straight into the PSRAM framebuffer (DIRECT mode), and the P4 has no LVGL SIMD
blend routines (esp_lvgl_port ships them for esp32/esp32s3 only). Already done: draw threads at
prio 4, touch 10/16 ms, no app slide, FatFs folder scan, shared Start styles, idle icon prewarm.

Two levers, each to be measured with the same log line and soaked for hours before a release:

- **L2 cache 128 -> 256 KB** (`CONFIG_CACHE_L2_CACHE_256KB`): rendering is PSRAM-bound, a larger
  L2 should cut the first frame noticeably. Costs 128 KB of internal SRAM (290 KB free at idle
  today): watch esp_hosted / Wi-Fi under load, the SRAM-famine crash of 1.1.120 is the risk.
- **LVGL's PPA draw unit** (`CONFIG_LV_USE_PPA`, LVGL 9.5): hardware fills and image blits.
  Risk: PPA waits forever when a transaction stalls (video player history); needs the bounded wait
  of the PPA entry above before it can be default-on.
- Smaller, safe: Recents thumbnails and the wallpaper JPEG are still read on the UI thread on first
  use; the two LVGL draw-thread stacks (2 x 8 KB internal) could move to PSRAM once their
  high-water mark is measured.
