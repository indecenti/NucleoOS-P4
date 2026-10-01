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
| Skills `/data/anima/skills/*.md` and Agent Skills `<name>/SKILL.md` (catalog, progressive disclosure) | `nucleo_anima_skills.c` | `sd/data/anima/skills/README.md.txt` |
| Workspace SOUL/USER/MEMORY/HEARTBEAT.md | `nucleo_anima_skills.c`, heartbeat in `nucleo_anima_online.c` + `nv_apps/anima_system.cpp` | `docs/ANIMA_WORKSPACE.md` |
| Keyless live tools: news, crypto, holidays, sun, weather, FX | `nucleo_anima_online.c` (`nucleo_anima_online_live`) | — |
| Speech-to-text: home Whisper server first (`stt_url`), cloud fallback | `nucleo_anima_online.c` (`nucleo_anima_transcribe`, `_stt_route`) | — |
| Hands-free wake word (ESP-SR, opt-in `CONFIG_NV_WAKE_ESP_SR`) | `components/nv_wake`, mic tap in `nv_hal/nv_audio.cpp`, flow in `nv_apps/anima_app.cpp` | `docs/HANDSFREE.md` |
| Telegram channel (pairing code, owner-only) | `nv_anima/nucleo_anima_telegram.c`, `nv_apps/anima_channels.cpp` | `docs/ANIMA_WORKSPACE.md` |
| Agent loop over the device shell (`ACT sh <cmd>`, ≤5 steps, output fed back) | `nucleo_anima_online.c` (grok_chat loop), `nucleo_anima.c` (`nucleo_anima_sh_class`, `_set_shell`), `nv_apps/anima_channels.cpp` (`anima_sh_exec`) | `docs/ANIMA_WORKSPACE.md` |
| Headless shell run + `store search/list/info/install` | `nv_apps/term_sh.cpp` (`sh_exec_capture`, `b_store`), launcher tile `nv_apps_store_installed` | — |
| MicroPython (`python` Terminal program) | `ports/micropython` (`build.sh`, `build.sh test`) | `ports/micropython/README.md` |
| Multimodal: model caps (Ollama /api/show), `ACT see`, vision helper (`vision_model`), `screenshot`, `/caps`, computer use (`ui`, `input tap/text/keyevent/swipe`, `home`), photos from Telegram (`nucleo_anima_attach_image`, `nucleo_anima_tg_fetch`) | `nucleo_anima_online.c` (`anima_model_caps`, `img_load`, `add_user_content`, grok_chat loop), `nv_apps/term_sh.cpp` (`b_screenshot`, `b_ui`, `b_input`), `nucleo_anima_telegram.c`, `nv_apps/anima_channels.cpp` | `docs/ANIMA_WORKSPACE.md` |
| Native tool calling (OpenAI/Ollama `tools`, translated to ACT) | `nucleo_anima_online.c` (`kToolsJson`, `tool_call_to_act`) | `docs/ANIMA_WORKSPACE.md` |
| Timers and alarms, offline (spoken durations/times IT/EN) | `nv_anima/nucleo_anima_time.c`, ringing in `nv_apps/anima_system.cpp` (`timers_tick`) | `docs/ANIMA_WORKSPACE.md` |
| Automations: event rules (schedule/message/startup/app_open -> run_agent/run_sh/send_message), ESP-Claw format | `nv_anima/nucleo_anima_rules.c`, events + task in `nv_apps/anima_system.cpp`, Telegram in `anima_channels.cpp` | `docs/ANIMA_WORKSPACE.md` |
| Smart home from the shell: `ha` (Assist, template-filtered lists, services) and `dev` (Shelly/Tasmota/WLED, mDNS) | `nv_apps/term_sh.cpp` (`b_ha`, `b_dev`), skill `casa.md` | `docs/ANIMA_WORKSPACE.md`, `docs/HOME_AUTOMATION_PLAN.md` |
| App Casa (Home Assistant dashboard, store `smarthome/ha`) | `apps/casa/main.c` | `apps/casa/GUIDE.md` |
| Store index for ANIMA (`anima-index-<lang>.json`) | `server/appstore/export_static.py` (`anima_index`) | — |
| Web APIs | `nv_web/nv_web.cpp`: `/api/anima/{net,models,wake,hb,telegram}`, `/api/llm` | — |
| Settings UI | native `nv_apps/settings_app.cpp` (`cat_anima`), web `sd/web/ai-keys.js` | — |

## Rules of the house
- Engine calls run under the spine gate (`nucleo_anima_try_lock/unlock`) on PSRAM-stack tasks (24 KB);
  never do TLS on the httpd task (8 KB): queue work (see `nucleo_anima_tg_request_token`).
- LVGL calls only on the LVGL thread or under `lvgl_port_lock`; `lv_async_call` needs the lock too.
- New user-visible strings: `nv_i18n` (5 languages), not literals, in native UI.
- Secrets on SD go through `nv_sealed_*` (list in `nv_kernel/nv_sealed.cpp`).
- Free GitHub plan: avoid needless CI runs; prefer host tests. CI on a branch: `workflow_dispatch` of
  `ci.yml` with `fuzz_seconds: 10`.
- **Memory budget** (`tools/ci/check_budgets.py`): internal RAM static ≤ 210,000 B and main has only a
  few hundred bytes of headroom. Every new static buffer goes to PSRAM (`NV_PSRAM_BSS` from
  `nv_mem_attr.h`, `EXT_RAM_BSS_ATTR` in nv_anima), including function-local `static` arrays.
