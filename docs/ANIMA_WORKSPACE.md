# ANIMA's workspace (ideas taken from OpenClaw and OpenCode)

Text files on the SD card, in `/data/anima/`, that you edit by hand (or from web Settings ▸ AI ▸
Workspace). No recompiling: ANIMA rereads them every time it uses them.

| File | What it is for |
|---|---|
| `SOUL.md` | Who ANIMA is: tone, values, limits. Goes into the model's prompt on every answer. |
| `USER.md` | Who you are: name, city, preferences. Goes into the model's prompt. |
| `memory.jsonl` | What ANIMA has learned about you (at most 48 facts): it updates it by itself (`ACT remember`, "ricordati che…" ("remember that…")) and it goes into the prompt of every chat (device, web, Telegram). You manage it from the Memory page of the web chat. An old `MEMORY.md` is imported automatically and renamed `MEMORY.md.migrated`. |
| `HEARTBEAT.md` | Checklist of the proactive checks (see below). |
| `telegram.json` | The bot's token and the linked chat (sealed to the chip, do not edit it by hand). |
| `permissions.json` | What a model may do on its own: `allow` / `ask` / `deny` per action. |
| `skills/*.md` | Skills: instructions activated by keywords (see `skills/README.md.txt`). |

## Permissions (like OpenCode)
This applies to the actions that **a model** decides with an `ACT …` line; the commands you give
yourself stay direct. Defaults: opening apps, volume, brightness, stopping the music → `allow`;
reminders and creating files → `ask` (ANIMA proposes and waits for "sì"/"no" ("yes"/"no"); the
proposal expires after 2 minutes).

    {"create_file": "allow", "add_event": "ask", "set_volume": "deny", "*": "ask"}

`"*"` applies to every action not listed.

## Heartbeat (like OpenClaw)
Every `anima.hb` minutes (15/30/60, or off: Settings ▸ Anima AI on the device or web Settings ▸ AI)
ANIMA passes the model the checklist, the time and today's and tomorrow's appointments.
If nothing needs attention the model answers `HEARTBEAT_OK` and nothing happens; otherwise
**one** notification arrives (with sound, unless Do Not Disturb is on). Without `HEARTBEAT.md`, in Offline mode or with no
model configured, no call is made.

Example `HEARTBEAT.md`:

    - C'è un impegno nelle prossime 2 ore? Ricordami cosa e quando.
    - Domani mattina è piena? Dimmelo stasera dopo le 20.

(In English: "Is there an appointment in the next 2 hours? Remind me what and when." / "Is tomorrow
morning full? Tell me this evening after 8 pm.")

## Telegram (OpenClaw's "channels")
You talk to ANIMA from anywhere, commands included ("abbassa il volume" ("lower the volume") acts
on the device).

1. On Telegram write to **@BotFather** → `/newbot` → copy the token.
2. Web Settings ▸ AI ▸ Telegram: paste the token and press *Connect bot* (the device verifies it
   with Telegram).
3. Send your bot the command shown (`/pair 123456`, also visible in Settings ▸ Anima AI on the
   device). The code is valid only once; from then on the bot answers only your chat.

Notifications from the proactive checks arrive there too. The device checks for messages every 15 s
(every 3 s for two minutes after an exchange); in Offline or Local mode the channel stays idle.

## The shell (an agent in the style of OpenCode / Claude Code)
The model works in steps with the device's Linux-like shell (the same one as the Terminal, run
without a screen): it writes `ACT sh <command>`, receives the output, can run others (at most 5), then
answers. Example: "quanto spazio mi resta?" ("how much space do I have left?") → `df -h` → "Hai 17 GB
liberi su 29" ("You have 17 GB free out of 29").

- **Read-only** (`ls cat grep find df du free date ps ip sensors store search|info …`, no `>`):
  always runs, without asking.
- **Changes something** (`rm cp mv mkdir touch sed -i curl -o store install …`): follows the `sh`
  permission (default `ask`: ANIMA proposes, you say "sì" ("yes")).
