# NucleoOS P4 · an operating system for the Guition ESP32-P4 7" touchscreen

**NucleoOS turns the Guition JC1060P470C (ESP32-P4, 7", 1024×600) into a small computer you can
actually use:** a touch launcher and a classic desktop, an app store with over 400 apps and games,
an ebook reader with a catalog of free books, a spreadsheet and a word processor, game engines and
emulators, an assistant that works offline, a second-screen mode for your PC, a Home Assistant
dashboard, a virtual-pinball DMD, and signed updates over Wi-Fi. Flash it from Chrome in a few
minutes; no toolchain needed.

[![Install from the browser](https://img.shields.io/badge/⚡_Install-from_your_browser-1f5eff?style=for-the-badge)](https://indecenti.github.io/nucleoos-p4-store/flash/)
[![Latest release](https://img.shields.io/github/v/release/indecenti/NucleoOS-P4?style=for-the-badge&label=firmware)](https://github.com/indecenti/NucleoOS-P4/releases/latest)
[![App store](https://img.shields.io/badge/App_store-400%2B_apps_and_games-8a4dff?style=for-the-badge)](https://indecenti.github.io/nucleoos-p4-store/)
[![User guide](https://img.shields.io/badge/📖_User_guide-read_it-2ea44f?style=for-the-badge)](docs/USER_GUIDE.md)

![Platform: ESP32-P4](https://img.shields.io/badge/platform-ESP32--P4-informational)
![Board: Guition JC1060P470C](https://img.shields.io/badge/board-Guition_JC1060P470C-informational)
![Framework: ESP-IDF v5.5.2](https://img.shields.io/badge/ESP--IDF-v5.5.2-red)
![LVGL 9](https://img.shields.io/badge/LVGL-9-green)
[![CI](https://github.com/indecenti/NucleoOS-P4/actions/workflows/ci.yml/badge.svg)](https://github.com/indecenti/NucleoOS-P4/actions/workflows/ci.yml)
![License: PolyForm Noncommercial 1.0.0](https://img.shields.io/badge/license-PolyForm%20Noncommercial%201.0.0-blue)

<p align="center">
  <img src="docs/screenshots/home.jpg" alt="NucleoOS P4 launcher on the Guition JC1060P470C ESP32-P4 7 inch display" width="720">
</p>

| Tablet launcher | Classic desktop | App Store |
|---|---|---|
| ![Launcher](docs/screenshots/home.jpg) | ![Classic desktop with the Start menu](docs/screenshots/classic-start.jpg) | ![App Store](docs/screenshots/store.jpg) |
| **Books**: catalog and reader, upright | **Sheets**: xlsx spreadsheets | **ANIMA**: the assistant |
| ![Books catalog and a page](docs/screenshots/books.jpg) | ![Sheets](docs/screenshots/sheets.jpg) | ![ANIMA](docs/screenshots/anima.jpg) |
| **Doom engine** (Freedoom) | **Vertice GP**, a 3D game on our engine | **Game Boy** emulator |
| ![Doom](docs/screenshots/doom.jpg) | ![Vertice GP](docs/screenshots/vertice-gp.jpg) | ![Game Boy](docs/screenshots/gameboy.jpg) |
| **Terminal**: shell, Python, Lua | **Second Screen** for your PC | **System Monitor** |
| ![Terminal](docs/screenshots/terminal.jpg) | ![Second Screen](docs/screenshots/second-screen.jpg) | ![System Monitor](docs/screenshots/sysmon.jpg) |

*Live captures from the device, 1024×600. What is verified on the board and what is still
experimental is listed feature by feature in [`docs/STATUS.md`](docs/STATUS.md).*

## Highlights

No Linux, no Android: NucleoOS is its own operating system, written for a microcontroller with two
360 MHz RISC-V cores and 32 MB of PSRAM. On that chip it runs:

- **An AI agent that works offline first.** ANIMA answers on the device with no network. When it
  needs a language model, it can use one on your LAN that it finds by itself (Ollama, LM Studio,
  llama.cpp) or a cloud API. With a model it sees pictures and runs the shell, and it can read and
  drive the screen like a person. You can reach it from Telegram, and from Claude Desktop, Claude
  Code or Cursor through MCP. API keys are sealed to the chip and never leave it.
- **A desktop in your browser, served by the board itself**: about 40 windowed apps (files,
  spreadsheet, SSH, mail, AI chat and multi-agent builders, a DJ mixer, dictation), an AI command
  bar on Ctrl+Space, and installation as a PWA.
- **An App Store with over 400 signed apps and games**: whole consoles (WASM-4, Game Boy, Arduboy,
  CHIP-8), Doom, ScummVM, LÖVE, interactive fiction, programming languages, office and smart-home
  apps. WebAssembly compiled ahead of time to native RISC-V code.
- **A real-time 3D engine in the firmware** that renders on both cores, with games built on it.
- **Two shells**: a touch launcher and a classic desktop with a Start menu, windows and right-click.
  It can switch on by itself when you plug in a mouse and a keyboard.
- **An ebook reader with thousands of free books**, offline Wikipedia, a spreadsheet that opens and
  saves xlsx, and a word processor.
- **It connects to your other devices**: a second screen for Windows, Mac, Linux or Android, the
  DMD display of a virtual pinball cabinet, and a Home Assistant dashboard that also publishes the
  board itself into Home Assistant.
- **Signed updates with a safety net**: signed OTA updates, a recovery app, a safe mode and a
  last-known-good copy. CI checks memory budgets on every commit, and fuzzers cover every parser.

## Install in 3 steps

1. **Open the [web flasher](https://indecenti.github.io/nucleoos-p4-store/flash/)** in Chrome or
   Edge, plug the board in with a USB-C data cable, press *Connect & install*.
2. **Insert a FAT32 microSD.** Apps, books, media, offline content and updates live there.
3. **Follow the setup wizard**: language, Wi-Fi, then one tap downloads the offline content
   (ANIMA's data, dictionaries, the web companion, about 140 MB) in the background. Skipped it? Find
   it later in **Settings > System content**. From then on the board updates itself and installs
   apps from the Store.

Prefer the command line? Each [release](https://github.com/indecenti/NucleoOS-P4/releases/latest)
has a single factory image:
`esptool.py --chip esp32p4 write_flash 0x0 nucleoos-p4-<version>-jc1060p470c-factory.bin`

> Needs an ESP32-P4 chip revision v0.x/v1.x, which covers every board sold so far. On a v3 chip
> the bootloader won't start, and the stock firmware can be flashed back.

**New here? Read the [user guide](docs/USER_GUIDE.md)**: every feature, step by step.

## What you get

### Two desktops: tablet and classic
- **Tablet launcher**: adaptive grid with pages, folders, a dock, search and wallpapers;
  landscape and portrait; multitouch (5 points); system gestures only on the screen edges.
- **Classic desktop**: desktop icons, a taskbar, a Start menu with search, pinned apps and games,
  a system tray, windows with title bars, and right-click menus. Turn it on in **Settings >
  Display**, or let it switch on by itself when a mouse and a keyboard are plugged in. It changes
  only the shell; the apps look the same.
- **Keyboard and mouse everywhere**: USB or Bluetooth (BLE). Focus moves with Tab, Esc goes back,
  F1 shows the shortcuts, Alt+Tab and Alt+F4 work. There's a desktop-style selection
  (Ctrl/Shift/rubber band), **Open with** and default apps, a system clipboard for text, images and
  files, and a screen-region capture.

### App Store: over 400 apps and games, installed over Wi-Fi
| Platform | What's in it |
|---|---|
| WASM-4 | 149 fantasy-console games by the WASM-4 community |
| Arduboy | 83 games |
| Game Boy | a Peanut-GB emulator and 39 free homebrew ROMs |
| Doom engine | Chocolate Doom with OPL music; 32 packages, including Freedoom 1 and 2 |
| ScummVM | ScummVM 2.9.1 with 8 freeware adventures (Beneath a Steel Sky, Flight of the Amazon Queen…) |
| CHIP-8 | one app with 83 CHIP-8 / SUPER-CHIP / XO-CHIP games |
| Interactive fiction | 33 titles on Frotz and Glulxe, Zork included |
| Lua and LÖVE | 16 LÖVE games and more on the Lua App engine |
| Terminal | Lua 5.4, JavaScript (QuickJS-ng), MicroPython, BASIC, Berry, Wren, Tcl, Scheme, pForth, SQLite, jq, bc, GNU units, Eigenmath, pdfio, zstd, FIGlet… |
| Apps | Weather, Air quality, Home, Devices, Synth, Pianino, Timer, Pomodoro Desk Hub, Puzzles (40 games by Simon Tatham), Chess, Library, WordGrinder… |

What the Store does:
- **Discover**: featured, most downloaded, new and recently updated, plus categories, a
  **Consoles** view with each platform's game count, and search, in 5 languages.
- **An app page like on a phone**: screenshots, author, version, size, license, what's new, a
  **guide with a QR code** to read on your phone, and the packages it needs, which are installed
  with it.
- **Permissions before you install**: a sheet lists what the app may do (Internet, devices on your
  network, MQTT or Home Assistant, files, camera, microphone). An update that asks for more
  highlights the new permissions, and you can take any of them back in Settings.
- **Signed packages**: the board checks every file of a package against the Store's key and refuses
  anything unsigned or altered.
- **Installed in seconds over Wi-Fi**, with the icon on Home straight away. The board notices
  updates in the background and notifies you. System tools (Lua, JavaScript, Python, SQLite, the
  office engines) install and update themselves.
- **More than apps**: offline knowledge packs for ANIMA, dictionaries for offline translation in
  5 languages, and Doom WADs.
- **Open and free to host**: the catalog is static on GitHub Pages, so you can
  [browse it in a browser](https://indecenti.github.io/nucleoos-p4-store/). The Terminal has it
  too: `store search`, `store install`.


### Reading: Books and Library
- **Books** is a full ebook reader: EPUB 2/3 (fixed layout too), FB2, CBZ comics, TXT, HTML,
  Markdown. Justified pages with hyphenation in 11 languages, widows and orphans kept away, real
  chapters and a table of contents, bookmarks, notes, full-text search, one or two pages, and a
  picture viewer with zoom.
- **Six themes**: white, sepia, dark, black, an **e-ink simulation** with its refresh flash, and
  **1-bit** monochrome with dithered pictures.
- **Upright reading**: hold the board in portrait and the reader turns with it.
- **A catalog of free books inside the app**: thousands of rights-checked public-domain and
  freely licensed books (Project Gutenberg, Standard Ebooks, Wikisource, Wikibooks) in English,
  Italian, German, Spanish, French and more, with covers. Pick a language, search, download in one
  tap (checked with SHA-256), read.
- **Library** reads offline Wikipedia and any Kiwix ZIM file, pictures and formulas included, and
  downloads ZIM files from the Kiwix catalog.

### Office: Sheets and WordGrinder
- **Sheets** is a native spreadsheet. It opens and saves **xlsx**, opens xls, ods, csv and tsv,
  has function names in your language, templates (monthly budget, shopping list, timesheet) and a
  recovery copy. ANIMA can propose edits that you apply with one tap.
- **WordGrinder** is a word processor for the terminal: odt, Markdown and HTML.
- With the classic desktop and a keyboard, the board works as a small writing and accounting
  machine.

### Games, engines and emulators
- **Vertice**, a 3D engine in the firmware: it renders on both cores in tiles, with lights, fog,
  particles, models, a Mode-7 floor, 360° panoramas and picking. Games on it: Vertice GP (karts),
  Vertice Bass (fishing), Tank Rally.
- **Doom engine** (doomgeneric / Chocolate Doom) with OPL music, keyboard, mouse and gamepads.
- **ScummVM**, **Game Boy**, **Arduboy**, **CHIP-8**, **WASM-4** with its upstream rasterizer
  and sound, **interactive fiction**, and **LÖVE** games on Lua.
- An on-screen gamepad for touch, plus USB and Bluetooth gamepads (experimental).

### ANIMA, the assistant that works offline first
ANIMA answers on the device whenever it can, calls a language model only when it has to, and says
"I don't know" instead of guessing.

**With no network at all**: calculations with follow-ups ("6 times 6", then "plus 5"), time and
date, unit conversions, timers and alarms, notes and reminders, opening apps, volume and brightness, system status, compound requests ("close the music and
open the notes") run as one plan, knowledge cards from offline Wikipedia and Wikidata packs, and
definitions, synonyms and translations from offline dictionaries, in five languages. Answers can be
spoken by the offline voice.

**Connect it to any model**:
- **On your LAN, found by itself**: Ollama, LM Studio or llama.cpp on your PC. In *local* mode nothing
  leaves your home.
- **In the cloud**: OpenAI-compatible APIs (Groq, OpenAI, OpenRouter…), Anthropic and Gemini.
- **A fallback ladder**: if a model fails it waits and tries the next one. If none answers, the
  device's own answer is still there. Your API keys are sealed to the chip and never leave it.
- **Four modes**: *offline*, *local*, *hybrid* (the default: the device first, then the web and a
  model), and *agent*, where the model leads but what the device can do exactly stays on the device.

**An agent on a microcontroller**:
- **Tools**: native tool calling over the shell, files, apps, timers, the calendar, Home Assistant
  and the LAN devices. A permissions file says what it may do without asking.
- **It reads and drives the screen** the way computer-use agents drive a browser: it gets the screen
  as a list of controls, then taps, types and swipes, and checks the result after each step.
- **It sees**: photos and screenshots go to a model that can see. A model that can't gets a written
  description from a separate vision model.
- **It writes code and runs it**: Lua and Python on the board, in a write → check → run → fix loop.
- **Memory, skills and routines**: it remembers up to 48 facts about you and recalls them by
  relevance. It reads skills in the Agent Skills format, runs event rules ("every morning at 8…",
  "when this app opens…"), and does a periodic check that stays silent unless something needs you.
- **Long conversations**: conversations are kept on the SD card, grouped by workspace, and
  compacted when they get long.

**Reach it from anywhere**: the ANIMA app; a **Telegram bot**, which also takes photos; the web
desktop's chat and command bar; and an **MCP server** ([`tools/anima_mcp.py`](tools/anima_mcp.py)),
so Claude Desktop, Claude Code, Cursor or OpenCode can ask ANIMA and drive the board. Optional
offline **wake word** (ESP-SR WakeNet, off by default in the build).

### Second Screen: the board as a display for your computer
Pick your computer and the board guides you step by step:
- **Windows** over USB (an indirect-display driver);
- **Mac** with Screen Sharing (VNC, with Apple Remote Desktop authentication);
- **Linux**, **Android** (droidVNC-NG) or **any device with a browser** (NucleoCast).

Full details in [`docs/SECOND_SCREEN.md`](docs/SECOND_SCREEN.md). USB mode is experimental.

### Smart home: Home and Home Assistant
- **Home** (store app) is a touch dashboard for **Home Assistant**: tiles by room for lights with
  brightness, thermostats, covers, sensors, energy, locks and scenes. Set it up in **Settings >
  Home** with your Home Assistant URL and a long-lived access token, or paste them from a PC.
- The board announces its own sensors and controls to Home Assistant over **MQTT discovery**.
- Related store apps: Devices, Zigbee, openHASP, MQTT Explorer. Guide:
  [`docs/HOME_ASSISTANT.md`](docs/HOME_ASSISTANT.md).

### DMD Pinball: the display of a virtual pinball cabinet
The board becomes the cabinet's dot-matrix display for dmdext: over Wi-Fi (`networkstream`) or
USB, where it emulates a ZeDMD 128×32. Both links can be live at once. It offers nine layouts
(backglass + DMD among them), a clock when the PC is silent, dot colour, shape and glow, a test
screen, and a PinUP Popper touch menu to pick tables through a small PC bridge. Verified on a real
cabinet. Guide: [`docs/DMD.md`](docs/DMD.md).

<p align="center">
  <img src="docs/screenshots/dmd_fullscreen.jpg" alt="NucleoOS DMD Pinball: a 128x32 DMD on the 7 inch display" width="480">
</p>

### Media
- **Camera** (OV02C10 module): auto exposure and white balance, photos, MJPEG video.
- **Music**: MP3, WAV, AAC, FLAC, M4A, with USB-audio output.
- **Video**: MJPEG `.avi` and MPEG-1 `.mpg`, tear-free. **Gallery** has hardware-JPEG thumbnails.
  **Recorder** records WAV (experimental).

### A desktop in your browser
The board serves a whole desktop to any browser on your network. Open its address and type the
6-digit code it shows. You get a windowed web OS with a taskbar, search, notifications and about
40 apps, all working on the board's own files and hardware:
- **Work**: File Commander on the SD card, Spreadsheet, Notepad, Tasks, Calendar, Contacts, Mail,
  an SSH client, Code Runner, an archive manager, an authenticator and a QR code maker.
- **AI**: a chat; **Agents**, an online multi-agent workspace that builds, reviews and publishes
  apps, asking before anything destructive; dictation with Whisper; voice commands; and a
  **command bar on Ctrl+Space** that talks to ANIMA and opens apps and files for you.
- **Media**: a media player, a photo viewer, a video player, a voice recorder, internet radio and
  **DJ Mix**, which mixes your music with an energy curve (warm-up, peak, cool-down).
- **The board**: a terminal, a system monitor, a log viewer, a Wi-Fi scanner, an IR remote,
  updates, and settings, including ANIMA's AI, Telegram, voice and workspace pages.

It installs as an app (PWA). The apps run on the board's paired REST API and need no cloud
account; only the AI apps call the model you set up. It is plain HTTP on your LAN, protected by
the pairing.

### The system underneath
- **Wi-Fi 6 through the ESP32-C6**, Bluetooth LE for keyboards, mice and gamepads; USB host for
  keyboards, mice, audio, gamepads and drives (drives and pads experimental).
- **Signed OTA updates** from GitHub with a primary and an offline backup key, a beta channel, a
  recovery app, a safe mode and a last-known-good copy in flash. Updates are staged on the SD card.
- **5 languages** (English, Italian, Spanish, French, German) and **offline text-to-speech** in
  Italian and English.
- **Hardware acceleration**: the JPEG codec, the PPA for scaling and rotation, and a
  double-buffered display with vsync. Apps get them too: hardware JPEG decoding and a canvas turned
  by the PPA (ABI 19).
- **RAM discipline**: one app runs at a time, and a PSRAM broker reclaims caches before heavy work.
- **A system downloader** with resume, SHA-256 checks and progress, shared by every app.

### For developers
- **Write apps in C, compiled to WebAssembly**, with the SDK in [`sdk/`](sdk/): graphics with
  dirty rectangles, sprites and panels, multitouch, keyboard and mouse, gamepads, audio and a mixer,
  voice, UDP, async HTTP, WebSocket, MQTT, mDNS, files (WASI), file associations, full-screen
  terminal programs, the downloader, canvas rotation and hardware JPEG. Host ABI **19**: see the
  table in [`docs/WASM_APPS.md`](docs/WASM_APPS.md).
- Or write **Lua apps** with `gfx`/`ui`/`nv` modules and a LÖVE layer
  ([`docs/LUA_APPS.md`](docs/LUA_APPS.md)), or **native C++** apps.
- **AOT compilation** (WAMR `wamrc`) for native speed on the P4's RISC-V cores, with an
  interpreter fallback.
- **Hot reload over Wi-Fi**, remote UI automation and screenshots over HTTP (`/api/ui/*`,
  `/api/screen`), and a remote shell. The API is paired: a 6-digit code on the device, then a
  session token (`python tools/pair.py`).
- **Start here:** [`docs/APP_SDK.md`](docs/APP_SDK.md) helps you choose a runtime (native C++,
  WASM, Lua, web). Then: architecture [`ARCHITECTURE.md`](ARCHITECTURE.md) · app guide
  [`docs/WASM_APPS.md`](docs/WASM_APPS.md) · game guide [`docs/GAME_DEV.md`](docs/GAME_DEV.md) ·
  3D engine [`docs/VERTICE.md`](docs/VERTICE.md) · shell [`docs/SHELL.md`](docs/SHELL.md).

## Documentation
| For | Read |
|---|---|
| Using the board | [User guide](docs/USER_GUIDE.md) · [Shell reference](docs/SHELL.md) · [Books](apps/libri/GUIDE.en.md) |
| ANIMA | [Modes](docs/ANIMA_MODES.md) · [Knowledge packs](docs/ANIMA_KB.md) · [Workspace, tools and Telegram](docs/ANIMA_WORKSPACE.md) · [Hands-free](docs/HANDSFREE.md) |
| Connections | [Second Screen](docs/SECOND_SCREEN.md) · [Home Assistant](docs/HOME_ASSISTANT.md) · [DMD Pinball](docs/DMD.md) · [Open with](docs/FILE_ASSOCIATIONS.md) |
| Updates and safety | [OTA](docs/OTA.md) · [Security](SECURITY.md) · [Feature status](docs/STATUS.md) |
| Building apps | [App SDK](docs/APP_SDK.md) · [WASM apps](docs/WASM_APPS.md) · [Lua apps](docs/LUA_APPS.md) · [Games](docs/GAME_DEV.md) · [Vertice 3D](docs/VERTICE.md) |
| Working on the OS | [Architecture](ARCHITECTURE.md) · [Engineering rules](docs/ENGINEERING_RULES.md) · [Contributing](CONTRIBUTING.md) |

## Engineering and testing
- **CI on every commit** ([`ci.yml`](.github/workflows/ci.yml)): a clean ESP-IDF build from pinned
  dependencies, a check that the lock file did not drift, **memory budgets** (internal static RAM
  and IRAM) that fail the build when exceeded, manifest checks, and a privacy check that keeps
  personal data and secrets out of the public repo.
- **Host tests and fuzzers** ([`tests/host`](tests/host)): 35 unit-test files and 16 libFuzzer
  targets for every parser that reads untrusted input (AVI, MP4, MPEG-1, JPEG headers, web paths and
  queries, OTA manifests, store packages, pairing, app network policy, Home Assistant replies…),
  under AddressSanitizer and UBSan, as 64-bit and 32-bit builds since the P4 is 32-bit. A weekly job
  fuzzes longer. The fuzzers have found real bugs (path-guard bypasses, decoder hangs and
  out-of-bounds reads), each fixed with a regression test.
- **Hardware in the loop** ([`tools/hil`](tools/hil)): opens and closes every app watching for
  reboots, freezes and leaks; checks the OTA safety net, downloads with resume, canvas rotation and
  hardware JPEG on the real board.
- **Crash forensics**: coredumps and an interrupt-storm watchdog are kept across reboots and served
  at `/api/crash`.
- **Engineering rules** ([`docs/ENGINEERING_RULES.md`](docs/ENGINEERING_RULES.md)): each rule comes
  from a real crash or freeze on this board, with the reason.
- **Security** ([`SECURITY.md`](SECURITY.md)): signed firmware and apps, a paired web API with
  Host/Origin checks, a lock-screen PIN, a security event log; what is protected and what is not.

## Known limitations
- **H.264 / MP4 video is not supported.** Use MJPEG `.avi` or MPEG-1 `.mpg`.
- **No Classic Bluetooth**: the ESP32-C6 co-processor runs BLE only.
- **One app runs at a time**, by design: the RAM goes to the app in front.
- **No Secure Boot or flash encryption** by default; settings encryption is implemented but off in
  shared builds.
- The **web companion is plain HTTP** on your LAN, protected by pairing.
- The CPU runs at 360 MHz: this board does not boot reliably at 400 MHz.
- Experimental features are marked in [`docs/STATUS.md`](docs/STATUS.md).

## Supported hardware
| Board | Status |
|---|---|
| **Guition JC1060P470C_I_W**: 7" ESP32-P4, 1024×600, JD9165 panel | ✅ primary target, used daily |
| Guition JC1060P470 with the newer panel revision (non-JD9165) | ❓ untested, reports welcome |
| Guition JC8012P4A1 (10.1" ESP32-P4) | ❌ not supported yet |

Also sold as "Guition ESP32-P4 7 inch display", "JC1060P470C", "JC1060P470C-I-W" (AliExpress /
Guition store): the same board with the JC-ESP32P4-M3 module.

- SoC: **ESP32-P4** (+ ESP32-C6 co-processor for Wi-Fi 6 / BLE via `esp_hosted`), 32 MB PSRAM, 16 MB flash
- Display: JD9165 7" **1024×600** MIPI-DSI, GT911 capacitive touch
- Audio: ES8311 codec (I²S) + on-board mic; hot-plug USB-audio (UAC) output
- Camera: MIPI-CSI connector (tested with an OV02C10 module); storage: microSD (FATFS)

## Build from source
Requires **ESP-IDF v5.5.2** (Linux, macOS or Windows).

```sh
. $IDF_PATH/export.sh              # Windows: run export.ps1 from your ESP-IDF folder
sh tools/ci/fetch_wamr.sh          # WebAssembly runtime at the pinned commit, plus the project patch
idf.py set-target esp32p4
idf.py build
idf.py -p <port> flash monitor     # e.g. /dev/ttyUSB0 or COM5
```

The same steps run in CI on a clean container, so a fresh checkout builds.

`sdkconfig` is generated from `sdkconfig.defaults*` (kept in-tree); component-manager
dependencies are pinned by `dependencies.lock`.

## Layout
| Path | What |
|------|------|
| `main/` | boot entry (`app_main.cpp`) |
| `components/nv_*` | OS subsystems: hal, ui, kernel, apps, wasm, media, tts, anima, web, dmd, … |
| `components/vertice` | the Vertice 3D engine |
| `apps/` | app packages: sources, manifests, store screenshots |
| `ports/` | third-party ports (Doom, ScummVM, Lua, the Books reader, the ZIM reader, CLI tools…) |
| `sdk/` | WASM app C SDK (`nucleo_sdk.h`) |
| `sd/web` | web OS companion (PWA served over Wi-Fi): the P4 build of the shared web OS, **pinned from nucleo-webos** (`webos.lock`, `tools/webos-sync.py`); fixes go in nucleo-webos, never here |
| `server/appstore` | the store's catalog sources |
| `tools/` | builds, store and OTA publishing, HIL tests, asset generators |
| `tests/host` | host unit tests and fuzzers |

> `system/icons/mdi` and `system/icons/flat-color` are third-party icon repos (own git history),
> not tracked here; re-clone them only if you need to regenerate `nv_icons.c`.

## Contributing
PRs welcome, see [`CONTRIBUTING.md`](CONTRIBUTING.md) (it includes a short contributor agreement
so the project can stay dual-licensed). Third-party components and their licenses are listed in
[`THIRD_PARTY.md`](THIRD_PARTY.md).

## License
**[PolyForm Noncommercial License 1.0.0](LICENSE.md)**: free for any **noncommercial** use
(personal, study, research, hobby, non-profit, education, government).

**Commercial or production use requires a paid commercial license**, see
[`COMMERCIAL.md`](COMMERCIAL.md). Want to ship NucleoOS P4 in a product? Get in touch:
**niki070585@gmail.com**.

© 2026 indecenti. All rights reserved.
