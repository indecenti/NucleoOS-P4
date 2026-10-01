# Il workspace di ANIMA (idee prese da OpenClaw e OpenCode)

File di testo sulla SD, in `/data/anima/`, che modifichi a mano (o dalle Impostazioni web ▸ IA ▸
Workspace). Nessuna ricompilazione: ANIMA li rilegge a ogni uso.

| File | A cosa serve |
|---|---|
| `SOUL.md` | Chi è ANIMA: tono, valori, limiti. Entra nel prompt del modello a ogni risposta. |
| `USER.md` | Chi sei tu: nome, città, preferenze. Entra nel prompt del modello. |
| `MEMORY.md` | Cosa ANIMA ha imparato di te: lo aggiorna da sola (`ACT remember`), le righe più recenti entrano nel prompt. Correggilo quando vuoi. |
| `HEARTBEAT.md` | Checklist dei controlli proattivi (vedi sotto). |
| `telegram.json` | Token del bot e chat collegata (sigillato al chip, non modificarlo a mano). |
| `permissions.json` | Cosa può fare un modello da solo: `allow` / `ask` / `deny` per azione. |
| `skills/*.md` | Skill: istruzioni attivate da parole chiave (vedi `skills/README.md.txt`). |

## Permessi (come OpenCode)
Vale per le azioni che **un modello** decide con una riga `ACT …`; i comandi che dai tu restano
diretti. Default: aprire app, volume, luminosità, fermare la musica → `allow`; promemoria e creare
file → `ask` (ANIMA propone e aspetta "sì"/"no", la proposta scade dopo 2 minuti).

    {"create_file": "allow", "add_event": "ask", "set_volume": "deny", "*": "ask"}

`"*"` vale per ogni azione non elencata.

## Heartbeat (come OpenClaw)
Ogni `anima.hb` minuti (15/30/60, oppure spento: Impostazioni ▸ Anima sul dispositivo o
Impostazioni web ▸ IA) ANIMA passa al modello la checklist, l'ora e gli impegni di oggi e domani.
Se niente richiede attenzione il modello risponde `HEARTBEAT_OK` e non succede nulla; altrimenti
arriva **una** notifica (con suono, salvo "non disturbare"). Senza `HEARTBEAT.md`, in modalità
Offline o senza un modello configurato non parte nessuna chiamata.

Esempio di `HEARTBEAT.md`:

    - C'è un impegno nelle prossime 2 ore? Ricordami cosa e quando.
    - Domani mattina è piena? Dimmelo stasera dopo le 20.

## Telegram (i "canali" di OpenClaw)
Parli con ANIMA da ovunque, comandi compresi ("abbassa il volume" agisce sul dispositivo).

1. Su Telegram scrivi a **@BotFather** → `/newbot` → copia il token.
2. Impostazioni web ▸ IA ▸ Telegram: incolla il token e premi *Collega bot* (il dispositivo lo
   verifica con Telegram).
3. Invia al tuo bot il comando mostrato (`/pair 123456`, visibile anche in Impostazioni ▸ Anima sul
   dispositivo). Il codice vale una volta sola; da quel momento risponde solo alla tua chat.

Le notifiche dei controlli proattivi arrivano anche lì. Il dispositivo controlla i messaggi ogni 15 s
(ogni 3 s per due minuti dopo uno scambio); in modalità Offline o Locale il canale resta fermo.

## La shell (agente in stile OpenCode / Claude Code)
Il modello lavora a passi con la shell Linux-like del dispositivo (la stessa del Terminale, eseguita
senza schermo): scrive `ACT sh <comando>`, riceve l'output, ne può eseguire altri (massimo 5), poi
risponde. Esempio: "quanto spazio mi resta?" → `df -h` → "Hai 17 GB liberi su 29".

- **Sola lettura** (`ls cat grep find df du free date ps ip sensors store search|info …`, niente `>`):
  parte sempre, senza chiedere.
- **Modifica qualcosa** (`rm cp mv mkdir touch sed -i curl -o store install …`): segue il permesso
  `sh` (default `ask`: ANIMA propone, tu dici "sì").
- **Schermo intero** (`edit less top watch …`): mai, servono il Terminale.
- **Modalità autonoma**: `"mode": "auto"` in `permissions.json` (Impostazioni web ▸ IA, o `/auto on`
  nell'app ANIMA): ciò che chiederebbe parte subito; un `deny` esplicito resta valido.

**File (come OpenCode)**: `ACT write <percorso>` con il contenuto tra `<<<` e `>>>` scrive un file
intero; `ACT edit <percorso>` con `<<< vecchio === nuovo >>>` cambia un passaggio esatto (deve
comparire una volta sola). Solo sotto `~/` (= `/sdcard/home`), `/sdcard/data`, `/sdcard/apps`.
Permesso `write` (default `ask`; automatico in modalità autonoma). Il ciclo arriva a 12 passi.

**Programmi**: `lua` e `js` girano anche senza la schermata del Terminale, così il modello vede
output ed errori (`lua -e "assert(loadfile('/lua/x.lua'))"` per il controllo di sintassi: il `lua`
del terminale vede `/sdcard/home` come `/`).

**Creare app**: la skill `skills/crea-app.md` insegna il motore Lua App (gfx / ui / nv, app minima,
ciclo scrivi → controlla → correggi). Uno script in `~/lua/<nome>.lua` compare nella tile Lua App.

Lo store dalla shell: `store search scacchi`, `store info chess`, `store install chess` (l'icona
compare subito nel launcher).

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
  the caption is the question ("Cosa c'e' in questa foto?" without one).
- Skill `skills/schermo.md`; `crea-app.md` uses it to check an app's look.

## The shell for models
The prompt says it is a BusyBox-like POSIX shell, so models use the coreutils they know, plus one
line of NucleoOS extras (store, apps, launch, screenshot, dmesg, sensors, python/lua/js, help CMD).
Common names map to ours (`python3`/`py` -> python, `node` -> js, `vim`/`nano` -> edit, `jq` -> cjson),
and "command not found" says where to look (help, apps, store search) in one line.

## Native tool calling
A model that declares `tools` (Ollama `/api/show`) gets the agent's tools as OpenAI function
schemas (`sh`, `write_file`, `edit_file`, `see_image`, `device`) with a short prompt instead of the
text grammar; its `tool_calls` are translated into the same ACT lines, so permissions, plan/auto
modes, the loop and the tests are shared. Models without tools (and Claude, which follows the
grammar well) keep the `ACT ...` text grammar. Code: `kToolsJson`, `tool_call_to_act`, `s_tools`
in `nucleo_anima_online.c`.