- **Full screen** (`edit less top watch …`): never, they need the Terminal.
- **Autonomous mode**: `"mode": "auto"` in `permissions.json` (web Settings ▸ AI, or `/auto on`
  in the ANIMA app): whatever would ask runs right away; an explicit `deny` still holds.

**Files (like OpenCode)**: `ACT write <path>` with the content between `<<<` and `>>>` writes a whole
file; `ACT edit <path>` with `<<< old === new >>>` changes one exact passage (it must appear only
once). Only under `~/` (= `/sdcard/home`), `/sdcard/data`, `/sdcard/apps`.
`write` permission (default `ask`; automatic in autonomous mode). The loop goes up to 12 steps.

**Programs**: `lua` and `js` also run without the Terminal screen, so the model sees
output and errors (`lua -e "assert(loadfile('/lua/x.lua'))"` for the syntax check: the Terminal's
`lua` sees `/sdcard/home` as `/`).

**Creating apps**: the skill `skills/crea-app.md` teaches the Lua App engine (gfx / ui / nv, minimal app,
write → check → fix loop). A script in `~/lua/<name>.lua` appears in the Lua App tile.

The store from the shell: `store search scacchi`, `store info chess`, `store install chess` (the icon
appears in the launcher right away).

## Python on the device
The skill `skills/python.md` teaches ANIMA to write `~/py/<name>.py` (`ACT write`), run it with
`ACT sh python /py/<name>.py`, read the traceback and fix it with `ACT edit`. The interpreter is the
store package `python` (MicroPython 1.26, `ports/micropython`); `store install python` adds it.

## Images: multimodal models and the vision helper
- ANIMA asks the server what the chat model can do: Ollama's `/api/show` lists `vision`, `tools`,
  `thinking`; for other servers the model family decides (qwen-vl, qwen3.5, gemma3, llava, gpt-4o,
  claude, gemini...). `"vision": true|false` in teacher.json overrides. `/caps` in the ANIMA app shows it.
- `ACT see <path>` (jpg/png under /sdcard, max 2 MB) attaches the image to the next request
  (OpenAI/Ollama `image_url` data URL, Anthropic `image` block). A model that cannot see gets a
  description written by the **vision helper** instead: `"vision_model": "qwen2.5vl:7b"` in
  teacher.json (same server; optional `"vision_base"`, `"vision_key"`). Multi-agent: the helper sees,
  the chat model reasons and acts.
- `screenshot [-d SEC] [FILE]` in the shell saves the screen as a JPEG (hardware encoder) to
  `~/shots/` and prints the path; without a FILE it is read-only for permissions.
