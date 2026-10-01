# Tasks for the local agent (needs the board / PC)

The cloud agent cannot flash or touch hardware. Do these on the PC with the board connected,
then report results (or fix and commit on `claude/zealous-sagan-wyjjor`).

1. **Build + flash**: `idf.py build flash monitor`. If `-Werror` fails, fix and commit.
   If `tools/ci/check_budgets.py` reports internal static > 210,000 B, move the new statics to PSRAM
   (`NV_PSRAM_BSS`, `EXT_RAM_BSS_ATTR`).
2. **SD**: sync `sd/` to the card (`.\tools\sync-sd.ps1 -Drive E:`), at least
   `data/anima/skills/crea-app.md` and `web/ai-keys.js(.gz)`, `web/sw.js(.gz)`.
3. **Shell checks** (terminal app, or `python tools/anima.py`):
   - `store search note`, `store info <id>`, `store install <id>` → tile appears in the launcher.
   - `launch luaapp` opens the Lua App launcher; `launch nope` → "no such app".
   - `lua -e "assert(loadfile('/lua/x.lua')) print('syntax ok')"` with a file in `/sdcard/home/lua/x.lua`.
   - a Lua script with an error: open it in Lua App, then `dmesg | tail -n 40` must show the traceback.
4. **ANIMA agent** (online model set up, e.g. Ollama 7B+):
   - "crea un'app lua che mostra un contatore" → ANIMA writes ~/lua/…, checks syntax, runs `launch luaapp`.
   - `/plan on` then "riorganizza i file in ~/" → it only reads and ends with a `- [ ]` plan; any
     write is refused. `/plan off` → it executes (asking first unless `/auto on`).
   - `/auto on` → no confirmations; a `"sh":"deny"` in permissions.json still blocks.
5. **Known gap**: the web Settings "autonomous" checkbox, when unchecked, removes `"mode"`, so it also
   turns plan mode off. Acceptable for now; a 3-way selector would be nicer.
6. Not yet verified at all: ESP-SR wake word (docs/HANDSFREE.md), heartbeat/Telegram on the device.
