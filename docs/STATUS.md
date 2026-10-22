# Feature status

What works, how we know, and what is still unproven. Updated with each release; last review
2026-10-22 (firmware 1.2.155 in the tree; the last public release is 1.2.149).

**Verified** means it was run on the real board (Guition JC1060P470C) and checked, with the firmware
version where that happened. **Tested** means automated tests or fuzzers cover it (see
[`tests/host`](../tests/host)). **Works** means it is in daily use but has no formal check.
**Experimental** means it ships but has not been proven on hardware; expect rough edges.
**Untested on HW** means the doc cited in the Notes records no board check (only PC tests, or none).
**Not released** means it is in the tree but not in a public release yet.

Every release also passes `tools/hil/smoke.py` on the board: each app is opened and closed while
the tool watches for reboots, freezes and memory leaks (22/22 apps on 1.1.113). That proves an app
starts and stops cleanly, not that every feature inside it is right.

## System

| Feature | Status | Notes |
|---|---|---|
| Boot, PSRAM broker, cache reclaim before heavy work | Verified (1.1.115) | |
| Flash layout v2: a recovery app installs the update from the SD card into a 10 MB `system` slot; the signed manifest is checked again by recovery | Tested; untested on HW | [`OTA.md`](OTA.md). Replaces the A/B scheme. The doc records no board result; a release candidate was installed on the dev board when it landed (commit 7f4477ce, 1.2.37). A layout v1 board needs one reinstall from the web flasher |
| Probation (60 s, UI probe, update server must answer), crash streak, safe mode, flash safety copy (last known good) | Tested; untested on HW | [`OTA.md`](OTA.md). Host tests and fuzzers cover the policy and the codec. The board check is `update drill` plus `tools/hil/ota_safety.py` before every release; no result is recorded in the doc. The 1.1.57 A/B survival gate it replaces was Verified |
| Staged rollout, beta channel, offline backup release key | Untested on HW | [`OTA.md`](OTA.md), `tools/dist.py`; release tooling is covered by `tests/tools` |
| Signed OTA (ECDSA P-256 manifest, sha256 + size + version checked before the boot switch) | Verified (1.1.123), tested | Same check on the layout v2 path, see above |
| Settings in NVS, mirrored to the SD card | Works | |
| NVS encryption (XTS-AES, key generated in eFuse) and sealed SD mirror | Verified on a test board, not yet released | First boot migrates the old plaintext settings; enabling it is one-way per board |
| Crash capture: coredump, `/api/crash`, interrupt-storm watchdog | Verified (1.1.116) | |
| RTC + SNTP clock | Works | |

## Display and input

| Feature | Status | Notes |
|---|---|---|
| Double-buffered display, swap at vsync, tear-free video layer | Verified (1.1.119) | |
| Touch polled at 60 Hz, tap guard against swipe-launches | Verified (1.1.68) | |
| Notification shade, sliders | Verified (1.1.81) | |
| Launcher: pages, folders, dock, search, rotation | Works | |
| Multitouch (5 points) for apps | Works | System UI uses one finger |
| On-screen keyboard with input types | Works | |
| "Open with" and default apps | Experimental | |
| Classic desktop shell: desktop icons, taskbar, Start menu, tray, title bar; Settings > Display > Interface: Tablet / Classic / Automatic (Classic when a mouse and a keyboard are present) | Works on HW (basic) | Seen working on the board 2026-10-22 (local 1.2.155) while taking the README screenshots: desktop icons, taskbar, Start menu with search and games, windows with title bars, switched live with `cfg ui_classic`. Not yet a full checklist run. [`CLASSIC_UI_PLAN.md`](CLASSIC_UI_PLAN.md) v3 (2026-10-01). In the public 1.2.149; Tablet stays the default. Windows are always maximized, landscape only |
| Keyboard and mouse in both shells: Italian and US layouts, key repeat, focus navigation, global shortcuts (Win, Alt+Tab, Alt+F4...), wheel, right click, cursor | Untested on HW | [`CLASSIC_UI_PLAN.md`](CLASSIC_UI_PLAN.md) phases 1 and 2 are marked done with the same caveat |

## Connectivity

