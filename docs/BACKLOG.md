# Known issues / backlog (audit 2026-09-07)

Findings from the 8-agent code audit that were NOT fixed in the cleanup pass, with the reason.
Everything fixed is in the commit history; this is what remains. Severity as assessed by the
auditors (verified by reading the code; nothing hardware-tested).

## Structural (design work, not one-liners)

- **Service manager / memory broker are not wired to real services.** Only `nv_ui`'s launch
  gate calls `nv_mem_request()`, and no component registers a service, so "suspend non-essential
  services before a heavy app" is a no-op today (only the cache reclaimers run). Needs per-app
  manifests with `required_services` + real suspend/resume hooks in nv_wifi/nv_audio/nv_web/camera.
  (`components/nv_kernel/nv_memory_broker.cpp`, `nv_service_mgr.cpp`)
- **No app close hook.** `NvApp` has `build()` only; every app improvises teardown on the root
  `LV_EVENT_DELETE` and hand-rolls generation counters for bgwork jobs. Add `close()`/`on_refresh()`
  to `NvApp`, called by `close_app()` / `ui_refresh_async()` (`components/nv_ui/nv_ui.cpp`).
- **Launcher persistence by registry index.** Order/folders store indices; a changed SD scan
  order across boots scrambles the home screen. Persist app ids (strings). Uninstall is now
  remapped live, but the cross-boot case remains (`nv_ui.cpp` order_load/order_save).
- **WASM runner has no run identity.** Apps runner, game view and web hot-reload each collect/abort
  "the" run; a token from `exec_start` would make them safe (`components/nv_wasm/nv_wasm.cpp`).
- **Store install never registers a launcher tile until reboot** (`apps_app.cpp`
  `apps_register_wasm` is boot-only). Needs a dynamic registry entry on INSTALLING→READY.
- **ANIMA engine is not re-entrant**: ~60 file statics behind one try-lock that only the query
  callers honour; L1 residency is toggled from seven places. Init is now once-only and the
  mode/reset side doors take the gate, but a real engine context object is the fix.
- **ANIMA tool contract lies**: the engine narrates create_file/add_event/close_app success while
  `nv_anima_os_exec` implements only set_volume/set_brightness/open_file (open_file is real since
  nv_open: it goes through `nv_open_file_async`)
  (`components/nv_apps/anima_system.cpp`, `nucleo_anima.c` ~1304-2231). Either implement or gate
  those tools out with an honest reply.
- **Web API auth**: none. `/api/wifi/join` + plain-HTTP unsigned OTA = LAN takeover. Implement the
  documented `web_token` and `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` + https manifests.
- **`teacher.json` (provider keys) is served by `/api/fs/read`** because the browser copilot reads
  it. Move browser-direct turns to device-exec, then deny the path (`nv_web.cpp` map_fs).
- **SD removal safety is opt-in per call site**: ~100 bare `fopen/opendir` remain (recorder loop,
  camera video, OTA download, WASM assets, Recents thumb). A VFS-level refcount would cover all.
- **PCM stream ownership**: music holds the sink for the whole track (even paused); TTS/SFX now time
  out after 400 ms instead of blocking, but "duck music for voice" is still missing.
- **httpd blocks for the whole ANIMA cascade** (up to ~120 s on a black-holed network: compaction +
  chat). Needs a shared deadline object and a bounded wait / job id (`nv_web.cpp`, `online.c`).
- **mbedTLS/cJSON allocate in internal SRAM** (`CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y`,
  `SPIRAM_MALLOC_ALWAYSINTERNAL=16384`): ~40 KB per TLS handshake. `MBEDTLS_EXTERNAL_MEM_ALLOC=y`
  + `cJSON_InitHooks` to PSRAM would remove the L1-unload-before-TLS dance. Not flipped blind: the
  online tier has never run on hardware (the arbiter bug), verify it first.

## Medium

- `nv_hal/nv_sd.cpp`: deferred unmount is retried every 1.5 s with a 3 s drain; cap the deferrals.
- `nv_hal/nv_wifi.cpp:72-79`: `saved_store()` commits NVS + publishes under the wifi mutex;
  snapshot and publish after unlock. `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=2304` is tight for
  `wifi_evt` (measure high-water mark, bump to 4096).
- `nv_hal/nv_hid_host.cpp`: mouse cursor/indev not hidden on HID disconnect; keystrokes dropped when
  `lvgl_port_lock(50)` fails.
- `nv_hal/nv_hal.cpp:601-619`: `nv_hal_temp_read` lazy install races httpd vs LVGL callers.
- `nv_ui/nv_ui.cpp`: `strip_goto` commits "lpage" on every page swipe (debounce); `open_recents`
  reads 6×36 KB thumbnails synchronously; PIN entry has no retry throttling; folder rename can cut
  UTF-8 mid-sequence (`max_length` counts chars, copy counts bytes); DND chip not translated.
- `nv_keydeck`: still runs widget/app code on its own task under the port lock (8 KB internal stack
  now); marshal injections to the LVGL thread. `nv_cpu_load.cpp` duplicates `nv_sysmon_perf`.
- `nv_apps/camera_app.cpp:107,135`: photo save + video stop run synchronously on the LVGL thread
  (0.3-2 s freeze). Move to nv_bgwork with a generation guard.
- `nv_apps/music_app.cpp:76-94`, `recorder_app.cpp:528`: per-file duration probes on the LVGL
  thread at every open/tap; probe on nv_bgwork. Recorder lists the in-progress recording as
  actionable (rename/delete of an open FATFS file) — needs the active path exposed by nv_audio.
