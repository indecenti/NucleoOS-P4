# Known issues / backlog (audits 2026-09-07 and 2026-10-03, re-checked 2026-10-20)

Findings from the code audits that were NOT fixed, with the reason. Everything fixed is in the
commit history; this is what remains. Severity as assessed by the auditors (verified by reading the
code; nothing hardware-tested). Re-checked against the code on 2026-10-11: entries fixed since were
removed, partly fixed ones now list only what is left. The 2026-10-03 audit (5 agents) was merged
on 2026-10-20: its fixes and dead-code removals were ported, and its open findings that still hold
in today's code were added below.

## Structural (design work, not one-liners)

- **ANIMA engine is not re-entrant**: ~45 file statics (`nucleo_anima.c`, `nucleo_anima_l1.c`)
  behind one try-lock that only the query callers honour; L1 load/unload is called from ~24 sites.
  Init is once-only and the mode/reset side doors take the gate, but a real engine context object
  is the fix.
- **Store catalog is unsigned.** Every package (apps, data and content packs) is signed and checked
  file by file since 54c7922, but `store.json` itself is not: a forged catalog can hide updates or
  list fake entries (their install then fails the package signature).
- **Recovery has no downgrade protection**: it installs any correctly signed `nvupd/next.jsn`
  whatever its version; only the running firmware checks "newer" (`nv_ota.cpp`), recovery's
  `run_update` does not (`recovery/main/recovery.cpp`).
- **Provider keys are plaintext on the SD card of a board without settings encryption.** Since
  the key vault (`nucleo_anima_vault.c`) no API, shell command or WASI program reads
  `teacher.json`, and pages use the keys only through the `/api/llm` relay, which adds them. On
  a board without the eFuse key, though, `nv_sealed` stores the file in clear: whoever takes
  the card out has the keys. Pages on the paired origin can still spend them (not read them).
- **SD removal safety is opt-in per call site**: `nv_sd_fopen` / `nv_sd_session_*` cover ~150 uses
  (the recorder is fixed), ~200 bare `fopen/opendir` remain: camera video (`nv_camera.c`), OTA
  download (`nv_ota.cpp`), WASM assets/saves/module (`nv_wasm.cpp`), Recents thumbs (`nv_ui.cpp`).
  A VFS-level refcount would cover all.
- **PCM stream ownership**: music holds the sink for the whole track (even paused); TTS/SFX are
  dropped after 400 ms ("sink busy") instead of blocking, but "duck music for voice" is missing.
- **httpd still blocks on some ANIMA paths.** Turns run as jobs with a bounded wait since 48bae08
  and the online turn has a deadline, but `/api/anima/query` and `GET /api/anima` hold httpd up to
  45 s, the `/api/llm` relay holds its httpd worker for the whole streamed answer (one relay at a
  time, up to 180 s), and compaction does not share the turn deadline.

## Security (audit 2026-10-03, still open)

- **WASM network policy, residual gaps.** `check_dest` (`nv_wasm_net.cpp`) resolves the host once
  and the HTTP client resolves it again: a TTL-0 DNS answer can switch public -> private between
  the two (DNS rebinding); connecting to the checked IP would close it. The ABI v5 UDP imports
  (`nv.net_send/bcast`) reach any LAN address with "net" alone: that is their documented purpose
  (LAN multiplayer), but it predates the net/lan split — decide whether UDP should need "lan".
- **KeyDeck PIN**: a wrong PIN only drops the connection (no delay, no lockout, plain TCP), so a
  numeric PIN falls to rapid reconnects (`nv_keydeck.cpp` `handle_line`). Reuse nv_auth's lockout.
- **`POST /api/clipboard` file paths** are prefixed with `/sdcard` and nothing else: no `map_fs`
  (`..`, `settings.nvb`, `web` guards), and `/mnt/usbN` paths are mangled (`nv_web.cpp`).
- **LAN teacher re-link**: when the configured LAN model server stops answering,
  `nucleo_anima_teacher_relink` re-points `teacher.json` to ANY host on the subnet that lists a
  model of the same name; on a shared network another machine can capture the chats.