| Feature | Status | Notes |
|---|---|---|
| Wi-Fi through the ESP32-C6 (esp-hosted), reconnect, join watchdog | Verified (1.1.120) | 2.4 GHz; about 0.2 to 0.7 MB/s in practice |
| BLE scanning | Verified (1.1.129) | The C6 firmware is BLE-only: no Classic Bluetooth |
| USB audio output (UAC) | Verified (1.1.54) | |
| KeyDeck (Cardputer as a Wi-Fi keyboard) | Verified (1.0.9) | Off by default |
| Bluetooth and USB gamepads, BLE keyboards and mice | Experimental | No controller has been paired on the board yet |
| USB drives (`/usb0`..) | Experimental | No drive has enumerated yet: needs an OTG adapter |
| MQTT and Home Assistant | Experimental | Off by default; not yet run against a real broker |
| Ethernet | Experimental | PHY never confirmed on this board |

## Security

| Feature | Status | Notes |
|---|---|---|
| Web API pairing (code on screen, session token) | Verified (1.1.122), tested | |
| Web hardening: Host and Origin checks, security headers | Verified; released (in 1.2.144) | |
| Signed store packages, per-app permissions | Tested | |
| Firmware from SD only with a signed manifest | Released (in 1.2.144); untested on HW | [`OTA.md`](OTA.md), [`SECURITY.md`](../SECURITY.md); recovery verifies the signature again |
| Lock-screen PIN with back-off, security event log | Released (in 1.2.144); untested on HW | [`SECURITY.md`](../SECURITY.md) |

Details and known gaps: [`SECURITY.md`](../SECURITY.md).

## Media

| Feature | Status | Notes |
|---|---|---|
| Camera (OV02C10): live view, auto exposure / white balance, photos | Verified (1.1.91) | |
| Gallery with background thumbnails and video posters | Verified (1.1.91) | JPEG up to 512 KB |
| Video: MJPEG `.avi` with PCM audio, MPEG-1 `.mpg` | Verified, tested | MPEG-1 real time up to about 640x360 |
| Music: MP3, WAV | Verified | AAC and FLAC decoders present, not verified |
| Offline text-to-speech (Italian, English) | Verified | |
| Camera video recording | Experimental | About 5 fps |
| Voice recorder | Experimental | |
| H.264 / MP4 | Not supported | The decoder exists but hangs in the hardware colour path; off by default |

## Apps and platform

