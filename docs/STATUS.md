# Feature status

What works, how we know, and what is still unproven. Updated with each release; last review
2026-09-30 (firmware 1.1.133 plus the security work queued for the next release).

**Verified** means it was run on the real board (Guition JC1060P470C) and checked, with the firmware
version where that happened. **Tested** means automated tests or fuzzers cover it (see
[`tests/host`](../tests/host)). **Works** means it is in daily use but has no formal check.
**Experimental** means it ships but has not been proven on hardware; expect rough edges.

Every release also passes `tools/hil/smoke.py` on the board: each app is opened and closed while
the tool watches for reboots, freezes and memory leaks (22/22 apps on 1.1.113). That proves an app
starts and stops cleanly, not that every feature inside it is right.

## System

| Feature | Status | Notes |
|---|---|---|
| Boot, PSRAM broker, cache reclaim before heavy work | Verified (1.1.115) | |
| A/B OTA with a 60 s survival gate and rollback | Verified (1.1.57) | |
| Signed OTA (ECDSA P-256 manifest, sha256 + size + version checked before the boot switch) | Verified (1.1.123), tested | |
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
| Web hardening: Host and Origin checks, security headers | Verified (next release) | |
| Signed store packages, per-app permissions | Tested | |
| Firmware from SD only with a signed manifest | Next release | |
| Lock-screen PIN with back-off, security event log | Next release | |

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
| WASM runtime and app ABI | Verified | |
| AOT-compiled apps (WAMR `wamrc`) | Verified (1.1.71) | |
| WASI console programs: Lua, JavaScript, SQLite, BASIC | Verified (1.1.99) | |
| App Store, WASM-4 games | Verified (1.1.92) | |
| Puzzles, Zork, Glulxe, CHIP-8 | Verified | |
| Vertice 3D engine and its games | Works | 28 to 37 ms per frame |
| ScummVM, Doom, Game Boy | Experimental | Not yet in a release |

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