- **Cast `GET /api/state`** is unauthenticated with `Access-Control-Allow-Origin: *` and embeds the
  peer / client label unescaped (`ss_cast.cpp` `send_state_json`).
- Debug endpoints ship in release builds (paired clients only): `/api/bench/nvs` (up to 100k flash
  reads, blocks the server), `/api/bench/sd` (reads up to 256 MB), `/api/audio/selftest` (a GET
  that records the mic). Gate them behind a developer switch.
- Session revoke by index (`nv_auth_session_revoke`) can hit the wrong client if a pairing evicts
  one in between. `NV_WPERM_CAMERA`/`MIC` gate nothing yet are shown before install.

## Medium

- **Shared-interrupt storms not seen on hardware yet (audit 2026-09-29, the proven ones are fixed:
  see ENGINEERING_RULES §9).** `nv_irqwatch` will name the source if one ever fires.
  - I2C0: IDF 5.5.2 `i2c_master.c:672` notes the controller can re-raise ACK_ERR endlessly with a
    stuck bus; its busy-waits are tick-bounded, and the tick is frozen by the storm itself. GT911
    uses an infinite timeout (esp_lcd panel IO). Fix would need an `esp_driver_i2c` override.
  - DW_GDMA: common-register interrupt (bit 16) and ECC status (`int_st1`) are propagated at reset
    and never cleared by the driver; no code path provokes them today.
- `nv_hal/nv_sd.cpp`: deferred unmount is retried every 1.5 s with a 3 s drain; cap the deferrals.
- `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=2304` is tight for `wifi_evt` (measure the high-water
  mark, bump to 4096 in the defaults).
- `nv_hal/nv_hid_host.cpp`: `mouse_setup_task` keeps the device handle for ~1 s with no check that
  the mouse is still there (use after free on unplug). The mouse parser duplicates `nv_hidr_parse`.
- `nv_hal/nv_rtc.cpp`: `persist_cb` does blocking I2C on the esp_timer task (shared bus with touch
  and codec, §3); its timer is stopped but never deleted.
- `nv_web`: `h_fs_write` and `h_web_put` write in place (a dropped upload truncates the file: use the
  temp-file rule, §5).
- `nv_ui/nv_ui.cpp`: `open_recents` reads 6×36 KB thumbnails synchronously on the UI thread; folder
  rename can cut UTF-8 mid-sequence (`max_length` counts chars, the copy counts bytes); the
  launch-counter key `lu_%.11s` makes ids sharing 11 leading characters share a counter.
- `nv_apps/anima_app.cpp`: the attachment-thumbnail callback tests `lv_obj_is_valid()` on a holder
  that may be freed and reused by then (memory-safe, may land in the wrong object).
- `nv_keydeck`: still runs widget/app code on its own task under the port lock (8 KB internal stack);
  marshal injections to the LVGL thread. `nv_cpu_load.cpp` duplicates `nv_sysmon_perf`.
- `nv_apps/gallery_app.cpp`: viewer decode (6 MB + 8 MB raster) can overlap a thumb build's raster
  (≈23 MB transient vs 12 MB budget); sync `scan_sdcard` fallback when the bgwork queue is full
  can race a stale scan job writing `s_items`.
- `nv_apps/settings_app.cpp`: `sens_scan` probes 112 I2C addresses on the LVGL thread; hard-coded IT
  strings in `video_app.cpp`.
- `nv_apps/diagnostics_app.cpp`: "Run WASM app" joins a pthread and `nv_crash_erase` erases flash on
  the LVGL thread (dev-only buttons).
- `nv_vplayer.c`: `free_resources` ignores the frame lease (`s_hold`), so `/api/video/stop` can free
  the ring under a blit (video_app itself joins `disp_task` first); `stop_audio_task_and_wait` gives
  up after 1 s (two `vpaudio` tasks possible); frame publish uses separate volatiles.
  `pl_mpeg.h` fork: frame allocs are checked now, other `PLM_MALLOC`s are not,
  `plm_make_fast_vlc` uses raw `malloc` (6 KB internal).