| Feature | Status | Notes |
|---|---|---|
| Settings, Files, Camera, Gallery, Video, Music | Verified | |
| Terminal with a POSIX-like shell | Verified | |
| Second Screen over Wi-Fi (NucleoCast, VNC) | Verified (1.1.93) | About 21 fps |
| Second Screen over USB | Experimental | Never tested with the cable |
| System Monitor, Tasks, Diagnostics, Notes, Calculator | Works | Smoke-tested |
| Anima assistant | Works | Cloud answers need your own API key |
| WASM runtime and app ABI | Verified | ABI 17 (full-screen terminal programs) and 18 (system downloader) are in 1.2.149; ABI 19 (canvas rotation, hardware JPEG) is not released, see below |
| AOT-compiled apps (WAMR `wamrc`) | Verified (1.1.71) | |
| WASI console programs: Lua, JavaScript, SQLite, BASIC | Verified (1.1.99) | |
| App Store, WASM-4 games | Verified (1.1.92) | |
| Puzzles, Zork, Glulxe, CHIP-8 | Verified | |
| Vertice 3D engine and its games | Works | 28 to 37 ms per frame |
| DMD Pinball: the board as the dot-matrix display of a virtual pinball cabinet (dmdext over Wi-Fi `networkstream` or over USB as a ZeDMD 128x32; nine layouts, idle clock, dot colour / shape / glow, test screen) | Verified on a real cabinet, tested | [`DMD.md`](DMD.md), [`DMD_POPPER.md`](DMD_POPPER.md): VPX 10.8.1 with VPinMAME + dmdext 2.5; firmware version not recorded. 395 unit checks and a fuzzer on the protocol parsers. In 1.2.149. Known limits: dmdext resets the board when it opens the USB port (the app blocks that while it is open; close it before flashing), a USB hub without its own supply cuts the power, over USB the DMD is 128x32 only |
| DMD Pinball: PinUP Popper touch menu (tables, wheels, showcase pictures, quit a game) through the PC bridge `tools/popper_bridge` | Verified on a real cabinet | [`DMD_POPPER.md`](DMD_POPPER.md) version 2, about 1000 tables, end to end. Needs the bridge on the PC. Not verified: the column of Popper's global favourites. In 1.2.149 |
| Fogli (Sheets): native spreadsheet, `.xlsx .xls .ods .csv .tsv .sc`, formulas in the user's language, ANIMA panel, Files integration | Works on HW (basic) | Seen working on the board 2026-10-22 (local 1.2.155) while taking the README screenshots: the start page and a template (Monthly budget) open and render in English and Italian. [`FOGLI.md`](FOGLI.md) records the PC tests (`ports/tui/test.sh`: formula notation, engine protocol, xlsx round trips). Engine is sc-im. In 1.2.149 |
| sc-im and WordGrinder: full-screen terminal programs (ABI 17, `"tty": true`), system apps with a Home tile | Experimental; known issue on HW | WordGrinder starts and edits on the board (2026-10-22, 1.2.155), but its colours come out wrong (yellow page, green margins) and the first keystrokes after the start can be lost. [`ports/README.md`](../ports/README.md): `ports/tui/test.sh` runs on the PC (tty shim, file formats, 16 MB memory cap, screens in tmux). In the Store since 2026-10-12 |
| Libri (Books): ebook reader (EPUB, FB2, CBZ comics, TXT, HTML, Markdown), themes, bookmarks, portrait, free-books catalog with downloads | Verified on the board 2026-10-21/22 (catalog downloads, portrait, comics); not released | [`BOOKS_PLAN.md`](BOOKS_PLAN.md) (`ports/books`, `apps/libri`). Not in the Store yet (needs the owner's OK); the shipped build is ABI 18. The plan's own status (2026-10-21) still says the catalog was only tried on the PC; the board results are in the commits (b808319a catalog on 1.2.149, 2aef9efc comics). Open: the third-party licences screen |
| Biblioteca (Library): offline reader for Kiwix / openZIM files (Wikipedia, Wiktionary, ...), images and formulas, "Download collections" screen | Verified (1.2.113 reader, 1.2.115 images, 1.2.142 downloads) | [`ZIM_READER_PLAN.md`](ZIM_READER_PLAN.md). Biblioteca 1.2.0 is in the Store (updated 2026-10-20) and needs the downloader firmware. Open: `zim` launched from an ANIMA chat once died with `native stack overflow` (3 times on 1.2.109, not seen on 1.2.111, cause not found). Large apps failed to start in fragmented PSRAM; fixed in commit 81503674 (in 1.2.149) |
| System downloader `nv_dl` (ABI 18): resumable, SHA-256 and per-piece checks, runs with the app closed, shell `dl` | Verified (1.2.142) | [`ZIM_READER_PLAN.md`](ZIM_READER_PLAN.md) F5: a 942 MB collection came down with one Wi-Fi drop and one reboot and matched its SHA-256. In 1.2.149 |
| ABI 19: canvas rotation by the PPA (`nv_gfx_set_rotation`) and hardware JPEG decode (`nv_jpeg_decode`, `nv_jpeg_info`) | Verified on the board with the local 1.2.155 build; not released | [`WASM_APPS.md`](WASM_APPS.md), board checks `tests/apps/rotest` + `tools/hil/rotation.py` and `tests/apps/jpgtest` + `tools/hil/jpeg.py` (PASS on 1.2.155). Needs public firmware 1.2.156 or later; rotation needs 1.2.151+, JPEG 1.2.155+. Numbers under Measured numbers |
| LÖVE compatibility layer in the Lua App engine (LÖVE 0.10 / 11 games from `main.lua`) and the 16 `love-*` games | Untested on HW | [`LUA_APPS.md`](LUA_APPS.md), [`ports/luaapp/LOVE_GAMES.md`](../ports/luaapp/LOVE_GAMES.md): the docs record PC runs only. In the Store since 2026-10-01. Not supported: shaders, meshes, particles, physics, streamed music |
| ANIMA hands-free (offline wake word, ESP-SR WakeNet) | Untested on HW; not in the default build | [`HANDSFREE.md`](HANDSFREE.md): the core (decimator, states, end of question) is unit-tested on the PC; the ESP-SR hookup "must be tried on the device". Off by default (`CONFIG_NV_WAKE_ESP_SR`), needs the ESP-SR component and a `model` partition that layout v2 does not have |
| Game Boy emulator (Peanut-GB) and 36 homebrew games (`gbh-*`) | Experimental; runs on HW | Seen working on the board 2026-10-22 (local 1.2.155) while taking the README screenshots (Tobu Tobu Girl with the touch pad). [`ports/gbhomebrew/GAMES.md`](../ports/gbhomebrew/GAMES.md): each ROM ran 1500 frames in the PC harness with scripted input, licences checked. In the Store since 2026-09-30 |
| Arduboy: 83 games rebuilt on an Arduboy2 shim | Experimental, untested on HW | [`ports/arduboy/GAMES.md`](../ports/arduboy/GAMES.md) lists licences and sources, no board run. In the Store since 2026-09-30 |
| CHIP-8 / SUPER-CHIP / XO-CHIP: 83 bundled CC0 ROMs | App Verified (see above); ROM pack untested on HW | [`ports/chip8/GAMES.md`](../ports/chip8/GAMES.md) lists licences only |
| Doom engine (doomgeneric, OPL music) and the games that run on it (32 packages) | Experimental; runs on HW | Seen working on the board 2026-10-22 (local 1.2.155) while taking the README screenshots (Freedoom: Phase 1). [`ports/doom/README.md`](../ports/doom/README.md) documents the PC harness only; the git history has on-device fixes (3a7cf3ef, c0c776ee) but no recorded verification. In the Store since 2026-09-30 |
| ScummVM 2.9.1: 8 engine modules and 8 freeware adventures (`svm-*`) | Experimental, untested on HW; known open issue | [`ports/scummvm/README.md`](../ports/scummvm/README.md): all eight games install and start on the PC host only. In the Store since 2026-10-01 (hidden for a while on 2026-09-30: the first all-in-one build did not fit in PSRAM, commit ea9c6d60). Open: on 1.2.155 the engine's ~8 MB AOT image can fail to load and the game is terminated; being investigated (reported on the test board, not yet in the docs) |

## Developer tools

| Feature | Status | Notes |
|---|---|---|
| UI automation and screenshots over HTTP (`/api/ui/*`) | Verified (1.1.99) | |
| Web companion (browser desktop, files, WebSocket) | Verified | Needs the SD card |
| HIL smoke / soak test (`tools/hil/smoke.py`) | Works | |

## Measured numbers

All measured on the board; the notes say what changed.

| What | Result |
|---|---|
| WASM benchmark, interpreter vs AOT | 714 ms vs 25 ms (about 28x) |
| MJPEG playback, 1280x720 | 29.5 fps (was 1.5 fps before reading frames with one `fread`) |
| MJPEG + PCM audio, 1024x576 | 29.9 fps, audio in sync |
| MPEG-1 decode, 320x240 | 20 to 46 fps |
| MP3 decode | 5 s of audio in 290 ms (17x real time) |
| OTA download of a full image | 55 s (was 171 s) |
| SD sequential read, 64 KB chunks | 12.2 MB/s |
| Touch polling | 60 Hz |
| Internal static RAM (`.bss`) | 40 KB (was 202 KB), checked by CI on every commit |
| Largest free PSRAM block after heavy apps | 21 MB, stable across open/close cycles |
| Comic page 1000x1383 scaled to 496x685 in Libri | About 195 ms with ABI 19 hardware JPEG (the call alone: 21 ms decode + about 140 ms scale = 164 ms), was about 1.8 s with the software decoder in WASM. Not released |
| Libri portrait flush (canvas turned by the PPA, ABI 19) | 29 to 39 ms (was about 120 ms); a page turn about 66 ms a frame (was about 150 ms). Not released |
| Libri catalog scroll, landscape (1.2.149) | About 35 ms a frame (was about 120 ms) |
| DMD Pinball draw, 128x32 | 4.5 to 5 ms on 1024x256, 13.7 ms over the whole 1024x600 panel (was 17.6 and 40 ms); USB link 60 fps with every chunk acked |
| Downloader, 942 MB collection over weak Wi-Fi (1.2.142) | 71 min at 80 to 390 KB/s, resumed from the last verified 4 MiB piece after a Wi-Fi drop and a reboot, SHA-256 equal |
