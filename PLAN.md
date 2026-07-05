# NucleoOS Anima — Architecture & Build Plan

Modern, AI-native, RAM-frugal OS for the **Guition JC1060P420C_I** (ESP32-P4 + ESP32-C6, 7" 1024×600 MIPI-DSI touch).
Product: **NucleoOS Anima**. Repo: `D:\NucleoV2`. Stack: **ESP-IDF v5.5.2 + C++23 + LVGL 9 + FreeRTOS + WAMR**.
Loosely inspired by the RAM-first discipline of NucleoOS (Cardputer Edition, `G:\Nucleo`): manifest-driven apps, host-first dev, aggressively free RAM before heavy work.

---

## 0. Hardware (recap)
Module **JC-ESP32P4-M3** (SoC ESP32P4NRW32) rev1.3 (RISC-V dual 360-400MHz + LP), 32MB OCT PSRAM @200M, 16MB flash. Display JD9165 MIPI-DSI (RST GPIO5, BL PWM GPIO23). Touch GT911 I2C 0x5D (SDA7 SCL8 INT21 RST22). Audio ES8311 DAC + ES7210 ADC (I2S LRCLK10 BCLK12 MCLK13, DOUT9, DIN48, PA GPIO11) — **onboard MIC1**, external speaker on 2P JST. **2MP MIPI-CSI camera** (ISP). SD SDMMC 4-bit (D0-3=39-42, CLK43, CMD44). **Wi-Fi6 + BLE5 (NO BT Classic/A2DP)** via ESP32-C6-MINI-1U (esp-hosted SDIO). **Ethernet 100Mbps** wired port. RTC RX8130 I2C 0x14 (CR1220 coin cell). 2D PPA graphics accel. **Two USB-C:** FS = USB-Serial/JTAG (COM5, flash/debug); HS = USB 2.0 OTG-HS (host/UAC). 2×13 pin header exposes GPIO + C6 UART pins.

---

## 1. Design principles
1. **RAM is the scarce resource, always.** Even with 32MB PSRAM, treat every KB of internal SRAM as precious (TLS, LVGL, WASM, drivers fragment it). Free before you allocate.
2. **One heavy thing at a time (solo mode).** The launcher is an app; it unloads when a foreground app runs, and relaunches on exit.
3. **Capability + permission everywhere.** WASM apps are sandboxed; they declare what they may touch.
4. **Lazy + LRU.** Nothing loads until needed (fonts, icons, translations, services); evict under pressure.
5. **AI is a system service (Anima), not a bolt-on.** Any app can ask Anima; it's memory-brokered like everything else.
6. **Host-first dev.** Test logic/services on PC; flash only to confirm. Never auto-flash.

---

## 2. Layered architecture

```
┌───────────────────────────────────────────────────────────────┐
│  APPS   Native core (Launcher, Settings, Files, Anima console) │
│         + WASM app-store apps (sandboxed, capability-gated)     │
├───────────────────────────────────────────────────────────────┤
│  ANIMA  LLM router (Claude/Groq/Gemini) · voice (mic+TTS) ·     │
│         agent · streaming — a memory-brokered system service    │
├───────────────────────────────────────────────────────────────┤
│  UI     Two-plane compositor (LVGL9+PPA): SystemUI (status bar· │
│         notification center+quick settings· gestures· HUD·      │
│         keyboard/IME· lockscreen· wallpaper) OVER one live app.  │
│         Orientation mgr (portrait/landscape)· theme engine·     │
│         launcher· widget kit                                    │
├───────────────────────────────────────────────────────────────┤
│  KERNEL Memory Broker · Service Manager · App Loader (solo) ·   │
│         Event Bus · Config store · Locale/i18n · Time · Storage ·│
│         Power governor                                          │
├───────────────────────────────────────────────────────────────┤
│  HAL    JD9165 · GT911 · ES8311/ES7210 · SDMMC · C6(esp-hosted)·│
│         RX8130 · PPA · USB  (drivers from esp-bsp/iot-solution) │
├───────────────────────────────────────────────────────────────┤
│  BASE   ESP-IDF 5.5.2 · FreeRTOS · WAMR                         │
└───────────────────────────────────────────────────────────────┘
```

---

## 3. Kernel core (the heart)

### 3.1 Memory Broker — the central innovation
Every app manifest declares: `ram_budget`, `required_services[]`, `gpu_needs`, `wants_network`, `priority`.

**Launch sequence:**
1. Read the target app's manifest.
2. Compute required free RAM = `ram_budget + safety_margin`.
3. If launching a foreground app → **unload the launcher** (solo mode).
4. **Suspend/stop non-required services** (ref-count aware) — Wi-Fi, BT, audio, Anima, mesh, etc.
5. **Evict background apps** (LRU) until budget met.
6. Verify `free_heap ≥ required`; if not → **refuse launch** with a toast (never crash).
7. Allocate the app's arena: big buffers in **PSRAM** (`nv_malloc_big`), hot/small in **SRAM** (`nv_malloc_fast`).
8. Start the app.

**Exit sequence:** free the app arena → **restore** the services/apps that were suspended (resume snapshot) → **relaunch the launcher**.

**Low-mem watchdog task:** samples `free_heap` continuously. Under threshold → evict caches (font/icon/i18n LRU), warn the foreground app (`on_low_memory`), then kill as last resort.

**Policies (user-selectable in Settings → Memory):** `aggressive` (free everything possible), `balanced` (keep cheap-to-keep services), `performance` (keep more resident when RAM allows).

### 3.2 Service Manager
Ref-counted lifecycle for every service: `start()/stop()/suspend()/resume()`, dependency graph, linger timers for expensive-to-start services (Wi-Fi/C6). A service starts only on request (app or system) and stops at refcount 0. Services: `wifi`, `eth` (100Mbps wired — network without Wi-Fi), `ble`, `audio_out`, `audio_in`, `usb_audio`, `anima`, `ntp`, `webserver`, `ota`, `camera`, `notification` (apps post notifications → shade), `input_method` (on-screen keyboard/IME), `wallpaper`, `lockscreen`, `rotation` (orientation manager). SystemUI itself (status bar, shade, gestures, HUD) is an always-resident thin layer, not broker-suspended.

### 3.3 Event Bus
Lightweight pub/sub IPC (topic → subscribers). Used for live-apply of settings (brightness, theme, locale), low-memory signals, service state, app lifecycle. No dynamic alloc on the hot path (fixed ring).

### 3.4 Config store
- **Secrets** (API keys, Wi-Fi PSK, PIN) → **encrypted NVS** (flash encryption + NVS encryption). Never on SD plain.
- **Preferences** (JSON) → `/system/config/*.json` on SD, cached in RAM only for the active screen.
- Change → Event Bus → live apply.

### 3.5 Locale / i18n service
- **Shipped languages at t0: IT, EN, ES, FR, DE (5).** Full UI text + keyboard layout + locale (date/time/number/first-day) for all five. New languages later = drop a table + layout, no code change.
- Active-locale table `key→string` from `/system/i18n/<lang>.bin` (compact binary), **only the active language in RAM**, lazy + LRU.
- Fallback chain `lang → base(lang) → en`.
- `tr("settings.wifi.title")` lookup, O(1) hashed.
- **i18n gate** (build tool): every key present in **all 5 shipped languages**, no orphans.

### 3.6 Time service
- RX8130 RTC read/write; **NTP** sync when Wi-Fi is up (via C6); apply **POSIX TZ** string (compact, no full tzdata); DST from the TZ rule. 12/24h + format from locale.

### 3.7 Storage / VFS
FAT32 on SD (`/system`, `/apps`, `/data`, `/media`, `/anima`), LittleFS/NVS on internal flash. Mounted at boot; SD hot-plug tolerant.

### 3.8 Power governor
Idle → dim → sleep; CPU freq scaling; LP-core for background wake; battery/charge state.

### 3.9 Logging & Observability (`nv_log`) — know what's wrong, fast
A first-class, always-on, RAM-frugal logging + diagnostics subsystem covering the **OS, services, and every app**. Built on ESP-IDF `esp_log` as the low-level backend, wrapped with our tag/ring/sink layer.

**Core design (lightweight):**
- **Levels + tags:** `ERROR / WARN / INFO / DEBUG / TRACE`, per-**tag** (subsystem/app) filtering, **runtime-adjustable level per tag** (no reflash). `DEBUG/TRACE` compiled out in release builds → zero cost.
- **Lock-free ring buffer in PSRAM:** the last N entries (configurable, e.g. 128–512 KB in the 32MB PSRAM) live in a lock-free ring — **no SD write on the hot path, no heap churn, no fragmentation**. Entries are compact (uptime+RTC ts, level, tag, msg, optional code/ctx); formatted only when read.
- **Sinks (pluggable, async, off the hot path):**
  - **Serial/UART** — live `idf.py monitor` on COM5.
  - **On-device viewer** — a **Diagnostics** core app + Settings → Developer → Logs: filter by tag/level, search, **live tail**, share/export. Fast "what's wrong" on the screen itself.
  - **Network stream** — WebSocket/HTTP log stream to a PC browser (the NucleoOS-Cardputer "browser as console" pattern) over Wi-Fi/Ethernet, + on-demand download.
  - **SD rotating files** — `/data/logs/*.log`, size-capped + rotated, **flushed periodically or on ERROR** (never per-line) → SD/RAM friendly.
- **Crash capture (know what killed it):** panic/`esp_coredump` handler → on crash, dump **backtrace + registers + last-N ring buffer** to a **`coredump` flash partition** and `/data/logs/crash-<ts>.log`. On next boot, **Settings → System → Crash diagnostics** (Cardputer-style) surfaces it: what crashed, where, the log tail before it. A notification flags "last boot crashed".
- **Per-app logging (WASM):** apps log via a host API `nv_log(level, tag, msg)` — **tagged by app id, sandboxed, rate-limited** (an app can't spam or blind the OS log). App logs appear in the same viewer, filterable by app.
- **Live metrics ring:** free heap (internal + PSRAM), per-service state, FreeRTOS task high-water marks, FPS, Memory-Broker events, low-mem warnings — sampled into a stats ring; shown in Settings → Memory and a toggle-able on-screen **Diagnostics overlay** (fps/heap HUD).
- **Fast triage:** ERROR events publish on the **Event Bus** → SystemUI can toast/notify immediately; a **"What's wrong now"** diagnostics screen aggregates last errors, failing/suspended services, low-mem events, and last-boot crash — one place to look.
- **Host-first:** the same `nv_log` API compiles on the PC host harness, so service/app logic is debuggable on PC before flashing (Cardputer lesson).

**RAM budget:** ring in PSRAM (sized in Settings), hot path is lock-free store (no malloc, no SD, no printf on the caller), release strips DEBUG/TRACE. Costs ~nothing when quiet, everything visible when needed.

---

## 4. UI layer — a real modern (Android-like) shell
**LVGL 9**, color depth 16, **PPA** accel (fill/blend/rotate/scale) for smooth 1024×600.

### 4.1 Two-plane compositor (reconciles "real OS UX" with solo-mode RAM)
- **SystemUI plane — ALWAYS resident, tiny footprint.** A minimal always-loaded layer drawn *over* the current app: status bar, notification center/shade, nav-gesture handler, volume/brightness HUD, toasts/dialogs, on-screen keyboard, lock screen, wallpaper. It holds no heavy content, so it survives solo-mode. This is what makes the device *feel* like an OS even though only one heavy app is resident.
- **App plane — one live app (solo mode).** The launcher and foreground apps live here; unloaded/swapped by the Memory Broker.
- The two planes composite each frame (PPA); SystemUI overlays (shade, HUD, keyboard, dialogs) draw on top without unloading the app.

### 4.2 Status bar (top, persistent)
Clock (locale format) · battery %/charging · Wi-Fi / **Ethernet** / BLE icons · volume/mute · **notification icons** · Anima indicator. Tap = open notification center.

### 4.3 Notification center + Quick Settings (Android-like shade)
- **Swipe down from top** → notification shade: app notifications (posted via the Notification service), dismiss/swipe-away, tap-to-open, grouping, priority, **Do Not Disturb**.
- **Quick Settings tiles** at the top of the shade (or swipe-down-from-top-right): Wi-Fi, Ethernet, BLE, brightness slider, volume slider, airplane, DND, rotation lock, screenshot, Anima, "Free RAM now".
- Notifications persist to `/data/notifications`; SystemUI keeps only the visible set in RAM (lazy).

### 4.4 Gesture engine (GT911 multitouch) — full set
| Gesture | Action |
|---|---|
| Swipe **up from bottom edge** | **Home** (exit app → relaunch launcher) |
| Swipe **up from bottom + hold** | **Recents** (task switcher) |
| Swipe from **left/right edge** | **Back** |
| Swipe **down from top** | **Notification center** |
| Swipe **down from top-right** (or 2-finger down) | **Quick Settings** |
| **Long-press Home** zone | **Anima** assistant |
| **Pinch / spread** (2-finger) | zoom (in-app) |
| **Double-tap status bar** | scroll-to-top / expand |
Single-finger nav gestures via LVGL indev; multi-finger (pinch, 2-finger) via a custom GT911 multi-point reader. Edge-swipe zones are configurable (Settings → Gestures) with left/right-hand and sensitivity options.

### 4.5 Recents / task switcher (RAM-aware)
Solo-mode means no live app cards. Recents shows a **metadata list** (icon + name + last-screen thumbnail snapshot saved on app exit) of recently used apps; tapping relaunches through the Memory Broker. Cheap: thumbnails are small JPEGs on SD, LRU-cached.

### 4.6 Orientation — portrait AND landscape
- **Rotation manager:** 0° / 90° / 180° / 270°. LVGL software rotation, **PPA-accelerated**; touch coordinates remapped to match; status bar, shade, gestures, keyboard all re-lay-out per orientation.
- Apps declare `supported_orientations` in their manifest; the system can **lock** or follow a global setting. Rotation-lock tile in Quick Settings.
- No IMU on this board → "auto-rotate" is **manual/tile-driven or per-app** by default; if the user attaches an I2C IMU (expansion header) the system enables true sensor auto-rotate (graceful).
- Both display orientations and both aspect handling (1024×600 landscape / 600×1024 portrait) are first-class in the widget/layout kit.

### 4.7 On-screen keyboard / IME (multilingual)
- LVGL-based **virtual keyboard**, **per-locale layouts at t0**: IT/EN/ES (QWERTY + ñ/accents), FR (AZERTY), DE (QWERTZ + umlauts); dead-keys/accents for all five; number/symbol/emoji panes.
- Smartwatch input tricks carried from NucleoOS Cardputer: **autocomplete, word history, resume, 1–9 quick-select**.
- Input Method service; apps request text input via the system (never draw their own keyboard). Keyboard layout follows Settings → Language, overridable.

### 4.8 Lock screen & security UI
Lock screen (clock, date, notifications, wallpaper) → unlock via **PIN** or swipe. Idle-lock timeout, lock-on-sleep. Managed by the Lockscreen/Security service.

### 4.9 Launcher, wallpaper, system UI bits
- **Launcher:** paged icon grid + app drawer + **global search** (apps, settings, Anima). Native app, unloaded in solo mode.
- **Wallpaper** service (home + lock), from `/system/wallpapers` or user image on SD.
- **Toasts / snackbars / modal dialogs / runtime permission prompts** (WASM capability grants) — all SystemUI, consistent styling.
- **Volume/brightness HUD** overlay on change; **screenshot** (→ `/media`).

### 4.10 Theme engine
- **Design tokens** (colors, spacing, radius, elevation, motion) → LVGL theme; **light / dark / auto (by time)**, **accent color**, **UI density**, **animations on/off**.
- **Font:** family + **size/scale** (accessibility), base Latin in flash, extended (CJK…) lazy from SD per locale.
- **Icon packs** + **wallpapers** + **themes** are asset bundles on SD (`/system/themes/<name>`), lazy + LRU. Default set = **Material Design Icons** (Apache-2.0).
- Change → Event Bus → **live apply** across SystemUI + running app (no reboot).

### 4.11 Accessibility
Font scaling, high-contrast theme, larger touch targets, reduce-motion, color-blind palettes, and **TTS screen reader** (reuses Anima/`nucleo_tts` voice).

---

## 5. Anima AI (system service)
- **LLM router** → providers: **Claude** (Anthropic Messages raw HTTP, default `claude-haiku-4-5` for cost/latency), **Groq** (OpenAI-compatible), **Gemini** (OpenAI-compat endpoint). Two adapters: `OpenAiCompatProvider` (Groq+Gemini) + `AnthropicProvider`. Streaming SSE → UI tokens.
- **Keys** in encrypted NVS; TLS via `esp_crt_bundle` (one CA bundle covers all three hosts).
- Any app can call Anima via a system API (capability-gated). Suspended by the broker when idle.

### 5.1 Voice
Two modes, chosen by connectivity + Settings:
- **Offline voice — REUSE the Cardputer TTS (`nucleo_tts`).** NucleoOS Cardputer already ships a working **concatenative offline TTS**: a pure, host-testable planner (`nucleo_tts_plan`) turns text → CLIP/PAUSE tokens (number/date/math expansion for **EN + IT**, phrase-first greedy match, letter-by-letter spelling fallback, "read it on screen" safe fallback), and pre-rendered PCM clips are concatenated at playback. **RAM ~zero, offline, natural voice.** We reuse it wholesale:
  - **Component `nucleo_tts`** (`nucleo_tts.c/.h`, `nucleo_tts_index`, `nucleo_tts_plan`) — planner is portable C, drop-in.
  - **Existing EN + IT clip corpora + dictionaries** — `index.bin` + `clips.pcm` per language, built by `build_voice.py`. Source assets in the Cardputer repo: `oversized-assets/parts/tts-en-clips/*` and `tts-it-clips/*`. Deploy to `/system/tts/<lang>/` on SD (Cardputer used `/sd/data/tts/<lang>/`).
  - **P4 upgrade:** with 32MB PSRAM we can **cache the whole `clips.pcm` in PSRAM** for instant, gapless playback (Cardputer read clips from SD on demand due to no PSRAM). Same corpus, better latency.
  - Time/number/math speak (`nucleo_tts_speak_time`, `mathspeak`) and `translate_word` come for free with the component.
  - See Cardputer `docs/tts.md` + `docs/voice.md` and `build_voice.py`.
- **Online voice (optional):** when Wi-Fi is up and the user opts in — cloud STT (mic ES7210) + cloud TTS for open-ended, out-of-corpus speech.
- **Pipeline:** ES7210 mic → (optional local wake) → STT (offline keyword / cloud) → LLM (Anima router) → **TTS: offline `nucleo_tts` by default, cloud when online+enabled** → ES8311 out.
- **Language coverage at t0:** UI/keyboard/locale = **5 langs (IT, EN, ES, FR, DE)**. **Offline voice corpora exist only for EN + IT** (reused from Cardputer). So at t0: **ES/FR/DE speak via cloud TTS** (online); offline `nucleo_tts` corpora for ES/FR/DE are generated later with `build_voice.py` (planner + runtime are already language-agnostic). If offline and no corpus for the active language → degrade to text ("read it on screen").

### 5.2 Audio hardware — mic ONBOARD, speaker EXTERNAL
Corrected from official Guition board photos:
- **Microphone = ONBOARD** ("MIC1" on the silk; not a connector) → feeds ES7210 (I2S DIN GPIO48). **STT works with no external hardware.**
- **Speaker = EXTERNAL connector** ("MX 1.25 2P speaker interface", #3) → ES8311 + PA (enable GPIO11) drives a small external 8Ω/4Ω speaker (cable not included). **OR** use USB-UAC output on the HS Type-C (see 5.3).
- I2S pins (Tactility .dts): BCLK 12, WS/LRCLK 10, MCLK 13, **DOUT 9** (playback), **DIN 48** (capture); codec control on I2C (SDA7/SCL8).
Therefore **Anima Voice is graceful/optional for OUTPUT only** (input is built-in):
- Speak → external speaker or USB-UAC; if neither present, **degrade to text** (reuse the Cardputer "read it on screen" fallback).
- Settings → Sound: output backend selector + test tone; mic level meter (mic always available).
- Voice output is never assumed; the OS is fully usable silent. Mic input is available out of the box.

### 5.3 Audio backends — I2S codec OR USB-UAC (abstracted)
Anima Voice sits behind an `AudioBackend` abstraction with two implementations, auto-selected at runtime:
- **(A) I2S codec** — onboard ES8311 (out) + ES7210 (in) driving external JST speaker/mic (see 5.2).
- **(B) USB-UAC host** — the ESP32-P4 is a **USB 2.0 OTG High-Speed host**; with the ESP-IDF/esp-iot-solution **USB Host UAC driver** it can use a standard **USB microphone / USB speaker / USB headset / USB sound card** (UAC 1.0/2.0), giving input+output over a single port with **no JST wiring**. Multi-channel 48kHz/16-bit supported; can coexist with UVC. Reference examples already cloned: `esp-iot-solution/examples/usb/host/usb_audio_player`, `usb_camera_mic_spk`, `usb_stream`.
  - **Constraints:** must use the P4 **OTG-HS** USB-C (not the internal USB-Serial/JTAG port on COM5), in host mode, sourcing **5V VBUS** — likely needs a **USB-C OTG adapter**, or a **powered USB hub** for higher-draw devices. Only one OTG-HS controller (host XOR device); JTAG/serial debug is a separate peripheral, unaffected. USB host + UAC stack has a RAM/flash cost → exposed as a memory-brokered service `usb_audio`, started only when a UAC device is attached.
- **(C) Wi-Fi audio cast (optional, later)** — stream to a networked speaker over Wi-Fi (AirPlay / DLNA / Chromecast / Snapcast / HTTP) via the C6. Real wireless output but a heavier service; a Phase 6+ nice-to-have.
- **Selection:** UAC device present → use USB; else I2S codec + external speaker/mic; else (optional) Wi-Fi cast; else degrade to text. Settings → Sound shows the active backend + device name.

**❌ Bluetooth speakers (A2DP) are NOT possible.** The only Bluetooth here is the ESP32-C6, which is **BLE-only (no Bluetooth Classic / BR-EDR)** — A2DP requires BR/EDR. **LE Audio (LC3)** on C6 is marked "Won't Do" in ESP-IDF (not implemented). So no classic BT speaker/headset for audio. BLE is still used for input peripherals (keyboards, remotes, sensors, notifications), not audio output.

---

## 5A. Web console & remote API (reuse Cardputer web stack)
NucleoOS Cardputer is **web-native** — the browser is its rich operator console (`nucleo_httpd`, `nucleo_ws`, `nucleo_webfs`, `web/shell` PWA, `schemas/`). NucleoOS Anima has its **own** rich on-device UI, so the web console is a **secondary/remote** console — admin, dev, file transfer, automation, remote control — not required for daily use, but we **reuse the Cardputer web stack** wholesale.
- **HTTP + WebSocket server** (`esp_http_server` + WS): a **memory-brokered `webserver` service**, started on demand, **PIN/token-authed**, optional TLS. Reachable over Wi-Fi **or Ethernet**.
- **Web console PWA** served from the device (reuse Cardputer `web/shell`): dashboard, **file manager** (WebFS: browse/upload/download SD), **settings mirror**, **live logs** (the `nv_log` network sink), **app store + hot-reload** (push a WASM app from the PC → runs instantly), **terminal**, device info/diagnostics.
- **REST + WebSocket API** — schema-contracted (reuse Cardputer `schemas/`) for automation/integration (Home-Assistant-style), other devices, scripts.
- **Screen mirror & remote control (P4 upgrade):** stream the LVGL framebuffer to the browser; since the P4 has a **hardware H.264 encoder**, optionally H.264-encode the screen for low-bandwidth remote viewing (Cardputer couldn't). Inject remote touch/keyboard for full remote control.
- **OTA + SD-sync over the network** (reuse Cardputer `release.ps1` flow: delta SD sync + firmware OTA) — **gated behind explicit user action, never automatic** (carried-over rule).
- Reuse from `G:\Nucleo`: `nucleo_httpd`, `nucleo_ws`, `nucleo_webfs`, `web/shell`, `schemas/`, `registry/`.

## 6. App model
- **Native core apps** compiled into the OS: Launcher, Settings, Files, Anima console, Terminal.
- **WASM apps** from `/apps/<id>/`: `app.wasm` + `manifest.json`. **WAMR** runtime, **capability-based permissions** (net, fs, camera, audio, gpio, anima). **App store** over Wi-Fi: download → verify → install, **no reflash**. Hot-reload from PC for dev.
- **Manifest schema:** `id, name, version, icon, ram_budget, required_services[], permissions[], locale_keys, entry`.

---

## 7. Settings app (native core — full spec)
Gear icon on the launcher. Manages **all** device settings. RAM-light: loads **one category screen at a time** (lazy), frees on Back. Changes flow through the Event Bus for **live apply**.

| # | Category | Manages |
|---|---|---|
| 1 | **Network** | Wi-Fi scan/connect/saved (C6), **Ethernet 100Mbps** (DHCP/static, link status), BLE (peripherals), hotspot, airplane mode |
| 2 | **Display** | Brightness (BL PWM GPIO23), screen timeout, **orientation (portrait/landscape + rotation lock)**, auto-rotate (if I2C IMU attached) |
| 2b | **Themes** | **Theme** (light/dark/auto-by-time), **accent color**, **wallpaper** (home + lock), **icon pack**, **font family + size/scale**, UI **density**, corner radius, **animations on/off**. Live-apply. Bundles from `/system/themes`. |
| 2c | **Notifications** | Per-app notification allow/priority, **Do Not Disturb** + schedule, lock-screen visibility, quick-settings tile order, sounds |
| 2d | **Keyboard & Input** | On-screen keyboard **layout per language** (QWERTY/QWERTZ/AZERTY), autocomplete/history on/off, key sound/haptic, emoji |
| 2e | **Gestures** | Enable/tune nav gestures, edge-swipe zones + sensitivity, left/right-hand, long-press-Home = Anima toggle |
| 2f | **Accessibility** | Font scaling, high-contrast, larger touch targets, reduce-motion, color-blind palettes, **TTS screen reader** (Anima voice) |
| 3 | **Sound** | **Output:** volume, mute, output backend (**I2S external speaker via ES8311+PA** / **USB-UAC**), test tone. **Input:** gain (ES7210), **mic source** (onboard MIC1 / USB-UAC / line), **▶ Mic test** (record 3s → playback + **live level meter**), noise-suppress toggle. Per-app volume. System sounds. |
| 4 | **Camera & Video** | Camera enable + live preview (2MP MIPI-CSI + ISP), resolution, fps, **ISP** (brightness/contrast/AWB/exposure). **H.264 encoder** (hardware): bitrate, GOP/keyframe interval, profile, resolution, fps. **JPEG/MJPEG** quality. Snapshot/record targets. |
| 5 | **USB & Peripherals** | OTG-HS host device list (**USB audio cards / headsets → UAC driver**, USB mass storage, USB HID, USB camera/UVC). Per-device: status, pick as audio in/out, sample-rate/channels. Host mode + VBUS state. |
| 6 | **Sensors** | Live readings + enable/calibrate for **present** sensors: camera (image), **microphone (level)**, **RTC** (RX8130), **battery** (voltage/%/charging), **chip temperature** (P4 internal). **I2C scan** tool + auto-detect + support for **external I2C sensors** on the SH1.0 I2C connector / pin header. (Board has no IMU/light/proximity — shows "no additional sensors" gracefully.) |
| 7 | **Language & Region** | Language, region, date/time format, 12/24h, first-day-of-week, units, number format |
| 8 | **Date & Time** | RTC set (RX8130), NTP on/off + server, timezone picker |
| 9 | **Anima AI** | Provider (Claude/Groq/Gemini), model, **API keys** (masked, encrypted NVS), voice on/off, wake word, offline/online TTS |
| 10 | **Storage** | SD info, internal flash, format SD, per-app data usage, clear caches |
| 11 | **Apps** | Installed list, per-app permissions, uninstall, app store, default apps |
| 12 | **Memory** | Live RAM monitor, per-service state, broker policy (aggressive/balanced/performance), **"Free RAM now"** |
| 13 | **Power** | Battery, sleep/idle timeout, CPU governor, LP-core |
| 14 | **Security** | Device PIN, encryption status, per-app permissions overview |
| 15 | **System / About** | Version, **OTA update**, factory reset, device info, licenses, **Crash diagnostics** (last-boot crash + backtrace), developer options (**live log viewer** by tag/level, log-level per subsystem, network log stream, diagnostics overlay, host mode) |

Persistence: secrets → encrypted NVS; the rest → `/system/config/*.json`. Every change publishes an event so the OS applies it immediately (brightness, theme, locale, volume, encoder params…). Each category is a plugin registering its schema/screen — new hardware = new category, no core change.

---

## 8. Storage & partitions
- **Flash 16MB:** bootloader · partition table · `factory` (OS) · `ota_0`/`ota_1` · `nvs` (encrypted) · `phy_init` · **`coredump`** (crash dumps).
- **SD (FAT32):** `/system` (i18n, fonts, icons, themes, config, default assets) · `/apps` (WASM apps) · `/data` (per-app) · `/media` · `/anima` (caches).

---

## 9. Boot flow
1. Bootloader → OS `factory` app.
2. HAL init: display (JD9165) + touch (GT911) + SD + RTC.
3. Kernel init: Memory Broker · Service Manager · Config · Locale · Time · Event Bus.
4. UI init: LVGL + PPA + theme + status bar.
5. Launch **Launcher**.
6. C6/Wi-Fi stays lazy — started only when a service needs it.

---

## 10. Dev workflow (from Cardputer lessons)
- **Host-first:** kernel logic/services compiled and unit-tested on PC where possible; LVGL PC **simulator** for UI. Flash only to confirm.
- **Gates:** config/manifest validation · **i18n completeness** · **memory-budget** check.
- **Build:** `idf.py build`; **flash:** `idf.py -p COM5 flash monitor`. **Never auto-flash/OTA without explicit user request** (carried over from NucleoOS).

---

## 11. Folder structure (`D:\NucleoV2`)
```
NucleoV2/
  CMakeLists.txt · sdkconfig.defaults · sdkconfig.defaults.esp32p4 · partitions.csv
  main/                 app_main, boot sequence
  components/
    nv_hal/             display · touch · audio · sd · c6 · rtc · ppa
    nv_kernel/          memory_broker · service_mgr · event_bus · config · locale · timesvc · storage · power · log(nv_log)
    nv_ui/              compositor · systemui(statusbar, notif_center, quick_settings, hud, toasts) ·
                        gestures · keyboard(IME) · rotation · theme_engine · wallpaper · lockscreen ·
                        launcher · widgets
    nv_anima/           llm_router · providers(anthropic, openai_compat) · voice · agent
    nucleo_tts/         REUSED from Cardputer — offline concatenative TTS (planner + index)
    nv_apps/            settings · files · terminal · anima_console · diagnostics(logs+crash)   (native core apps)
    nv_wasm/            wamr integration · loader · capabilities
    nv_web/             httpd · ws · webfs · rest_api · screen_mirror(H.264)   (REUSE Cardputer nucleo_httpd/ws/webfs)
  web/                  web console PWA (REUSE Cardputer web/shell)
  schemas/              REST/WS API JSON schemas (REUSE Cardputer schemas/)
  apps/                 WASM app sources + manifests
  system/               i18n · fonts · icons · themes · default config   → deployed to SD
    tts/<lang>/         index.bin + clips.pcm  (REUSED EN+IT clip corpora from Cardputer)
  tools/                validators · i18n gate · simulator · flash scripts
  reference/            cloned guide repos (Tactility, brookesia, WAMR, esp-bsp, iot-solution)
  docs/                 durable specs
```

---

## 12. Phased roadmap
- **Phase 0 — Skeleton & bring-up (foundations):** ESP-IDF C++ project, partition table (incl. `coredump`), config store, **`nv_log` (ring + serial sink) + crash capture from day one**, HAL bring-up (display + touch + SD via BSP), LVGL + PPA "hello", boot to a blank home screen.
- **Phase 1 — Kernel core:** Memory Broker + Service Manager + Event Bus + Config + Locale + Time. Host-unit-tested.
- **Phase 2 — UI shell (SystemUI):** two-plane compositor, **status bar**, **notification center + quick settings**, full **gesture engine**, volume/brightness HUD, **on-screen keyboard/IME**, **orientation manager (portrait/landscape)**, **theme engine** + Material icon asset pack + wallpaper, Launcher (icon grid + search), i18n wired. Lock screen (basic).
- **Phase 3 — Settings app:** all 12 categories, live-apply via Event Bus, encrypted key storage.
- **Phase 4 — App model:** WAMR + WASM loader + capability permissions + manifest + one demo WASM app; solo-mode + broker enforced.
- **Phase 5 — Anima AI:** LLM router (Claude/Groq/Gemini) + streaming + voice pipeline + Anima console; keys managed in Settings.
- **Phase 6 — Connectivity & web:** full C6 Wi-Fi/BLE + Ethernet, NTP, OTA update, app store, **web console + REST/WS API + WebFS + screen-mirror** (reuse Cardputer web stack).
- **Phase 7 — Polish:** power governor, animations, more core apps (Files, Terminal), security/PIN.

---

## 13. First concrete step
Phase 0 scaffold: create the ESP-IDF C++ project skeleton (`main/`, `components/nv_hal`, `nv_kernel`, `nv_ui`), partition table, `sdkconfig.defaults.esp32p4` (PSRAM OCT 200M, flash 16MB@80M, ESP-HOSTED C6, LVGL, PPA), and a boot that brings up JD9165 + GT911 + SD and shows a blank NucleoOS Anima home. Memory Broker + Service Manager stubs wired from day one so every later feature plugs into them.
