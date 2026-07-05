# NucleoV2 — Landscape scan: projects to learn from / reuse

Target: custom evolved OS for Guition ESP32-P4 7" (JD9165 + GT911 + ES8311/ES7210 + C6 + SD).
Stack: ESP-IDF 5.5 + C++23 + LVGL 9 + FreeRTOS. Date: 2026-07-01.

## 1. ESP32 OS / launchers / app frameworks
| Rank | Project | License | Notes |
|---|---|---|---|
| 1 | **Tactility** (TactilityProject/Tactility) | **GPLv3** | Real OS, C++23/ESP-IDF/LVGL9/FreeRTOS. YOUR board supported. ELF side-load apps from SD. Services: wifi/BLE/NTP/ESP-NOW. Best full-OS structural reference. Copyleft caveat. |
| 2 | **esp-brookesia** (espressif) | Apache-2.0 | Official HMI framework: phone/desktop system UI + app manager, app isolation/coexistence, now + AI-agent (LLM/MCP). P4/S3. Best permissive UI/app-mgr spunto. |
| 3 | **M5Apps / Mooncake** (m5stack) | MIT | C++ app-lifecycle framework. M5-hardware oriented; study Mooncake app model. |
| 4 | **bmorcelli/Launcher** | — | Multi-board OTA app-selector (boot-selector style). Simpler model. |

## 2. Dynamic / side-loaded app architectures (the innovation lever)
| Rank | Project | Model | Notes |
|---|---|---|---|
| 1 | **WAMR** (bytecodealliance/wasm-micro-runtime) | WASM sandbox | Apache-2.0. ESP-IDF supported. App-manager for remote dynamic module load. Any lang→WASM, safe/portable. Best "app store, no reflash". |
| 2 | **esp-wasmachine** (espressif) | WASM | Official WASM app machine on ESP32. |
| 3 | **Tactility elf_loader** | native ELF | Full speed, ABI-fragile. |
| 4 | MicroPython+LVGL / Berry / Espruino | script | Easiest dev, bigger runtime. M5 Tab5 runs uPy+LVGL. |

Tradeoff: WASM = sandbox+portable+safe (app-store friendly) • ELF = native perf but ABI-fragile • script = fastest dev.

## 3. Advanced LVGL 9 + ESP32-P4 PPA
- **LVGL 9.3+ PPA draw unit** (`src/draw/espressif/ppa`, `LV_USE_PPA`) — HW fill/blend/rotate/scale/mirror via P4 PPA + 2D-DMA. Experimental, working. Key for smooth 1024×600 RGB.
- **esp_lvgl_port** (in esp-bsp) — display/touch/encoder glue, PPA rotation.
- Designers: **EEZ Studio**, **SquareLine Studio**, GUITION Designer.
- Open bugs to watch: PPA tearing / dirty-area (lvgl #9046), 800×800 RGB888 perf (esphome #16873).

## 4. Drivers / BSP for exact hardware (mostly IDF managed components — no full clone needed)
- **esp-bsp** — board bring-up, esp_lvgl_port, codecs.
- **esp-iot-solution** — `esp_lcd_jd9165`, GT911 driver, tons of components.
- **esp-hosted** — C6 Wi-Fi/BT offload over SDIO (already live on board).
- **esp-codec-dev / esp-adf** — ES8311 DAC + ES7210 ADC.
- **esp_video / esp32-camera** — MIPI-CSI camera.

## 5. Connectivity / services (reusable service layers)
- **esp-hosted** (C6, integrated) • **esp-matter** (smart-home interop) • **esp-rainmaker** (cloud+provision+app) • **NimBLE** (BLE) • **ESP-NOW** (peer mesh) • **esp_https_ota/app_update** (OTA).
- Tactility already wraps wifi/BLE/NTP/ESP-NOW as OS services — copy that pattern.

## 6. UX / app-model inspiration (other MCUs)
- **InfiniTime** (PineTime, C++/FreeRTOS) — great App/Screen/System-task lifecycle + tickless low-power. Study screen manager.
- **wasp-os** (uPy watch) — simple Python app API.
- **Bangle.js/Espruino** — JS apps + **web app-loader/store** (loader.js). Best "install app from web" idea.
- **Meshtastic** — modular plugin firmware + mesh.
Transfer: app lifecycle states, window/screen stack, tickless power mgmt, web app-store/loader.

## 7. ESP32-P4 showcase / reusable code
- **espressif/esp-dev-kits** — P4 Function EV demos (display/camera/brookesia phone). JD9165 init came from here.
- **waveshareteam/esp32-p4-platform** — broad examples (GPIO, wifi, SD, audio, display, LVGL, camera, brookesia).
- **M5Stack Tab5 UserDemo** — P4 + brookesia + uPy/LVGL.
- **XiaoZhi** — P4 voice assistant (audio+AI+display).
- **Elecrow CrowPanel** / ESPHome configs.
- esp32p4-rv32ima — Linux-on-P4 emulator (curiosity, impractical).

## Recommendation — reference stack (cloned to ./reference/)
- OS structure: **Tactility** (full OS, your board) + **esp-brookesia** (official UI/app-mgr, Apache-2.0).
- App model / innovation: **WAMR + esp-wasmachine** (sandboxed WASM app store, no reflash).
- HW/drivers: **esp-bsp** + **esp-iot-solution** (also as IDF components).
- Graphics: LVGL 9.3+ with **PPA** enabled.
- UX patterns (read-only): InfiniTime + Bangle.js loader.

## The innovative thesis for NucleoV2
Combine: Tactility-style OS kernel + brookesia-grade phone/desktop UI + **WASM app store** (download & run sandboxed apps over Wi-Fi, no reflash) + AI-agent layer (brookesia LLM/MCP) + PPA-accelerated 1024×600 UI. No single existing project does all of this — that's the differentiator.

### License note (decide early)
Tactility = **GPLv3** (copyleft: forking+distributing forces open-source). esp-brookesia / WAMR / esp-* = **Apache-2.0/MIT** (permissive). If we want a closed/commercial OS, base architecture on brookesia+WAMR and use Tactility only as read-only study, NOT as a fork.
