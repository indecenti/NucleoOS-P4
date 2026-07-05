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
| WAMR (wasm-micro-runtime) | WASM app runtime | Apache-2.0 (with LLVM exceptions) |
| pl_mpeg | MPEG-1 video/audio decode | MIT |
| minimp3 | MP3 decode | CC0 / public domain |
| TJPGD (bundled in LVGL) | software JPEG decode | BSD-style |
| Material Design Icons | UI glyphs (source for generated icons) | Apache-2.0 |
| Flat Color Icons (icons8) | app/launcher icons (source for generated icons) | MIT |

Notes:
- The icon **source** repos (`system/icons/mdi`, `system/icons/flat-color`) are not tracked in this
  repository; only the generated `components/nv_ui/generated/nv_icons.c` is committed.
- If you add a new third-party dependency, list it here with its license and keep its notices.
- This file is informational, not legal advice. When in doubt, consult each component's LICENSE.
