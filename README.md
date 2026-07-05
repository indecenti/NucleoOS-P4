# NucleoOS P4

A modern, RAM-frugal, AI-native operating system for the **Guition JC1060P420C_I**
(ESP32-P4 + ESP32-C6, 7" 1024×600 MIPI-DSI touchscreen) — LVGL UI, native + WASM apps, Wi-Fi OTA.

Part of the NucleoOS family; this is the **P4 edition** (the Cardputer build lives in its own repo).

- **Full design / roadmap:** [`PLAN.md`](PLAN.md)
- **Game platform:** [`GAMEDEV.md`](GAMEDEV.md)
- **Stack:** ESP-IDF v5.5.2 · C++23 · LVGL 9 · FreeRTOS · WAMR (WASM)
- **AI persona:** *Anima* (on-device assistant)

## Hardware
- SoC: **ESP32-P4** (+ ESP32-C6 co-processor for Wi-Fi 6 / BLE via `esp_hosted`)
- Display: JD9165 7" **1024×600**, GT911 capacitive touch
- Audio: ES8311 codec (I²S) + on-board mic; hot-plug USB-audio (UAC) output
- Storage: microSD (FATFS)

## What's inside
- **LVGL launcher** — adaptive grid, folders, wallpaper, smart dock, search
- **Native apps** — Settings, Files, Camera (H.264/MJPEG), Gallery (HW-JPEG), Music,
  Video (MJPEG/MPEG-1/H.264), Voice Recorder, System Monitor, Anima assistant, web companion
- **WASM runtime** (WAMR) — sandboxed games/tools via the ABI v2 gfx surface; C SDK in `sdk/`
- **OS services** — Wi-Fi, clock (RTC + SNTP), NVS config/backup, offline TTS voice, gestures,
  notifications, i18n (it/en/es/fr/de)
- **Wi-Fi OTA** — dual-OTA partitions, manifest-driven auto-update

## Build / flash
Requires **ESP-IDF v5.5.2**.

```powershell
# one-time per shell (adjust to your ESP-IDF install)
$env:IDF_TOOLS_PATH='D:\esp\tools'; . 'D:\esp\esp-idf-v5.5.2\export.ps1'

idf.py set-target esp32p4
idf.py build
idf.py -p COM5 flash monitor      # flash + serial (device on COM5)
```

`sdkconfig` is generated from `sdkconfig.defaults*` (kept in-tree); component-manager
dependencies are pinned by `dependencies.lock`.

## Layout
| Path | What |
|------|------|
| `main/` | boot entry (`app_main.cpp`) |
| `components/nv_*` | OS subsystems — hal, ui, kernel, apps, wasm, media, tts, anima, web, … |
| `apps/` | WASM app sources (compiled to `app.wasm`) |
| `sdk/` | WASM app C SDK (`nucleo_sdk.h`) |
| `sd/web` | web OS companion (PWA served over Wi-Fi) |
| `system/icons` | UI icon sources (build consumes the generated `components/nv_ui/generated/nv_icons.c`) |
| `tools/` | asset/voice/icon generators, OTA + sync scripts |

> `system/icons/mdi` and `system/icons/flat-color` are third-party icon repos (own git history),
> not tracked here — re-clone them only if you need to regenerate `nv_icons.c`.

## Rule
Never flash / OTA / sd-sync without an explicit request.

## License
TBD.
