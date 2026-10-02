# ANIMA L0: comandi in linguaggio naturale, offline

Come ANIMA capisce le richieste al dispositivo senza modello e senza rete, e come lo si fa crescere.
Il codice è in `components/nv_anima/`; i test in `tests/host/unit/test_anima_nl.cpp`.

## I quattro strati, nell'ordine in cui rispondono

1. **Regole** (`nucleo_anima.c`, `l0_query`): verbi, oggetti e direzione ("alza / abbassa", "troppo alto" →
   abbassa, "giù / su"). Deterministiche, e le uniche che decidono un'azione da sole.
2. **Parafrasi** (`anima_phrases.c`, generato): una tabella "frase normalizzata → frase canonica". Una frase in
   tabella viene riscritta nella canonica, che le regole capiscono già: niente logica duplicata. Fonti:
   - `tools/anima_phrases.txt`: modelli a mano, `(a|b)` = una delle due, `[x]` = facoltativo;
   - `tools/anima_phrases_ha.txt`: frasi di test di Home Assistant (CC-BY-4.0), importate con
     `tools/import_ha_intents.py`; in caso di conflitto vincono sempre le nostre.
3. **Frasi imparate** (`/data/anima/phrases.user.tsv` sulla SD): quelle che l'utente ha confermato con un "sì"
   (vedi sotto). Consultate dopo la tabella di serie, che vince sempre; allocate in PSRAM al primo uso.
4. **Suggeritore** (`anima_intent.c` + `anima_intent_model.c`, generato): quando nessuno strato ha capito,
   un classificatore (regressione logistica su parole, coppie di parole e n-grammi di caratteri, pesi int8,
   ~300 KB in flash) propone la richiesta più vicina: *"Non sono sicuro di aver capito: intendi «alza il
   volume»? (sì/no)"*. **Non esegue mai niente da solo.**
   - Soglie: probabilità ≥ 0,45 e margine sul secondo ≥ 0,15, poi la lingua della classe.
   - Filtro di polarità: non propone mai la direzione opposta alle parole dette ("più silenzio" non sarà
     mai «alza il volume»).
   - "sì" → esegue la canonica e impara la frase (strato 3); "no" → chiede altre parole e non impara;
     qualsiasi altra cosa → la proposta decade. Solo chi l'ha ricevuta può rispondere, entro un minuto.

Quando nulla risponde, il "non lo so" dice che cosa funziona offline: mai una risposta vuota.

## Strumenti (sul PC)

| Comando | Cosa fa |
|---|---|
| `python tools/gen_anima_phrases.py [--check]` | rigenera tabella e test dalle parafrasi (`--check`: fallisce se non aggiornati, come in CI) |
| `python tools/import_ha_intents.py <checkout home-assistant/intents>` | reimporta le frasi di Home Assistant |
| `python tools/import_nucleo_evals.py <checkout NucleoOs>` | reimporta i corpus di azioni dell'ANIMA del Cardputer come test |
| `python tools/augment_anima_intent.py [--verify]` | **GPU**: un LLM locale (Ollama, default `qwen3.5:9b`) genera parafrasi e negativi "difficili"; `--verify` fa da giudice e tiene solo quelle giuste (scarti in `.rejected`) |
| `python tools/train_anima_intent.py [--eval-only]` | addestra il suggeritore, lo valuta sul set mai visto e scrive i pesi C + i casi golden |
| `python tools/anima_misses.py [--promote]` | dalla scheda: frasi non capite, proposte fatte, frasi imparate; `--promote` porta queste ultime nella tabella di serie |

I tre script di addestramento richiedono `numpy`, `scikit-learn`, `pyyaml` (solo sul PC; quello che va
nel firmware è C generato e committato).

## Il ciclo di miglioramento

1. La scheda registra ogni turno (`telemetry.ndjson`) e le frasi confermate (`phrases.user.tsv`).
2. `anima_misses.py` mostra cosa non è stato capito e cosa proporrebbe il suggeritore.
3. Le frasi giuste diventano modelli in `anima_phrases.txt` (o `--promote` per quelle confermate).
4. `augment_anima_intent.py` + `--verify` allargano i dati di addestramento con la GPU.
5. `gen_anima_phrases.py`, `train_anima_intent.py`, `make -C tests/host`: tabella, pesi e test.

## Le misure (160 frasi scritte a mano, mai viste)

Il set `tools/anima_intent_heldout.txt` ha 160 frasi: 30 sono già nella tabella (le capisce L0), le altre misurano
il suggeritore. Un controllo di contaminazione toglie dall'addestramento ogni frase uguale a una di prova.

