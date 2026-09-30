# Architecture

NucleoOS P4 is an ESP-IDF application: FreeRTOS underneath, C and C++ components on top, LVGL 9
for the interface, and a WebAssembly runtime for third-party apps. This page is the map; each
component's header file opens with a comment that explains its design in detail.

```
            ┌──────────────────────────────────────────────────────────────────┐
  Apps      │ native apps (nv_apps)            WASM apps from the SD card       │
            │ Settings, Files, Camera, Music…  (store, games, terminal tools)  │
            ├──────────────────────────────────┬───────────────────────────────┤
  Platform  │ SystemUI: launcher, shade,       │ nv_wasm: WAMR interpreter/AOT, │
            │ gestures, lock screen (nv_ui)    │ "nv" host ABI, WASI, permissions│
            ├──────────────────────────────────┴───────────────────────────────┤
  Services  │ nv_web (REST + WebSocket)  nv_ota  nv_appstore  nv_auth  nv_mqtt │
            │ nv_media  nv_vplayer  nv_camera  nv_tts  nv_anima  nv_secondscreen│
            ├──────────────────────────────────────────────────────────────────┤
  Kernel    │ nv_kernel: config store (encrypted NVS), event bus, service       │
            │ manager, memory broker, background worker, logs, crash capture    │
            ├──────────────────────────────────────────────────────────────────┤
  HAL       │ nv_hal: display compositor, touch, audio codec, SD, Wi-Fi via the  │
            │ C6, USB host, 2D engines (PPA, JPEG), RTC, backup                  │
            ├──────────────────────────────────────────────────────────────────┤
  ESP-IDF   │ FreeRTOS (2 cores, 360 MHz), drivers, lwIP, mbedTLS, esp_hosted    │
            └──────────────────────────────────────────────────────────────────┘
```

## Boot

`main/app_main.cpp` brings the system up in dependency order:

1. Logging and the interrupt-storm watchdog. A crash from the previous boot is reported first.
2. Event bus, then the **config store** (`nv_config`). It mounts NVS encrypted, and migrates a
   plaintext store on the first boot after an upgrade.
3. SD card, settings backup restore when NVS was wiped, paired web sessions, language, theme.
4. Service manager, memory broker, icons (inflated into PSRAM).
5. OTA bookkeeping. A new image must survive 60 s before it is marked valid; otherwise the
   bootloader rolls back.
6. Network services (Wi-Fi, Ethernet, clock), then display, audio, the app registry and the
   SystemUI.
7. Background services that idle until the network is up: web console, MQTT, Bluetooth, USB host.

## Memory

The P4 has 768 KB of internal SRAM and 32 MB of PSRAM. Internal RAM is the scarce resource:
DMA, Wi-Fi buffers and any task that writes flash need it.

- **One app runs at a time.** Opening an app closes the previous one; the RAM belongs to the app
  in front.
- The **memory broker** (`nv_memory_broker`) asks registered caches (image caches, file mirrors,
  WASM module caches) to release PSRAM before a heavy consumer such as the camera starts.
- Large buffers, the LVGL pool and most task stacks live in PSRAM. Tasks that write flash keep
  internal stacks, because the flash cache is off while they run.
- CI fails the build when internal static RAM or IRAM exceed their budgets
  (`tools/ci/check_budgets.py`).

The reasons behind each of these rules are in [`docs/ENGINEERING_RULES.md`](docs/ENGINEERING_RULES.md).

## Display

A 1024×600 MIPI-DSI panel (JD9165). `nv_disp` keeps two framebuffers and swaps them at vsync, so
LVGL never draws into the buffer being scanned out. Video bypasses LVGL: `nv_vplayer` decodes into
a ring of frames and hands them to a compositor layer at their presentation time. The PPA scales
and rotates in hardware, and the hardware JPEG codec decodes photos and MJPEG video.

## Apps

- **Native apps** (`components/nv_apps`) are C++ descriptors: identity, icon, RAM budget and a
  `build()` that creates LVGL widgets inside the app's screen.
- **WASM apps** live on the SD card (`/sdcard/apps/<id>`). They run in WAMR, compiled ahead of time
  when an `.aot` file is present, and reach the system only through the `nv` host ABI: a drawing
  surface, input, audio, voice, files and network. What an app may use is declared in its
  manifest, and sensitive rights can be revoked per app. The C SDK is in [`sdk/`](sdk/).
- **WASI console programs** (Lua, JavaScript, SQLite, ...) run inside the Terminal app.

## Network

- Wi-Fi goes through the ESP32-C6 co-processor over SDIO (`esp_hosted`, `esp_wifi_remote`).
- `nv_web` serves the browser companion from the SD card and a REST + WebSocket API. Every API call
  needs a session paired with a code shown on the screen (`nv_auth`).
- Updates: `nv_ota` fetches a manifest signed with ECDSA P-256 and installs only a newer image whose
  hash, size and version match it. `nv_appstore` installs signed app packages.

Security as a whole is described in [`SECURITY.md`](SECURITY.md).

## Storage

- **NVS** (internal flash) holds settings and secrets, encrypted with keys derived from an eFuse key.
- The **SD card** (FAT) holds apps, media, the web companion, voice packs, and a sealed mirror of
  the settings.

## Repository

| Path | What |
|---|---|
| `main/` | boot sequence |
| `components/nv_*` | the OS, one component per subsystem |
| `components/vertice` | 3D software rasteriser used by games |
| `apps/` | WASM app sources and their built modules |
| `sdk/` | C SDK for WASM apps |
| `ports/` | build scripts for third-party programs ported to WASI |
| `sd/web/` | browser companion (served by the device) |
| `tests/host/` | host unit tests and fuzzers |
| `tools/` | release, signing, asset generation, hardware-in-the-loop tests |
| `docs/` | design notes, status, rules |

Design notes kept for history are in [`docs/archive/`](docs/archive/).
