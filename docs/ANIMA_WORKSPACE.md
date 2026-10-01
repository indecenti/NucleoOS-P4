# Il workspace di ANIMA (idee prese da OpenClaw e OpenCode)

File di testo sulla SD, in `/data/anima/`, che modifichi a mano (o dalle Impostazioni web ▸ IA ▸
Workspace). Nessuna ricompilazione: ANIMA li rilegge a ogni uso.

| File | A cosa serve |
|---|---|
| `SOUL.md` | Chi è ANIMA: tono, valori, limiti. Entra nel prompt del modello a ogni risposta. |
| `USER.md` | Chi sei tu: nome, città, preferenze. Entra nel prompt del modello. |
| `HEARTBEAT.md` | Checklist dei controlli proattivi (vedi sotto). |
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
