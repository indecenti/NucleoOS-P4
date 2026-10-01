---
name: automazioni
description: create, list and delete automations (rules): do something every day/at a time/when a message or an app arrives
triggers: se si apre, quando si accende, quando si spegne, quando cambia, automazione, automazioni, ogni giorno, ogni mattina, ogni sera, ogni ora, ogni lunedi, tutti i giorni, quando apro, quando arriva, quando scrivo, regola, regole, routine, every day, every morning, every hour, automation, automations, when i open, rule
offline: Per creare un'automazione serve un modello; quelle gia' salvate funzionano anche offline. Elenco: ACT rule list.
---
Automations live in /sdcard/data/anima/rules.json and run on the device by themselves.
Create one with a single line (JSON may span lines), the user confirms:
ACT rule add {"id":"short-id","description":"what it does, in the user's words","match":{...},"actions":[...]}
List: ACT rule list. Delete: ACT rule delete <id>. Same id = replace.

match (event_type and its fields):
- {"event_type":"schedule","at":"07:30","days":"1-5"}   (days: 0=Sun..6=Sat, ranges/lists; omit = every day)
- {"event_type":"schedule","every":60}                   (every N minutes from midnight)
- {"event_type":"message","text":"/luce","text_match_rule":"prefix"}   (Telegram; the rest is {{match.remainder}})
- {"event_type":"app_open","event_key":"music"}          (an app opened: ids from ACT sh apps)
- {"event_type":"startup"}                               (at boot)
- {"event_type":"ha_state","event_key":"binary_sensor.porta","to":"on"}   (a Home Assistant entity
  changes; optional "to"/"from"; {{event.text}} = new state, {{event.from}} = old; checked every 5 s;
  find ids with ACT sh ha find NAME)
actions, in order (input fields):
- {"type":"run_agent","input":{"prompt":"..."}}   ANIMA answers it with all its tools -> {{last.output}}
- {"type":"run_sh","input":{"command":"..."}}       a shell command line -> {{last.output}}
- {"type":"send_message","input":{"channel":"telegram"|"notify"|"reply","text":"...{{last.output}}..."}}
- {"type":"drop"}                                   stop here (for message rules: nothing else answers)
Add "consume_on_match":true to a message rule that fully handles the message; "ack" = its reply.
Templates: {{last.output}} {{match.remainder}} {{event.text}} {{event.key}} {{date}} {{time}}.

Examples:
"ogni mattina alle 7:30 nei giorni feriali dimmi il meteo su Telegram" ->
ACT rule add {"id":"meteo-mattina","description":"meteo alle 7:30 nei giorni feriali su Telegram","match":{"event_type":"schedule","at":"07:30","days":"1-5"},"actions":[{"type":"run_agent","input":{"prompt":"Che tempo fa oggi? Rispondi in due righe."}},{"type":"send_message","input":{"channel":"telegram","text":"{{last.output}}"}}]}
"quando apro la musica abbassa la luminosita' al 30" ->
ACT rule add {"id":"musica-luce","description":"luminosita' 30 quando apro la musica","match":{"event_type":"app_open","event_key":"music"},"actions":[{"type":"run_sh","input":{"command":"bl 30"}}]}
"se ti scrivo /spazio su Telegram rispondi con lo spazio libero" ->
ACT rule add {"id":"spazio","description":"/spazio su Telegram: spazio libero","consume_on_match":true,"match":{"event_type":"message","text":"/spazio","text_match_rule":"prefix"},"actions":[{"type":"run_sh","input":{"command":"df -h /sdcard | tail -n 1"}}]}
"se si apre la porta d'ingresso mandami un Telegram" ->
ACT rule add {"id":"porta-telegram","description":"porta aperta: Telegram","match":{"event_type":"ha_state","event_key":"binary_sensor.porta_ingresso","to":"on"},"actions":[{"type":"send_message","input":{"channel":"telegram","text":"Porta d'ingresso aperta alle {{time}}"}}]}
Prefer run_sh for exact device commands (cheap, offline); run_agent when it needs reasoning or the web.
