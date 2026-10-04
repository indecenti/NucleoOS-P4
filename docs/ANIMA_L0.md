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

## Contesto tra un turno e l'altro

Un frammento che continua il turno precedente viene **riscritto in una richiesta completa** prima di entrare
negli strati, come una parafrasi entra con la sua canonica (`nucleo_anima_query`, `anima_followup_math` in
`anima_solve.c`). Oggi vale per i calcoli:

| Prima | Poi | Riscritto | Risposta |
|---|---|---|---|
| "6x6" | "più 5?", "e meno 6", "+ 4" | "36 piu 5" | "Partendo da 36: fa 41." |
| "10 + 10" | "raddoppia", "il doppio?", "e la metà?" | "20 * 2" / "20 / 2" | |
| "12 per 12" | "la radice di quello", "al quadrato" | "la radice di 144" | |
| "6 times 6" | "plus 5?", "and times 2", "halve it" | "36 plus 5" | "Starting from 36: …" |

Lo stesso vale per gli altri ambiti (`anima_context.c`, `anima_ctx_rewrite`), riscrivendo la richiesta precedente:

| Prima | Poi | Riscritto |
|---|---|---|
| "che tempo fa a Roma?" | "e a Milano?", poi "e domani?" | "che tempo fa a milano", "che tempo fa a milano domani" |
| "che ore sono a Tokyo" | "e a New York?", "e Londra?", "what about Paris?" | "che ore sono a new york" |
| "converti 5 km in miglia" | "e 10?", poi "e in metri?" | "converti 10 km in miglia", "converti 10 km in metri" |
| "alza il volume" / "abbassa la luminosità" | "di più", "ancora di più", "a bit more" / "di meno" | stessa direzione / "abbassa …" |

La richiesta riscritta compare in `corrected` ("ho capito: …"). Ora nelle città e conversioni sono verificate
risolvendo la riscrittura ("e a Gotham?" non diventa una risposta); un luogo non è mai un pronome o "casa".