- Computer use, as Playwright MCP does for browsers: `ui` prints the screen as an accessibility
  snapshot from the LVGL object tree (`[ref] role "text" @x,y`, switch/checkbox state, focus);
  `input tap @REF|TEXT|X Y`, `input text`, `input keyevent ENTER|ESCAPE|DEL|TAB|DPAD_*`,
  `input swipe` (Android's `adb shell input` syntax), `tap` and `home`. When the agent runs them,
  each action answers with the new snapshot. `ui` is read-only; the actions follow the `sh` permission.
- Pictures sent to ANIMA: `nucleo_anima_attach_image(path)` attaches one to the next question,
  which then always goes to the model (never the offline tiers): a model that sees gets it with
  the first request, otherwise the vision helper's description joins the text. Telegram uses it:
  a photo (the largest size under 1.9 MB) or an image sent as a file is saved to `~/inbox/` and
  the caption is the question ("Cosa c'e' in questa foto?" ("What is in this photo?") without one).
- Skill `skills/schermo.md`; `crea-app.md` uses it to check an app's look.

## The shell for models
The prompt says it is a BusyBox-like POSIX shell, so models use the coreutils they know, plus one
line of NucleoOS extras (store, apps, launch, screenshot, dmesg, sensors, python/lua/js, help CMD).
Commands added for models (each saves several calls or a whole file in the context): `diff -u A B`
(hunks identical to GNU diff), `jq [-rc] FILTER [FILE]` (`. .a.b .[N] .[] keys length`, `|` chains),
`sysinfo` (time, foreground app, Wi-Fi, SD, RAM, volume, brightness in one call; also `status`,
`neofetch`), `vol [N]`, `notify [-t T] TEXT`, `tg TEXT` (or `cmd | tg`), and option aliases like a
~/.bashrc: `ll` = ls -la, `la` = ls -A, `l` = ls -lA, `rg` = grep -rn. diff, jq, sysinfo, rg are
read-only (never ask); vol, notify and tg follow the `sh` permission.
Common names map to ours (`python3`/`py` -> python, `node` -> js, `vim`/`nano` -> edit),
and "command not found" says where to look (help, apps, store search) in one line.

## Native tool calling
A model that declares `tools` (Ollama `/api/show`) gets the agent's tools as OpenAI function
schemas (`sh`, `write_file`, `edit_file`, `see_image`, `device`) with a short prompt instead of the
text grammar; its `tool_calls` are translated into the same ACT lines, so permissions, plan/auto
modes, the loop and the tests are shared. Models without tools (and Claude, which follows the
grammar well) keep the `ACT ...` text grammar. Code: `kToolsJson`, `tool_call_to_act`, `s_tools`
in `nucleo_anima_online.c`.

## Timers and alarms (offline)
`nucleo_anima_time.c` understands spoken durations and clock times in Italian and English, with
the rules Duckling / chrono use: "timer di 10 minuti per la pasta" ("10-minute timer for the pasta"), "timer 1h30", "un'ora e mezza"
("an hour and a half"), "un quarto d'ora" ("a quarter of an hour"), "half an hour", "svegliami alle
7 e mezza" ("wake me at 7:30"), "alle 8 meno un quarto di sera" ("at a quarter to 8 in the
evening"), "a mezzogiorno" ("at noon"), "set an alarm for 7pm", "sveglia domani alle 6:45" ("alarm
tomorrow at 6:45"); "che timer ho?" ("what timers do I have?"), "annulla le sveglie" ("cancel the
alarms"). It is the first offline tool, so it works with no network and no model; models use
`ACT timer|alarm ...` (or the `device` tool). The store is `/data/anima/timers.json`; the OS checks
it once a second (cached, no SD read unless it changed) and rings with a notification and an alert
tone, also in Do Not Disturb (alarms ring longer).

## Skills: the Agent Skills standard
Besides `<name>.md` files, `skills/<name>/SKILL.md` folders in the open Agent Skills format
(agentskills.io; Claude Code, Codex, Gemini CLI, OpenClaw) and ESP-Claw's JSON front matter load
as they are. Without `triggers:` the description's keywords activate a skill. The agent's prompt
carries the catalog (`nucleo_anima_skills_catalog`: name, description, path) and the model reads a
SKILL.md, its `references/` and runs its `scripts/` through the shell when a task needs them
(progressive disclosure). See `sd/data/anima/skills/README.md.txt`.

## Automations (event rules)
`nucleo_anima_rules.c` ports the idea of ESP-Claw's event router (Apache-2.0) and keeps its rule
format (`id`, `match`, `actions`, `consume_on_match`, `ack`, `{{...}}` templates; `run_script` and
`kind` accepted), with NucleoOS events and actions:
- events: `schedule` (every minute: `at` HH:MM, `days` 0-6, `every` N minutes), `message` (Telegram;
  exact or `prefix` text, `{{match.remainder}}`), `startup`, `app_open` (`event_key` = app id);
- actions: `run_agent` (a prompt through the whole of ANIMA), `run_sh`, `run_script` (.lua/.py),
  `send_message` (telegram / notify / reply), `drop`.
Rules live in `/data/anima/rules.json`; the model adds them with `ACT rule add {json}` (permission
`rule`, default ask), `ACT rule list`, `ACT rule delete <id>` (same permission as add); skill `automazioni.md` has examples.
The OS posts events (only when rules.json exists) to a PSRAM task that runs them under the engine
gate; Telegram messages pass through the rules before ANIMA answers.

How it relates to what was already there (no overlap):
- heartbeat (HEARTBEAT.md every N minutes): one built-in proactive check whose answer is silent
  unless something needs the user; rules are exact, user-made routines;
- calendar reminders: one dated event; timers/alarms: one countdown or one time; rules: recurring
  or event-driven;
- ESP-Claw's own component needs its whole runtime (claw_core, capability registry, Lua engine on
  the device's main firmware): ANIMA keeps its engine, tools and permissions and reads the same rule
  format instead.

## Memory recall by relevance
MEMORY.md keeps its format (edit it freely). For a request, the model now gets the lines that share
its words (any age), the 5 most recent and the catalog of `#labels` with counts (claw_memory's idea,
ESP-Claw), plus a hint to search the rest with `rg`; without a request, the most recent part as
before. `ACT remember <fact> #label` adds a label; `ACT forget <words>` removes the facts containing
all those words (permission `remember`). Code: `memory_block`, `nucleo_anima_memory_forget` in
`nucleo_anima_skills.c`. memory.jsonl (the "ricordati che…" ("remember that…") capture) is unchanged.

## Conversations (sessions, grouped by workspace)

Like Claude Code keeps one session per project, the ANIMA app keeps every conversation in its own
folder: `/sdcard/data/anima/sessions/<id>/` holds `chat.ndjson` (the transcript the app replays),
`context.json` (what the model sees: window + compacted summary, the engine's own file) and
`meta.json` (`title`, `ws`, `created`); `sessions/current` names the one in use. The first question
titles a conversation; it belongs to the workspace it was started in and follows the workspace when
you change it. The newest 50 are kept.

- **New**: the `+` key on the bar, `/new` or Ctrl+T (a blank conversation is reused, not duplicated).
- **List**: the list key on the bar, `/sessions` or Ctrl+B opens a sidebar on the left, grouped by
  workspace, newest first, with how long ago each was used. Tap / Enter opens one; long press /
  Menu key: open or delete. Esc or a tap beside it closes it.
- `/rename title`, `/delete` (asks first), `/clear` empties the current one.

Switching hands the engine the other `context.json` (`nucleo_anima_session_open`): the model sees
only that conversation, the working memory starts over. The old single transcript
(`chatlog.ndjson` + `context.json`) becomes the first session on the first run.

## Context compaction in long agent runs
Past ~9 KB of steps in one turn, the steps older than the last two keep only the head of their
output ("...[older output trimmed]") and the first line of a written file ("(file content elided)").
Deterministic, no extra model call (OpenCode's compaction, Claude Code's tool-result clearing): a
small local model with a short context keeps the task instead of silently losing its start.
Code: `compact_steps` in `nucleo_anima_online.c`, before every request of the loop.

## Smart home from the shell: `ha` and `dev`
- `ha` uses the Home Assistant URL + token saved in Settings > Home (never printed):
  `ha say TEXT` hands the sentence to Assist (`/api/conversation/process`, the board's language):
  Home Assistant resolves names, rooms and Italian itself, one call for the model. `ha ls [room|
  name|domain]`, `ha find TEXT`: one line per entity (`light.cucina on 80% "Luce cucina" @Cucina`),
  filtered and formatted by Home Assistant through `/api/template`, so a large installation never
  sends megabytes of `/api/states` to the board; lists end with "+N more" or "(no matching
  entities)". `ha get ENTITY`, `ha on|off|toggle ENTITY|NAME`, `ha set ENTITY k=v` (the right
  service per domain: light, climate, cover, media_player, fan, number, select), `ha call D.S`,
  `ha status`. After an action it prints the entity's new state, so the model can verify.
- `dev`: LAN devices without Home Assistant, through their local APIs: Shelly (Gen2 RPC, Gen1
  fallback), Tasmota (`/cm?cmnd=`), WLED (`/json/state`); `dev scan` finds Shelly/WLED by mDNS,
  `dev add NAME TYPE IP` for the rest; `/sdcard/data/devices.json`.
- Reading (`ls find get status scan`) never asks; acting follows the `sh` permission.
- Skill `casa.md`: Assist first, then precise commands, then `dev`; automations can call them.
The Jinja templates were checked with jinja2 (Home Assistant's engine) on sample states.
- Home events in automations: rules with `"event_type":"ha_state"`, `"event_key"` = entity id and
  optional `"to"`/`"from"` fire when Home Assistant reports a change. Every 5 s, and only while such
  rules exist, the automations task sends one `/api/template` request returning `entity=state` for the
  watched entities (a few lines); the engine diffs them (`nucleo_anima_rules_ha_watch/_ha_states`),
  primes on the first answer and ignores `unavailable`/`unknown` flapping. It uses the token from
  Settings > Home directly, so it works in ANIMA's offline mode too.

## The dev loop: write -> check -> run -> fix
- After every `ACT write`/`edit` of a .lua/.py/.json, ANIMA runs `app check` and the result carries
  `CHECK:` with "ok" or the error and the bad line (OpenCode's post-edit diagnostics, Aider's
  auto-lint): no extra step for the model.
- `app run NAME` starts a Lua App script directly (`~/lua/.run`, engine change) and returns after a
  few seconds either the exact error (`~/lua/.last_error`: message, traceback, the bad line) or
  "running, no errors" with a screenshot path for `ACT see`. Skill `crea-app.md` drives the loop
  (at most 4 rounds). `app check`/`app ls` never ask; `app run` follows the `sh` permission.

### Managing the OS (skill `sistema`)
- `cfg` lists the whitelisted system settings (value + meaning); `cfg KEY VALUE` (or `KEY=VALUE`)
  writes nv_config and fires `NV_EV_SETTINGS_CHANGED`, so Settings, theme and screen apply it live.
  Secrets (ha_token, mqtt_pass, lockpin) print as `***`. Reading never asks; writing follows `sh`.
  `ota_url`, `store_url`, `lock_en`, `lockpin` are read-only from the shell (anti prompt-injection:
  a model must not redirect firmware/app sources or drop the lock screen).
- `wifi [status|scan|on|off|join SSID [PASS]|leave|forget SSID]`: status/scan never ask.
- Together with `apps`/`launch`/`home`, `store`, `update`, `bl`, `usb`, `ps`, `dmesg`, `sysinfo`
  ANIMA covers every Settings page from the shell.
- `store remove ID` uninstalls via `nv_wasm_uninstall` (system apps, the running app and packages
  other apps require are refused) and drops the Home tile live; it always asks.
- `cfg export` prints `KEY=VALUE` lines without secrets or read-only keys; `cfg import FILE` applies
  them through the same checks as `cfg KEY VALUE` (import asks).
- Verified on the PC: the rebuilt engine under WAMR (`luahost`) runs `.run`, writes `.last_error`,
  rejects paths outside /lua; the lua/python check commands with the real apps under `nvhost`.

### Permission hardening (audit, Oct 2026)
- The `sh` classifier reads the **first argument** of subcommand tools (`store`, `app`, `cfg`,
  `wifi`, `ha`, `dev`), splitting words on space/tab like the shell lexer: `store remove x info`
  or `cfg brightness<TAB>5` are no longer "safe". `sed` is a writer for any option cluster with `i`
  (`-i -ni -Ei --in-place`). `dev scan` asks (it rewrites devices.json).
- A pending "ask" confirmation is bound to its **origin** (`nucleo_anima_set_origin`: screen, web,
  tg, rule): a "sì" from another channel never approves it, and automations never confirm anything.
- `ACT rule delete` follows the `rule` permission (deny / ask) like `add`.
- `dev` refuses to save when devices.json is unreadable instead of overwriting it with an empty list.

### The agent bar (ANIMA app, bottom row)
One low row (30 px segments) styled as the terminal's status line: flat segments on the key-row
band, coloured glyph + mono text, hairline separators, a press tint; the Esc/Tab///arrow keys are
gone (a physical keyboard still has them). Left: workspace | model | context | permissions;
right: Stop | attach | mic | settings.
- **workspace** (folder glyph, blue): tap -> pick `~`, `~/projects/*`, `~/lua/*`, `~/python/*` or
  `/apps/*`. `nucleo_anima_set_workspace()` makes the shell `cd` there before ANIMA's next command and
  adds a `WORKSPACE:` line to the shell grammar; persisted as `anima.ws` (also used by Telegram).
- **model**: tap -> the server's model list (`nucleo_anima_teacher_models`, worker job), the
  current one marked; picking writes teacher.json like `/model NAME`.
- **context**: `used/window` + a slim meter (green, amber >= 60%, red >= 85%) and, from 50%, the
  countdown to auto-compact (`auto 18%`, `compacts`, `auto off`); tap for details, long-press to
  compact now. Used = the server's `usage` or ~chars/4; window = Ollama `context_length` when
  detected, else the model family (Ollama's effective `num_ctx` may be smaller).
- **permissions**: tap cycles Ask -> Auto -> Plan (`nucleo_anima_set_agent_mode`). Auto = Claude
  Code's skip-permissions: nothing asks, but `deny` entries, screen-only commands and the cfg
  read-only keys still hold; a red frame while Auto is on. It applies to every channel.
- **Stop** (red while ANIMA works): `nucleo_anima_cancel()` - every model call still to come in the
  turn (cloud API, Ollama/LAN server, compaction) gives up before and right after each HTTP attempt
  and between agent steps; the answer is discarded, **no action it proposed runs**, a shell command
  ANIMA started is interrupted (`sh_interrupt`, ANIMA's headless run only), the turn is not added to
  the context. A request already on the wire finishes in the background (bounded by its timeout)
  and is dropped: ESP-IDF's HTTP client cannot be aborted safely from another task.
- **paperclip**: the 40 newest files in ~/shots, /DCIM, ~, ~/Downloads. An image is attached to the
  next question (the model sees it; tap again to drop it); another file is named in the prompt as
  `[file ~/x]` for the model to read.

### Esc / Back (OS-wide)
Esc goes back *inside* an app (an "Open with" sheet, the app's back handler: sub-pages, modals) and
never closes it by surprise; leaving is the title bar's X, Alt+F4 or the edge gestures. A
full-screen app (no title bar) still closes on Esc. In ANIMA, Back/Esc closes the picker, then the
settings view, then stops a turn in flight (`back_sync`); idle, Esc does nothing and the left-edge
gesture closes the app. Tapping the "Thinking..." row also stops.

### Context compaction (Claude Code's /compact + auto-compact)
- The chat keeps the last 6 turns verbatim (240/700 chars each, was 4 x 80/200). A turn leaving that
  window is **folded**, never dropped: it waits in a fold buffer until the next compaction.
- **Compaction** = one `nucleo_anima_teacher_complete` call merging (previous summary + folded turns
  + older window turns) into ONE structured summary (Goal / Done / Decisions / Files-paths-commands
  / Errors and fixes / Open), max ~800 chars, injected in the system block as "SUMMARY OF THE EARLIER
  CONVERSATION". Same rolling-summary scheme as the web conversations (nucleo_anima_conv.c).
- **Auto** (default on, `anima.acomp`): before a turn, when the last request used >= 80% of the
  model's window (keeps only the last turn verbatim), or when the fold buffer passes ~1.6 KB.
  **Manual**: `/compact [focus]` (e.g. `/compact il gioco` ("the game")) or a long press on the context chip;
  `/compact auto on|off`.
- **Indicator** (agent bar): the context chip's caption reads "auto N%" from 50%,
  "compacts" at 80%, "auto off" when disabled; the value shows "compacting…" while it
  runs, the spinner says "Compacting the context", and a "■ Context compacted: N turns summarised,
  ~Xk tokens freed (the summary stays in context)" line follows. `/clear` drops the summary too.
- Sized to the model: summary up to 800 chars (window < 16k or unknown), 1400 (16k+), 2400 (64k+);
  the backlog compacts at 1.6 / 3 / 4.8 KB. Cloud models with room compact less and keep more.
- Persistent: summary, fold buffer and the verbatim window live in `/sdcard/data/anima/context.json`
  (temp file + commit, only when changed), reloaded at engine init; `/clear` deletes it.
- Note: the screen chat and Telegram share the engine's session ring, hence one summary.
- Not done (deliberately, needs on-board validation): window-driven pruning of old tool outputs
  before summarising (Anthropic context editing / OpenCode prune) and a model-invoked `ACT compact`.