- `nv_apps/gallery_app.cpp`: viewer decode (6 MB + 8 MB raster) can overlap a thumb build's raster
  (≈23 MB transient vs 12 MB budget); sync `scan_sdcard` fallback when the bgwork queue is full
  can race a stale scan job writing `s_items`.
- `nv_apps/settings_app.cpp`: Update page rebuilds both textareas on every progress tick (typing
  wiped); `sens_scan` probes 112 I2C addresses on the LVGL thread; hard-coded IT strings in
  apps/video; baked LAN default OTA URL (`http://192.168.0.216:8080/manifest.json`).
- `nv_apps/video_app.cpp`: `redraw_now()` blits from the LVGL thread while `disp_task` may blit on
  the same PPA client; `page_deleted` spins ≤300 ms then frees the ring under a possibly-running
  blitter (`nv_vplayer` frame lease would fix both — see below).
- `nv_apps/anima_app.cpp`: `teacher_info` probes the LAN teacher over mDNS (2 s) on the UI thread
  when no key is configured (`online.c` teacher_cfg → `nucleo_anima_lan_endpoint`).
- `nv_apps/secondscreen_app.cpp`: nv_hal's touch poll task keeps reading the GT911 while the second
  screen reads it too (doubled I2C traffic; memory-safe).
- `nv_apps/diagnostics_app.cpp`: "Run WASM app" joins a pthread and `nv_crash_erase` erases flash on
  the LVGL thread (dev-only buttons).
- `nv_vplayer.c`: render-vs-release UAF — `nv_vplayer_release()` (also from `/api/video/stop`) frees
  the ring while `disp_task` may still PPA-blit a slot; needs a per-slot lease. `stop_audio_task_and_wait`
  gives up after 1 s (two `vpaudio` tasks possible). Frame publish uses four separate volatiles.
  `nv_vplayer_render/set_aspect/set_frame_cb` are dead (video_app blits via nv_hal).
  `pl_mpeg.h` fork: unchecked `PLM_MALLOC`s, 4-byte over-read slack, `plm_make_fast_vlc` uses raw
  `malloc` (6 KB internal); `pl_mpeg_phoboslab_orig.h` is an unused upstream copy.
- `nv_media.c`: `esp_audio_dec_register_default()` links every default codec (flash 6% free);
  `nvmedia`/`nvvplay` stacks could move to PSRAM behind the fps/underrun telemetry.
- `nv_camera/nv_mp4.c`: `fwrite`/`fseek` results ignored, 32-bit `ftell` (>2 GB mdat wraps).
- `nv_usb`: `CFG_TUD_VENDOR_RX_BUFSIZE` 10×512 could shrink (internal TinyUSB FIFO); frame pool
  (1.8 MB PSRAM) is allocated at boot even with no PC attached (now checked, still eager).
- `nv_wasm.cpp`: bare `fopen` for assets/saves/manifest/module (removal-safe sessions);
  `NV_WPERM_FS` parsed but gates nothing; `worker_stack_to_psram` declared twice.
- `nv_appstore.cpp`: `s_lock` held across `parse_catalog` (32 SD manifest reads) stalls the LVGL
  thread in `nv_appstore_state/count/get`; parse into a temp array and swap.
- `nv_ota.cpp`: mark-valid one-shot timer handle never deleted (one leak per OTA boot).
- `nv_anima`: `s_l1_mode` etc. read/written unlocked; `lan.c` `s_base` / `setup_shim` `s_ip` torn
  across tasks (fill a caller buffer under a `portMUX`); `units.txt` grows forever; engine input
  caps 160/256 vs the 512-B native input (truncated before matching); `HTTP_CAP` 12 KB overflow
  cools the provider; ephemerality check on 79 bytes; `strstr` prefix triggers in conv.c
  ("cosa ricordi della guerra fredda"); UTF-8 split at byte caps in conv titles; conv cap scan
  only 24 metas / orphan `.j` never pruned; "watched task" WDT branches are dead code.

## Dead code worth deleting (kept for now, no runtime cost)

- `nucleo_anima.c`: `s_online_only` + every `online_llm` branch (unreachable), `verify_claim`,
  `note_file/observe/tool_content`, `l0_legacy`; `nucleo_anima_bench.c`; `nucleo_anima_online.c`:
  `transcribe_long/slice`, `summarize_file`, `longform`, `set_online/set_compact_reply`;
  Cardputer-era comments ("PSRAM-less chip", "S3 FPU", "18 KB heap").
- `nv_hal/nv_audio.cpp:439-486` `mic_setup()` (ES7210 path, replaced by the ES8311 ADC);
  `nv_wifi.cpp:114-245` simulated backend (unreachable); `nv_usb_audio.cpp` bus "diag" watcher task.
- `nv_ui.cpp`: `nv_ui_set_back` duplicates `nv_ui_set_back_handler`; legacy `lord%d` migration;
  stale swipe-up/BOTTOM comments. `nv_i18n`: 12 unreferenced string ids (×5 languages).
- `nv_web.h` endpoint list is stale (`/api/fs?path=`, `/api/fs/dl`… don't exist); `web_token`
  documented but unimplemented; hand-rolled `json_int/json_str` next to cJSON.

## RAM ideas not yet taken

- Wallpaper keeps both orientations resident (2×1.2 MB PSRAM): free the inactive one on rotate.
- Launcher icons: 26×25.6 KB ARGB8888 in flash (665 KB) — RGB565A8 or LVGL compressed halves it.
- Notes `s_notes` (19 KB) and Files `s_ents` (14 KB) PSRAM tables never freed (per-open rule).
- `nvmedia` 12 KB / `nvvplay` 16 KB internal stacks → PSRAM (gate on the underrun telemetry).