- `-Werror=all` includes format-truncation: size snprintf targets for the worst case (dates: 40 B).
- Shell commands for ANIMA: read-only ones never ask; anything that writes follows permissions.json
  `sh`; full-screen built-ins (edit/less/top/watch) are refused (`nucleo_anima_sh_class`).

## Status (end of this session)
- Firmware build + memory budgets: **green** in CI at `3929310` (run 179).
- The agent loop over the shell, autonomous mode (`permissions.json` `"mode":"auto"`, `/auto on|off`),
  the shell row in the web permission table and docs were finished and host-tested (unit_anima 127
  checks) but may still be **uncommitted** in the working tree: check `git status` first.
- Host tests: `unit_anima` 239, `unit_wake` 21, `tools/anima_mcp.py --selftest` 12.

## Next steps (agreed order)
1. Commit/push the pending work, one CI run.
2. DONE: WASI terminal programs run headless (`prog_run_headless` in `terminal_app.cpp`).
3. DONE: file tools `ACT write` / `ACT edit` (`nucleo_anima_file_tool`, permission `write`).
4. DONE (first version): skill `sd/data/anima/skills/crea-app.md` (Lua App engine, ~/lua scripts).
   Still to verify on the board: the `lua -e loadfile` syntax check and that ~/lua scripts show in
   the Lua App tile.
5. PARTIAL: `launch APP_ID` builtin (term_sh.cpp, opens an app via `nv_ui_open_app_id_async`); errors via `dmesg`.
6. DONE: plan mode (`/plan on|off`, permissions.json `"mode":"plan"`: read-only, writes denied, the
   grammar asks for a `- [ ]` plan) vs build; build keeps a `- [ ]`/`- [x]` todo checklist in replies.
7. DONE: MicroPython 1.26.1 as the store package `python` (`apps/python`, `ports/micropython`,
   README there): no-setjmp core patch over the host's protected call, GC made exact with Binaryen
   `--flatten --spill-pointers`, upstream suite 751/756 under WAMR with GC stress. ANIMA skill
   `sd/data/anima/skills/python.md` (write ~/py/x.py, `ACT sh python /py/x.py`, fix from the traceback).
   Still to do on the PC/board: riscv32 AOT, store publish, device run (docs/LOCAL_AGENT_TODO.md).

## Hardware tasks
See `docs/LOCAL_AGENT_TODO.md` (for an agent running on the PC with the board).

## Not verified on hardware yet
Wake word with real ESP-SR models, heartbeat/Telegram on the device, WebGPU on a real GPU. The ESP-SR
wake word (opt-in) needs a `model` partition carved from the reserved `assets` area (docs/HANDSFREE.md).

## Updating a device
1. **Firmware**: this branch follows main's flash layout v2 (recovery + 10 MB system, updates via SD,
   docs/OTA.md). `idf.py build flash` over USB, or the SD update path. Settings in NVS survive.
2. **SD card**: copy the repo's `sd/` mirror onto the card root, additively (nothing is deleted):
   `.\tools\sync-sd.ps1 -Drive E:` on Windows, or copy `sd/web/` -> `/web/` and `sd/data/anima/skills/`
   -> `/data/anima/skills/` by hand. Changed on this branch: `web/ai.js`, `web/ai-keys.js`,
   `web/webllm.js` (new), `web/copilot.js`, `web/copilot.css`, `web/sw.js`,
   `web/apps/settings/index.html`, each **with its `.gz` twin** (the device serves the `.gz` first),
   plus `data/anima/skills/{cucina.md,studio.md,crea-app.md,python.md,schermo.md,automazioni.md,casa.md,README.md.txt}`. Optional cleanup:
   `web/apps/anima/local-llm.js(.gz)` is no longer used.
3. Reload the web OS in the browser (the service-worker cache version changed, v117).

## Connecting an Ollama server (LAN) to ANIMA
On the PC (same Wi-Fi as the board):
1. Install Ollama, pull a model: `ollama pull qwen2.5:7b` (small/fast: `llama3.2:3b`).
2. Make it listen on the LAN, not only localhost: set `OLLAMA_HOST=0.0.0.0` then `ollama serve`
   (Windows: set it as a user environment variable and restart Ollama). Allow port 11434 in the
   firewall. Check from another device: `http://<pc-ip>:11434/v1/models` must answer.
3. Give the PC a fixed IP (router DHCP reservation), otherwise the address changes.

On ANIMA, web OS > Settings > IA:
4. Provider chip **Server locale**, then the **Ollama** preset (fills `http://<ip>:11434/v1`; edit
   the IP), press **↻ Elenco** to list the pulled models, pick one, **Salva**. No key needed.
   This writes `/data/anima/teacher.json` as `{"provider":"local","base":"http://<ip>:11434/v1","model":"…"}`.
5. Mode: **Locale** (LAN only: nothing goes to the internet) or **Ibrida** (offline first, then the
   model) or **LLM** (model first). Same from the board: `/mode local` in the ANIMA app, or
   `python tools/anima.py --mode local`.
6. Test: `python tools/anima.py --models` (lists the server's models) and
   `python tools/anima.py --mode llm "spiegami la fotosintesi in due frasi"`.

Notes: the board talks to Ollama itself (plain HTTP, OpenAI-compatible `/v1/chat/completions`,
timeouts up to 90 s per request because CPU-only models are slow); the browser goes through the
device relay `/api/llm`. LM Studio (`:1234/v1`) and llama.cpp server (`:8080/v1`) work the same way.
Tool-calling (`ACT …` lines) works with any instruct model; 7B+ follows it more reliably than 1–3B.
Troubleshooting: "unreachable" = wrong IP / firewall / OLLAMA_HOST not set; empty model list = no
model pulled; slow first answer = the model is loading into RAM.
