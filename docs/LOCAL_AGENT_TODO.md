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
5. **MicroPython (`apps/python`)**: `app.wasm` is committed (built and tested in the cloud).
   - riscv32 AOT: `wamrc --target=riscv32 --target-abi=ilp32f --cpu=generic-rv32
     --cpu-features=+m,+a,+c,+f --enable-multi-thread -o apps/python/app.aot apps/python/app.wasm`
     (same as `aot()` in `ports/build.sh`; must be <= 4 MB).
   - Rebuild/verify from source in WSL: `bash ports/micropython/build.sh` and, with
     `/root/nvhost` from `ports/host/build.sh`, `bash ports/micropython/build.sh test`.
   - Publish: `python tools/dist.py store` (catalog entry `python` is in `server/appstore/catalog.json`).
   - On the board: `store install python`, then in the Terminal `python` (REPL, a `for` block,
     `exit()`), `python -c "import os; print(os.listdir('/'))"`, a script in ~/py with a deliberate
     error (traceback with line number), `def r(n): return r(n+1)` then `r(0)` → `RuntimeError`,
     not a crash. Report speed: `python -c "import time;t=time.ticks_ms();sum(range(10**6));print(time.ticks_ms()-t)"`.
   - ANIMA: copy `sd/data/anima/skills/python.md`; ask "scrivi uno script python che conta le
     parole di ~/py/testo.txt" → it writes, runs `python`, fixes errors.
6. **Multimodal (Qwen3.5 9B on Ollama)**: `/caps` in ANIMA must say `vision [server]`. Ask "cosa
   vedi sullo schermo?" -> trace `sh screenshot > see`, and the answer matches the screen. With a
   text-only model + `"vision_model"` in teacher.json -> trace `see(helper)`. Check the request size
   (a 1024x600 JPEG ~150 KB, ~200 KB base64) goes through and how long the model takes.
   Computer use: open Settings, run `ui` in the Terminal (items with [ref] and @x,y must match the
   screen), `input tap @REF` on a switch, `input text ciao` in a field, `input keyevent ENTER`.
   ANIMA: "attiva il bluetooth dalle impostazioni" -> trace `sh launch > sh ui > sh input tap`.
   Telegram: send the bot a photo with a caption -> the answer is about the photo; file in ~/inbox.
   Shell: `screenshot`, `screenshot -d 3`, `python3 -c "print(1)"` (alias), `foo` (one-line hint).
7. **Timers/alarms offline**: Wi-Fi off, "metti un timer di 1 minuto per la pasta" -> after a
   minute a notification and the alert tone; "svegliami alle HH:MM" (two minutes ahead); "che timer
   ho?"; "annulla le sveglie". Check the tone is audible and stops after a few seconds.
8. **Automations**: ask ANIMA "ogni giorno alle HH:MM (tra due minuti) mandami su Telegram lo
   spazio libero" -> it proposes the rule, "si'", and at that minute the message arrives. Telegram
   "/spazio" with the example rule from skills/automazioni.md answers without the model. Open Music
   with a musica-luce rule -> brightness changes. `cat /sdcard/data/anima/rules.json`.
9. **Shell for models**: `sysinfo`, `ll ~`, `rg anima /sdcard/data/anima/skills | head`,
   `cat /sdcard/data/anima/permissions.json | jq -r .mode`, `cp a b; echo x >> b; diff -u a b`,
   `vol 40`, `notify ciao`, `tg prova` (paired Telegram). They were syntax-checked, diff/jq logic
   tested on the PC; not yet run on the board.
10. **Home**: with Home Assistant set in Settings > Casa: `ha status`, `ha ls`, `ha ls cucina`,
    `ha say accendi la luce della cucina`, `ha off <name>`, `ha set light.x brightness_pct=30`.
    Without HA: `dev scan`, `dev ls`, `dev toggle <name>`; a Tasmota by `dev add NAME tasmota IP`.
    ANIMA: "spegni le luci del salotto", "che temperatura c'e' in sala?".
    Home events: "quando accendo <una luce> mandami un Telegram" -> rule ha_state; toggle the light
    in HA and the message arrives within ~5 s.
11. **App Casa** (`apps/casa`, app.wasm built with wasi-sdk clang, freestanding like the SDK default):
    publish with `python tools/dist.py store` (it signs package.sig). On the board with HA set:
    rooms, toggle a light, its brightness bar, a thermostat -/+, a scene; 5 s refresh; swipe pages.
12. **Dev loop**: the Lua App engine changed (`apps/luaapp/app.wasm` rebuilt with
    `ports/luaapp/build_linux.sh`): build its riscv32 AOT (`bash ports/luaapp/build.sh`) and
    re-sign/publish it. Then: `app ls`, `app check ~/lua/x.lua`, `app run x` with a script that
    errors (the error + bad line) and one that works (screen path). ANIMA: "crea un'app lua con un
    contatore che si incrementa al tocco" -> write, CHECK, app run, see.
13. **Known gap**: the web Settings "autonomous" checkbox, when unchecked, removes `"mode"`, so it also
   turns plan mode off. Acceptable for now; a 3-way selector would be nicer.
14. Not yet verified at all: ESP-SR wake word (docs/HANDSFREE.md), heartbeat/Telegram on the device.

## 13. ANIMA OS management (cfg / wifi) — verify on the board
- Build and flash; in the Terminal: `cfg`, `cfg brightness 40` (screen dims live), `cfg dnd=1`,
  `cfg ota_url x` must answer "read-only here", `wifi`, `wifi scan`.
- Ask ANIMA "abbassa la luminosità al 30%": it must run `cfg brightness 30` and ask permission first
  (mode `ask`). `cfg` handlers run under `lvgl_port_lock`: check no deadlock with Settings open.
- `store remove ID`: the tile disappears from Home without reboot; `store remove lua` (system app)
  must be refused. `cfg export > ~/cfg.txt`, change brightness, `cfg import ~/cfg.txt` restores it.

## 14. ANIMA agent bar — check on the board
- Layout at 1024x600 (chips 52 px high, no overflow with a long model name / workspace).
- Model picker with Ollama (qwen3.5:9b): list appears, the pick sticks after reopening the app.
- Context chip fills after a turn; Ask/Auto/Plan cycles and survives a reboot (permissions.json).
- Paperclip: a screenshot from ~/shots reaches a vision model; tap on "Thinking…" interrupts.