Regole, perché un calcolo non si mangi la richiesta successiva:
- il turno sostanziale precedente deve essere dello stesso ambito, al massimo 3 turni fa (un "grazie" in mezzo va bene,
  "apri le note" chiude l'argomento);
- il frammento è corto (≤ 8 parole) e fatto **solo** di numeri, operatori e parole di un vocabolario chiuso
  (operatori, verbi come "togli", "raddoppia", anafore come "quello", congiunzioni come "e", "ora"): "per favore
  apri le note" resta una richiesta di aprire le note;
- deve riferirsi davvero al risultato ("quanto fa 2 per 3" sta in piedi da solo) e la riscrittura deve essere
  calcolabile; altrimenti la frase entra com'è;
- la risposta dice da quale numero è partita, così un contesto sbagliato si vede subito.

### A cosa si riferisce un frammento: una regola sola

Due tipi di memoria, con due regole (`ctx_fresh`, `topic_set` in `nucleo_anima.c`):

| Memoria | Esempi | Vale finché |
|---|---|---|
| **conversazione** | "più 5?", "e a Milano?", "e Newton?", "dimmi di più", "spiegati meglio" | è l'**ultimo turno sostanziale**, al massimo 3 turni fa |
| **dispositivo** | "chiudila" (l'app aperta), "aprilo" (l'ultimo file), "ripeti" (l'ultima azione) | il dispositivo non la cambia |

Gli atti di dialogo ("grazie", "sei sicuro?") non spostano l'ultimo turno, quindi non rompono il filo; ogni altra
richiesta sì. Prima ognuno aveva la sua finestra: il focus di "e Newton?" durava 8 turni qualunque cosa si dicesse
nel mezzo, e "dimmi di più" valeva per sempre sull'ultimo argomento (anche dopo un riavvio). Ora:
- "dimmi di più" senza un argomento fresco chiede "Di più su cosa?" invece di ripescarne uno vecchio;
- solo una richiesta "nuda" continua l'argomento: "dimmi di più su Einstein" e "voglio più volume" hanno il loro.

Test: sezione "CONTEXT" di `tests/host/unit/test_anima_nl.cpp`, compresi i casi che **non** devono essere letti
come seguito.

## Spagnolo, francese, tedesco

Il motore ragiona in italiano e inglese. Una richiesta in spagnolo, francese o tedesco viene **letta attraverso
l'inglese** e la risposta torna nella lingua dell'utente (`anima_lang.c`, l'involucro `nucleo_anima_query`):

```
"¿Cuánto es 6 por 7?"  -> piega i caratteri -> tabella frasi / glossario -> "what is 6 times 7" -> motore
motore: "It's 42."     -> tabella risposte                                -> "Da 42."
```

1. **Piegatura**: minuscole, á ñ ü ö ä ß ç œ in ASCII, ¿ ¡ e apostrofi in spazi (`anima_lang_fold`, uguale a
   `xfold()` di `tools/gen_anima_phrases.py`).
2. **Frase intera**: la tabella delle parafrasi ha chiavi `es:` `fr:` `de:` con un canonico **inglese**. Fonti:
   `tools/anima_phrases_xl.txt` (a mano) e le frasi di test di Home Assistant (`tools/import_ha_intents.py`, CC BY 4.0).
3. **Glossario** (`GL_ES`, `GL_FR`, `GL_DE`): verbi, app, impostazioni, tempo, matematica, dizionario; vince la
   corrispondenza più lunga ("pon el volumen al" prima di "pon"). Le parole che non conosce passano invariate:
   nomi, numeri, il testo di una nota. "a/à/um" davanti a un numero diventano "at" (l'ora di un promemoria).
4. **Sì da solo**: "sí", "oui", "ja", "vale", "d'accord", "klar"... valgono "yes" solo se sono tutta la frase.
5. **Risposte**: una tabella di modelli inglesi con segnaposto (`RP`), poi date e nomi di giorni e mesi
   ("Saturday, October 3, 2026" -> "sábado, 3 de octubre de 2026" / "samedi 3 octobre 2026" / "Samstag, 3. Oktober 2026").
   I `{value}` che l'OS riempie dopo restano al loro posto. Una risposta che la tabella non conosce resta in
   inglese: è quello che questi utenti ricevevano prima, mai una traduzione inventata.

| Esempio | Risposta |
|---|---|
| "sube el volumen" / "monte le son" / "mach lauter" | Subo el volumen. / J'augmente le volume. / Ich mache lauter. |
| "abre la calculadora" / "ouvre la calculatrice" / "öffne den Taschenrechner" | Abro / J'ouvre / Ich öffne + il nome dell'app |
| "¿Cuánto es 6 por 7?" / "combien font 6 fois 7" / "wie viel ist 6 mal 7" | Da 42. / Ça fait 42. / Das ergibt 42. |
| "pon un temporizador de 5 minutos" | Temporizador de 5 min en marcha: sonará a las 21:51. |
| "¿qué hora es en Tokio?" | En Tokyo son las 04:46. |
| "6x6" poi "más 5" | Partiendo de 36: Da 41. (il contesto vale anche qui) |
| "traduce perro al inglés", "traduce dog al español", "traduci cane in spagnolo" | dog, hound / perro / perro (via inglese) |

**Con un modello.** Il primo passaggio è solo sul dispositivo (`s_xl_device_only`): "sube el volumen" non aspetta
la rete. Se il dispositivo non capisce e c'è un modello utilizzabile, il modello riceve la frase **originale**.

**Dove passa la lingua.** L'app ANIMA e Telegram usano `nv_anima_lang()` (la lingua di sistema); il web passa
`lang`. Le voci offline parlano italiano e inglese: una risposta in spagnolo, francese o tedesco resta a schermo.

**Dizionari** (vedi sotto): `dict-{es,fr,de}-en.tsv`, `dict-en-{es,fr,de}.tsv` e `forms-{es,fr,de}.tsv` da
Wiktionary; italiano <-> spagnolo/francese/tedesco passa dall'inglese.

**Limiti**: le definizioni monolingui esistono in italiano e inglese; in spagnolo, francese e tedesco "cosa
significa X" dà la traduzione inglese. Le risposte di un modello seguono la lingua del suo prompt. I valori che
l'OS scrive in `{value}` (rete, memoria, capacità) sono in inglese.

## Dizionari offline: definizioni, sinonimi, contrari, traduzioni

Uno strumento di L0 (`nucleo_anima_lex.c`) risponde da dizionari sulla SD, senza rete e senza modello. Come il
traduttore, è fondato per costruzione: una parola c'è nel file o non c'è, e la risposta dice da dove viene.

| Richiesta | Esempio | Risposta |
|---|---|---|
| definizione | "cosa significa effimero", "che vuol dire procrastinare?", "definizione di serendipità", "what does ephemeral mean" | fino a 3 sensi con la parte del discorso, poi 4 sinonimi |
| sinonimi / contrari | "sinonimi di veloce", "il contrario di felice", "synonyms of happy", "opposite of cold" | fino a 6 parole |
| forme flesse | "cosa significa andavamo", "traduci correvano in inglese", "translate went to italian" | passa dal lemma e lo dice: «andavamo» è una forma di «andare» |
| parola dell'altra lingua | "cosa significa ephemeral", "what does gatto mean" | la definizione, con la lingua e la traduzione |
| senza definizione | "cosa significa casa" | lo dice, e dà la traduzione |
| "cos'è X" che nessuno strato conosce | "cos'è un ornitorinco" | la definizione, prima del "non lo so" |

Non è una richiesta di dizionario: "sono contrario alla guerra" (manca "di"), "che significa questo?" (un
pronome), "cosa significa 404", "cosa vuoi dire". Una definizione assente lascia passare la domanda a L1 e al
modello; sinonimi e contrari assenti danno un "non è nel dizionario" onesto. "Sinonimi di pioggia" non è il meteo.

### File sulla SD (`/data/anima/`, generati, fuori da git)

| File | Contenuto | Fonte | Licenza | Voci |
|---|---|---|---|---|
| `lex-it.tsv` | sensi, sinonimi, contrari, grafia | Wikizionario | CC BY-SA 4.0 | 67.000 |
| `lex-en.tsv` | sensi, sinonimi, contrari, grafia | Open English WordNet 2025 | CC BY 4.0 | 126.000 |
| `forms-it.tsv` | forma flessa -> lemma | Wiktionary (voci italiane) | CC BY-SA 4.0 | 526.000 |
| `forms-en.tsv` | forma flessa -> lemma | regole + verbi irregolari sui lemmi di WordNet | CC BY 4.0 | 80.000 |
| `dict-{es,fr,de}-en.tsv`, `dict-en-{es,fr,de}.tsv`, `forms-{es,fr,de}.tsv` | traduzioni e forme | Wiktionary (voci spagnole, francesi, tedesche) | CC BY-SA 4.0 | vedi `DICTIONARIES.txt` |
| `dict-it-en.tsv` | traduzioni IT -> EN | Wiktionary + Wikizionario + FreeDict/WikDict | CC BY-SA 4.0 | 122.000 |
| `dict-en-it.tsv` | traduzioni EN -> IT | FreeDict/WikDict + Wiktionary invertito | CC BY-SA 4.0 | 96.000 |
| `DICTIONARIES.txt` | fonti, versioni, licenze, conteggi | | | |

Circa 50 MB in tutto. Ogni file è `chiave<TAB>valore` ordinato per byte: il firmware lo cerca per bisezione
direttamente sulla SD (`anima_dict_get`, una ventina di letture, un buffer da 2 KB sullo heap, niente di residente).
Le chiavi sono normalizzate come il tokenizzatore del firmware (`anima_dict_tokenize`, condiviso da tutti i
dizionari), e il generatore le scrive allo stesso modo.

```bash
python tools/dicts/gen_dicts.py fetch    # scarica le fonti in tools/dicts/.cache (~135 MB)
python tools/dicts/gen_dicts.py build    # scrive sd/data/anima/*.tsv (~35 s)
python tools/dicts/gen_dicts.py check    # cerca parole come fa il firmware e verifica l'ordinamento
```

Poi `tools/sync-sd.ps1 -Drive X:` copia i file sulla card.

Qualità, scelte del generatore:
- le traduzioni EN -> IT mettono prima le parole su cui **due fonti concordano** (FreeDict le elenca e sono la
  prima glossa di Wiktionary): "cat" -> gatto, non "caponare";
- in ogni voce la parte del discorso con più sensi viene prima ("andare" il verbo, "fast" l'aggettivo);
- sensi arcaici, rari o specialistici vanno in fondo; i residui di modello di Wikizionario sono scartati. Qualche
  voce comune ("casa") non ha una definizione estraibile: in quel caso risponde con la traduzione.

I test sul PC usano dizionari in miniatura scritti dal test (`write_lexicon_fixture` in
`tests/host/unit/test_anima_nl.cpp`): stesso formato, stessa ricerca.

## Strumenti (sul PC)

| Comando | Cosa fa |
|---|---|
| `python tools/gen_anima_phrases.py [--check]` | rigenera tabella e test dalle parafrasi (`--check`: fallisce se non aggiornati, come in CI) |
| `python tools/import_ha_intents.py <checkout home-assistant/intents>` | reimporta le frasi di Home Assistant |
| `python tools/import_nucleo_evals.py <checkout NucleoOs>` | reimporta i corpus di azioni dell'ANIMA del Cardputer come test |
| `python tools/augment_anima_intent.py [--verify]` | **GPU**: un LLM locale (Ollama, default `qwen3.5:9b`) genera parafrasi e negativi "difficili"; `--verify` fa da giudice e tiene solo quelle giuste (scarti in `.rejected`) |
| `python tools/train_anima_intent.py [--eval-only]` | addestra il suggeritore, lo valuta sul set mai visto e scrive i pesi C + i casi golden |
| `python tools/dicts/gen_dicts.py fetch\|build\|check` | scarica le fonti, genera e verifica i dizionari offline (vedi sopra) |
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