- `nv_camera/nv_mp4.c`: `fwrite`/`fseek` results ignored, 32-bit `ftell` (>2 GB mdat wraps). The MP4
  path is inert on this chip revision but still built.
- `nv_usb`: `CFG_TUD_VENDOR_RX_BUFSIZE` 10×512 could shrink (internal TinyUSB FIFO); frame pool
  (1.8 MB PSRAM) is allocated at boot even with no PC attached.
- `nv_wasm.cpp`: bare `fopen` for assets/saves/manifest/module (use removal-safe sessions);
  `worker_stack_to_psram` sets `esp_pthread_set_cfg` on the CALLING task (LVGL, terminal) and never
  restores it, so a later `pthread_create` there inherits a PSRAM stack (§2).
- `nv_ota.cpp`: `nv_ota_message` / `nv_ota_available_version` hand out strings the worker rewrites.
- `nv_wake`: with `CONFIG_NV_WAKE_ESP_SR` (off by default) the PSRAM-stack wake task maps the model
  partition (`esp_srmodel_init` -> `esp_partition_mmap`, which stops the cache): it would abort.
  Give it an internal stack before enabling ESP-SR.
- `nv_anima`: `s_l1_mode` read/written unlocked (`nucleo_anima_l1.c`); `setup_shim` returns a static
  `s_ip` across tasks; `units.txt` grows forever (append per learn, no dedupe); engine input caps
  160/256 vs the 512-B native input (truncated before matching); an oversize 200 reply (> 32 KB
  `HTTP_CAP`) still cools the provider for 15 s; ephemerality check on 79 bytes; `strncmp` prefix
  triggers in conv.c ("cosa ricordi della guerra fredda"); UTF-8 split at byte caps in conv titles
  and in Telegram replies (4000-byte `snprintf`: Telegram rejects the whole message);
  conv cap scan sees only 24 metas and orphan `.j` are never pruned; "watched task" WDT branches in
  `online.c` are dead code (nothing calls `esp_task_wdt_add`).

## Dead code worth deleting (kept for now, no runtime cost)

Removed: the ES7210 `mic_setup()` path, the simulated Wi-Fi backend, the 40 px logo, the ANIMA
`verify_claim` / `bench` / long-form transcription leftovers; on 2026-10-20 the 2026-10-03 batch
(nv_vplayer render/aspect/frame callback, the upstream `pl_mpeg` copy, unused TTS planner helpers
and HAL/kernel getters, `console.html`). The USB bus "diag" watcher is now opt-in
(`-DNV_USB_DIAG=1`). Still open:

- `nv_ui_set_back` is an inline alias of `nv_ui_set_back_handler`, kept for `anima_app.cpp`; drop it
  once that app is migrated. `nv_ui.cpp`: legacy `lord%d` launcher-order migration (drop once no v1
  install is left); stale swipe-up/BOTTOM comments. `nv_i18n`: ~29 unreferenced string ids (×5
  languages; the BT_KIND ids are used by arithmetic in settings_app).
- Hand-rolled `json_int/json_str` in nv_web_util next to cJSON.
- Event topics nobody subscribes to: `NV_EV_CLIPBOARD`, `NV_EV_INPUT_DEVICES`.
- `docs/AGENT_HANDOFF.md` and `docs/LOCAL_AGENT_TODO.md` describe an old working branch: move them
  to `docs/archive/` once nobody relies on them.

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
  Our own PPA jobs are bounded since 1.2.99 (`nv_2d_srm`: 1.5 s, then the PPA is retired and the
  callers fall back), but LVGL's draw unit calls `ppa_do_*` BLOCKING itself: route it through
  `nv_2d` (or patch it to wait bounded) before it can be default-on.
- Smaller, safe: Recents thumbnails and the wallpaper JPEG are still read on the UI thread on first
  use; the two LVGL draw-thread stacks (2 x 8 KB internal) could move to PSRAM once their
  high-water mark is measured.
