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

Lo store dalla shell: `store search scacchi`, `store info chess`, `store install chess` (l'icona
compare subito nel launcher).
