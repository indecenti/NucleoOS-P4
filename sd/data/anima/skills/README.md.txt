Skill di ANIMA, in /sdcard/data/anima/skills/. Due formati, si aggiungono copiando: niente da ricompilare.

1) Un file <nome>.md (formato di ANIMA). Intestazione tra due righe "---":
     name:        nome breve
     description: a cosa serve
     triggers:    parole o frasi, separate da virgola, che la attivano
     offline:     (facoltativo) risposta da usare senza rete
   Sotto: istruzioni per il modello (si usano i primi 4000 caratteri).

2) Una cartella <nome>/SKILL.md, lo standard aperto Agent Skills (agentskills.io), lo stesso di
   Claude Code, Codex, Gemini CLI, OpenClaw: intestazione YAML con name e description (anche su
   piu' righe con > o |), piu' cartelle facoltative scripts/, references/, assets/. Vanno bene
   anche le skill di ESP-Claw (intestazione JSON). Senza "triggers" si attiva dalle parole della
   description. Nel prompt dell'agente c'e' il catalogo (nome, descrizione, percorso): il modello
   legge SKILL.md e i suoi file con la shell solo quando servono (python scripts/x.py per gli script).
   Per installarne una da GitHub, ANIMA (o tu nel Terminale) puo' fare:
     mkdir -p /sdcard/data/anima/skills/NOME
     curl -sL https://raw.githubusercontent.com/UTENTE/REPO/main/skills/NOME/SKILL.md -o /sdcard/data/anima/skills/NOME/SKILL.md

Le skill attive in una domanda sono al massimo 2.
