# Handoff for the next AI agent — branch `claude/zealous-sagan-wyjjor`

What this branch adds to NucleoOS (ESP32-P4, ESP-IDF 5.5.2, LVGL 9.5) and how to work on it.

## Build & test
- Firmware: `idf.py build` (CI: `.github/workflows/ci.yml`, runs on `main`/PRs; run it manually with
  `workflow_dispatch` on a branch). Uses `-Werror=all`: watch `snprintf` truncation and LVGL 9 APIs
  (e.g. `LV_FLEX_FLOW_ROW_WRAP`, not `lv_obj_set_flex_wrap`).
- Host tests (no hardware): `make -C tests/host CC=gcc CXX=g++ ARCHES=x64 build/x64/unit_anima build/x64/unit_wake`
  then run the binaries. `tests/host/anima/anima_stubs.c` has a **fake network**
  (`anima_fakenet.h`: fixtures by URL substring), so online features are testable offline.
- Web: static files in `sd/web`; after editing a file regenerate its twin with `gzip -9 -n -k -f <file>`
  and bump `CACHE` in `sd/web/sw.js`.
- Remote text shell / tests on a real board: `tools/anima.py` (`--test tools/anima/smoke.txt`).
- MCP server for agents: `tools/anima_mcp.py` (`--selftest`).

## Features (where to look)
| Feature | Code | Docs |
|---|---|---|
| LLM tool-calling (`ACT <tool> <args>` line, whitelist-validated) | `nv_anima/nucleo_anima.c` (`nucleo_anima_act_*`) | — |
| Permissions allow/ask/deny + yes/no confirm | same + `nucleo_anima_skills.c` (`permissions.json`) | `docs/ANIMA_WORKSPACE.md` |
| Skills `/data/anima/skills/*.md` | `nucleo_anima_skills.c` | `sd/data/anima/skills/README.md.txt` |
| Workspace SOUL/USER/MEMORY/HEARTBEAT.md | `nucleo_anima_skills.c`, heartbeat in `nucleo_anima_online.c` + `nv_apps/anima_system.cpp` | `docs/ANIMA_WORKSPACE.md` |
| Keyless live tools: news, crypto, holidays, sun, weather, FX | `nucleo_anima_online.c` (`nucleo_anima_online_live`) | — |
| Speech-to-text: home Whisper server first (`stt_url`), cloud fallback | `nucleo_anima_online.c` (`nucleo_anima_transcribe`, `_stt_route`) | — |
| Hands-free wake word (ESP-SR, opt-in `CONFIG_NV_WAKE_ESP_SR`) | `components/nv_wake`, mic tap in `nv_hal/nv_audio.cpp`, flow in `nv_apps/anima_app.cpp` | `docs/HANDSFREE.md` |
| Telegram channel (pairing code, owner-only) | `nv_anima/nucleo_anima_telegram.c`, `nv_apps/anima_channels.cpp` | `docs/ANIMA_WORKSPACE.md` |
| Web APIs | `nv_web/nv_web.cpp`: `/api/anima/{net,models,wake,hb,telegram}`, `/api/llm` | — |
| Settings UI | native `nv_apps/settings_app.cpp` (`cat_anima`), web `sd/web/ai-keys.js` | — |

## Rules of the house
- Engine calls run under the spine gate (`nucleo_anima_try_lock/unlock`) on PSRAM-stack tasks (24 KB);
  never do TLS on the httpd task (8 KB): queue work (see `nucleo_anima_tg_request_token`).
- LVGL calls only on the LVGL thread or under `lvgl_port_lock`; `lv_async_call` needs the lock too.
- New user-visible strings: `nv_i18n` (5 languages), not literals, in native UI.
- Secrets on SD go through `nv_sealed_*` (list in `nv_kernel/nv_sealed.cpp`).
- Free GitHub plan: avoid needless CI runs; prefer host tests.

## Not verified on hardware yet
Wake word with real ESP-SR models, heartbeat/Telegram on the device, WebGPU on a real GPU. Partition
table gained a `model` partition (needs a full reflash).
