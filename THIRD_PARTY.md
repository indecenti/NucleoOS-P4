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
| Home Assistant intents (home-assistant/intents, Home Assistant contributors) | test sentences (Italian, English, Spanish, French, German) imported as ANIMA L0 paraphrases and tests (`tools/anima_phrases_ha.txt` via `tools/import_ha_intents.py`, compiled into `components/nv_anima/anima_phrases.c`) | CC-BY-4.0 |
| Wiktionary / Wikizionario (Wikimedia contributors, extracted by kaikki.org / wiktextract; Italian, Spanish, French and German entries) and FreeDict+WikDict ita-eng / eng-ita 2025.11.23 | ANIMA offline dictionaries on the SD, never in the firmware: definitions, synonyms, antonyms, inflected forms and IT<->EN translations (`dict-*.tsv`, `lex-it.tsv`, `forms-it.tsv`, built by `tools/dicts/gen_dicts.py`; sources and counts in the generated `DICTIONARIES.txt`) | CC BY-SA 4.0 (WikDict: CC BY-SA 3.0) |
| Open English WordNet 2025 (Global WordNet Association) | ANIMA English definitions, synonyms and antonyms on the SD (`lex-en.tsv`, `forms-en.tsv`, same generator) | CC BY 4.0 |
| Wikipedia in Italian, English, Spanish, French, German (Wikipedia contributors; ZIM files by Kiwix/openZIM) and Wikidata identifiers, sitelinks and facts (Wikidata contributors) | ANIMA knowledge packs on the SD, never in the firmware (`*.akb6`, built by `tools/kb/akb6.py`, `tools/kb/wikidata.py` and `tools/kb/facts.py`; the attribution travels in each pack and in every answer's trace) | Wikipedia text CC BY-SA 4.0; Wikidata CC0 1.0 |
| miniz 3.0.2 (Rich Geldreich, RAD Game Tools, Valve) | host tests only: raw-deflate for the knowledge packs (`tests/host/anima/third_party/miniz.c`); the device uses the ESP32 ROM tinfl | MIT |
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
| Eigenmath (George Weigt, commit bf89927 of 2026-09-17) | system terminal program `eigenmath` (`ports/cli`, patch `ports/cli/eigenmath/eigenmath.patch`) | BSD-2-Clause |
| dateutils 0.4.12 (Sebastian Freundt) | system terminal program `dateutils` (`dateadd`, `datediff`, ...; `ports/cli/dateutils`) | BSD-3-Clause |
| html2text 2.2.3 (Martin Bayer, Arno Unkrig and contributors) | system terminal program `html2text` (`ports/cli/html2text`, patch `html2text.patch`) | GPL-2.0-or-later |
| lowdown 3.0.1 (Kristaps Dzonsons) | system terminal program `lowdown` (`ports/cli/lowdown`) | ISC |
| zstd 1.5.7 (Meta Platforms) + zlib 1.3.1 (Jean-loup Gailly, Mark Adler) + liblzma from xz 5.8.4 (Tukaani project) | system terminal program `zstd` (also gzip, gunzip, zcat, xz, unxz, xzcat; `ports/cli/zstd`, patch `zstd.patch`) | BSD-3-Clause / Zlib / 0BSD |
| PDFio 1.6.5 (Michael R Sweet) + zlib 1.3.1 | system terminal program `pdfio` (pdftotext, pdfinfo, pdfmerge from PDFio's examples; `ports/cli/pdfio`) | Apache-2.0 / Zlib |
| MicroPython 1.26.1 (Damien P. George and contributors) + micropython-lib modules | system terminal program `python` (`ports/micropython`, patch `micropython.patch`; rebuild with `ports/micropython/build.sh`) | MIT |
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