| Variante provata | Proposte giuste | Sbagliate | Frasi non comandi con proposta | Esito |
|---|---|---|---|---|
| Frasi LLM giudicate dal 9B | 62/95 | 5 | 5/35 | sostituita |
| **Frasi LLM riviste a mano + filtri stato/passato/negazione** | **62/95** | **2** | **3/35** | **in uso** |
| Conformal prediction (CICC) | meno proposte, 0 sbagliate | | | scartata: con la conferma, una proposta mancata costa più di una sbagliata |
| Energy score per il fuori tema | AUROC 0,82 contro 0,975 della classe "-" | | | scartato |
| Filtro per accordo tra modelli | 31 contro 37 (set da 57) | | | scartato: toglie i negativi difficili, i dati più utili |
| Negativi difficili e coppie confondibili generati | 33-35 contro 37 (set da 57) | | | scartati: troppe etichette sbagliate |
| Pesi più grandi (fino a 4,8 MB) | entro il rumore | | | scartato: il limite sono i dati |

Filtri del suggeritore (identici in Python e in C): direzione contraria alle parole dette; uno STATO senza verbo né
comparativo ("il volume è basso") è una lamentela e inverte la direzione, come "troppo"; un racconto al passato o
una negazione ("ho alzato il volume ieri", "non toccare la musica") non riceve mai una proposta.

Giudice LLM: `--judge-model qwen3.6:35b-a3b-mtp-q4_K_M --num-gpu 16` (35B a esperti, 3B attivi, metà strati sulla
GPU da 8 GB, il resto in RAM) è molto più severo del 9B ma lento (~1 h per 2.400 frasi). Per questo giro le frasi
sono state riviste a mano: scartate 408 su 1.373 (per esempio "go home" come guida verso casa, "resume" come
riprendere un discorso, "non vedo niente" etichettata come abbassare la luce).

## Perché non un encoder semantico per i comandi

`tools/anima_intent_heldout.txt` contiene frasi che nessuno strato ha mai visto: misura onesta, da non usare
mai per addestrare. Il test fallisce se un modello nuovo ne azzecca meno o ne sbaglia di più.

Gli encoder di frasi "statici" (quello già usato da L1, distillato da multilingual-e5-small, e anche
potion-multilingual-128M e static-similarity-mrl-multilingual) sono stati misurati sulle stesse frasi: danno
agli **opposti** ("apri / chiudi la musica" 0,83) una somiglianza più alta che alle vere parafrasi ("alza il
volume / aumenta l'audio" 0,31). Vanno bene per cercare conoscenza, non per scegliere un comando: per questo
il suggeritore è un classificatore addestrato, con conferma e filtro di polarità.

## Memoria: in PSRAM solo quello che è attivo

Niente di quanto sopra occupa memoria quando ANIMA non lavora:

| Cosa | Dove | Quando |
|---|---|---|
| Tabella delle parafrasi, pesi del suggeritore | flash (`const`), ~0,3 MB | sempre mappati, 0 byte di RAM |
| Frasi imparate (fino a 2048) | PSRAM heap, ~360 KB | al primo uso di ANIMA |
| Encoder L1 (3 MB int8) | PSRAM heap | al primo uso, solo se restano ≥ 8 MB liberi |
| Indice e shard di conoscenza (`nucleo_anima_l1.c`, `l1_fopen`) | PSRAM heap, fino a 24 MB, file fino a 12 MB | al primo uso, solo se restano ≥ 8 MB liberi; LRU |

Il router AKB5 sceglie gli shard che servono alla domanda, come un mixture-of-experts sceglie gli esperti:
in PSRAM restano quelli attivi. Il memory broker (`nv_mem_reclaimer_add("anima-l1-mirrors")`) libera tutte le
copie quando un'app pesante (fotocamera, video) chiede memoria; alla domanda successiva si ricaricano. La
PSRAM *statica* (`.ext_ram.bss`, sorvegliata da `tools/ci/check_budgets.py`) resta quasi intatta: tutto
quello che è grande è dinamico.

## Test

`make -C tests/host` (Linux/WSL, clang) esegue anche `anima_nl`:
- **frasi in linguaggio naturale:** italiano e inglese, con le regressioni trovate provando la scheda;
- **parafrasi:** ogni parafrasi deve dare lo stesso intento e argomento della sua canonica;
- **corpus del Cardputer:** quello curato è bloccante, comprese le frasi che *nominano* un'azione senza
  chiederla e che non devono mai eseguirla; quello grezzo è solo una metrica che non può scendere;
- **suggeritore:** parità C/Python sui casi golden, soglie sul set mai visto e flusso
  proposta → sì → esegue → impara.
