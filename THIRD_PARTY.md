# Third-party components

NucleoOS P4's own source code is licensed under [PolyForm Noncommercial 1.0.0](LICENSE.md).
It builds on third-party components that keep **their own licenses** — the project's
noncommercial terms do **not** restrict or override them. Each upstream is authoritative for
its exact terms; the list below is a convenience summary of the major dependencies.

| Component | Used for | License (see upstream for authoritative terms) |
|---|---|---|
| ESP-IDF | SoC framework / drivers | Apache-2.0 |
| esp_hosted | Wi-Fi/BLE over the ESP32-C6 co-processor | Apache-2.0 |
| esp_codec_dev / esp_audio_codec | ES8311 audio codec | Apache-2.0 |
| LVGL | UI toolkit | MIT |
| DejaVu Sans Mono | Terminal monospace font (`components/nv_fonts/nv_font_mono_17.c`, generated with lv_font_conv) | Bitstream Vera Fonts license + public domain changes (free, redistributable) |
| libvterm 0.3.3 (Paul Evans, via neovim/libvterm) | Terminal VT220/xterm emulation (`components/libvterm`; DEC/UK legacy charsets left out, see `src/encoding.c`) | MIT |
| WAMR (wasm-micro-runtime) | WASM app runtime | Apache-2.0 (with LLVM exceptions) |
| WASM-4 (runtime rasterizer, APU, font, `wasm4.h`) | WASM-4 cart compatibility (`components/nv_wasm/w4`, `sdk/w4`) | ISC |
| wasi-libc / wasi-sdk sysroot (build-time only, not in the firmware) | WASI and WASM-4 app builds | Apache-2.0 WITH LLVM-exception / MIT |
| Jet (cubecoders/jet, commit c56dfc0) | core rasterizer of the Vertice 3D engine (`components/vertice/core`, notice in `core/LICENSE-Jet`) | MIT |
| pl_mpeg | MPEG-1 video/audio decode | MIT |
| minimp3 | MP3 decode | CC0 / public domain |
| TJPGD (bundled in LVGL) | software JPEG decode | BSD-style |
| ScummVM 2.9.1 + the NucleoOS backend (`ports/scummvm`, app `apps/scummvm`; icon from the ScummVM tree) | adventure game engine app (WASI) | GPL-3.0-or-later (the whole `ports/scummvm` directory, see its `COPYING`) |
| zlib 1.3.1 / libogg 1.3.5 / Tremor / libmad 0.15.1b (linked into the ScummVM app only) | zip inflate, Ogg Vorbis, MP3 | Zlib / BSD-3-Clause / BSD-3-Clause / GPL-2.0-or-later |
| doomgeneric (ozkl, commit dcb7a8d) + Chocolate Doom (OPL music player, MIDI parser, OPL callback queue; commit 895f581) | Doom engine app (`ports/doom`, app `apps/doom`; sources fetched pinned by `ports/doom/fetch.sh`) | GPL-2.0-or-later |
| emu8950 (Mitsutaka Okazaki; OPL2 waveforms and block renderer by Graham Sanderson, rp2040-doom) | OPL2 FM music in the Doom app | MIT |
| Freedoom 0.13.0 / DOOM shareware 1.9 / community WADs (Scythe, Memento Mori, Zone 300, DTWID, SIGIL DOS, Plutonia 2, Doom 2 Reloaded, 1000 Lines 2) | game data of the Doom store games, re-hosted unchanged with their text files on the store site (`data/doom`), never in the firmware | BSD-3-Clause / id shareware license / each WAD's own terms (freely distributable; 1000 Lines 2: CC BY 4.0) |
| Lua 5.4.9 (Lua.org, PUC-Rio) + json.lua 0.1.2 (rxi) | Lua App engine (`ports/luaapp`, app `apps/luaapp`): Lua runtime and its built-in `json` module | MIT / MIT |
| Montserrat Medium (The Montserrat Project Authors) | anti-aliased text of the Lua App engine (`ports/luaapp/font`, baked into the module by `gen_font.py`) | SIL OFL 1.1 |
| RetroLove (Jon Thysell) / love-tetronimo (Przemekkkth) / sudoku.lua (Azdren Ymeri) | Lua store apps `retrolove`, `tetronimo`, `sudoku` (sources in `apps/<id>/src`, license files alongside) | MIT / MIT / MIT |
| libqrencode 4.1.1 (Kentaro Fukuchi) | system terminal program `qrencode` (`ports/cli`, sources fetched pinned by `ports/cli/fetch.sh`, patch `ports/cli/qrencode/qrencode.patch`; rebuild with `ports/cli/build.sh qrencode`) | LGPL-2.1-or-later |
| GNU units 2.24 (Adrian Mariano, FSF) + its unit database | system terminal program `units` (`ports/cli`, database compiled in) | GPL-3.0-or-later |
| Material Design Icons | UI glyphs (source for generated icons) | Apache-2.0 |
| Flat Color Icons (icons8) | app/launcher icons (source for generated icons) | MIT |

Notes:
- The icon **source** repos (`system/icons/mdi`, `system/icons/flat-color`) are not tracked in this
  repository; only the generated `components/nv_ui/generated/nv_icons.c` is committed.
- The ScummVM game packages (`apps/svm-*`) carry no game data: on first start the device downloads
  the original freeware archives from downloads.scummvm.org, unmodified, under each game's own
  licence (kept inside the archive).
- If you add a new third-party dependency, list it here with its license and keep its notices.
- This file is informational, not legal advice. When in doubt, consult each component's LICENSE.
