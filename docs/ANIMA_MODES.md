# ANIMA: modalità d'uso e scala di ripiego

Questo documento definisce il contratto tra le modalità di ANIMA e quello che ogni turno usa davvero.
Il codice di riferimento è `nucleo_anima_route()` / `nucleo_anima_query()` in
`components/nv_anima/nucleo_anima.c`; i test sono la sezione "FALLBACK LADDER" di
`tests/host/unit/test_anima.cpp`.

## Principio

La modalità scelta dall'utente è un **desiderio**: ogni turno lavora con quello che c'è in quel momento.
ANIMA non smette mai di funzionare. Se manca un pezzo scende di un gradino invece di fermarsi:

1. **Modello linguistico** (cloud o LAN): conversazione, agente, codice.
2. **Web**: Wikipedia, Wikidata, meteo, notizie, cambi, festività. Serve internet, non un modello.
3. **Dispositivo**: L0 (comandi e strumenti), risolutore matematico, L1 (conoscenza su SD), HDC.
   Funziona sempre, anche senza rete.

## Le tre risorse, controllate a ogni turno

| Risorsa | Vera quando |
|---|---|
| `network` | il dispositivo ha un IP e la modalità non è *offline* |
| `web` | `network`, e la modalità non è *locale* (in locale nulla esce dalla LAN) |
| `model` | c'è un modello configurato per la modalità (in *locale* solo server LAN), il dispositivo è online, e il primo endpoint non è in pausa dopo un errore |

Le pause dopo un errore (circuit breaker in `nucleo_anima_online.c`) sono queste:
- chiave o modello sbagliati (400/401/403/404): 10 minuti;
- quota esaurita (429): 1 minuto;
- server irraggiungibile o 5xx: 15 secondi.

Modificare `teacher.json` azzera le pause.

## Le modalità (`anima.net`)

| Modalità | Con modello utilizzabile | Senza modello (assente, in pausa, o non risponde) | Senza rete |
|---|---|---|---|
| **offline** | — | dispositivo | dispositivo |
| **locale** | dispositivo + modello LAN come ultima risorsa | dispositivo | dispositivo |
| **ibrida** (predefinita) | dispositivo + web + modello come ultima risorsa | dispositivo + web | dispositivo |
| **llm (agente)** | il modello guida il turno, **tranne ciò che il dispositivo fa con esattezza** | dispositivo + web, **comandi inclusi** | dispositivo, comandi inclusi |

`nucleo_anima_route()` restituisce il gradino del prossimo turno (`anima_run_t`):

| `run` | Significato |
|---|---|
| `ANIMA_RUN_DEVICE` | solo dispositivo |
| `ANIMA_RUN_WEB` | dispositivo + web, nessun modello |
| `ANIMA_RUN_LOCAL_LLM` | dispositivo + modello LAN come ultima risorsa |
| `ANIMA_RUN_HYBRID` | dispositivo + web + modello come ultima risorsa |
| `ANIMA_RUN_AGENT` | il modello guida |

`degraded` è vero quando la modalità è *llm* ma il modello non è utilizzabile.

## Prima il dispositivo, anche in modalità agente

Ciò che il dispositivo sa fare con esattezza non passa mai al modello, nemmeno quando il modello c'è
(`l0_exact` e `facts_answer`, prima del blocco agente in `nucleo_anima.c`):
- i calcoli e i loro seguiti ("12+34", poi "per 4" fa 184; il modello locale aveva risposto 11.5);
- ora, data, stato del sistema, "cosa sai fare", "cos'è NucleoOS";
- aprire e chiudere le app, timer, sveglie, volume, luminosità;
- i fatti Wikidata dei pacchetti ("quando è nato X").

Sono risposte istantanee, sempre giuste e sempre uguali. Al modello restano la conversazione e le azioni
da interpretare: "scrivi uno e due in t.txt" (cosa scrivere lo capisce lui) e le domande con un'immagine
allegata (solo lui la vede). Senza modello la risposta del dispositivo porta comunque `(offline)` o
`(senza modello)`.

## Regole del ripiego

1. **Non si aspetta un modello che non c'è.** Se non è configurato, se il dispositivo è offline o se è in pausa,
   il turno parte subito dai gradini del dispositivo, senza tentare la connessione.
2. **Un solo tentativo per turno.** Se il modello fallisce durante il turno, il resto del turno lo vede come
   assente (`nucleo_anima_online_model_off`). Traduzione, verifica Wikipedia e insegnante non lo richiamano.
3. **I comandi funzionano anche in modalità agente.** Se il modello non risponde, "apri la musica" o
   "alza il volume" vengono eseguiti da L0: l'utente li ha chiesti, e dal lato del modello non è stato eseguito niente.
4. **La risposta dice da dove viene.** In modalità *llm* una risposta testuale data dal dispositivo è
   prefissata con `(senza modello)` se la rete c'è, oppure con `(offline)` se manca. I comandi restano puliti.
   Il campo `degraded` del risultato (e `"degraded"` nel JSON web) permette all'interfaccia di mostrarlo.
5. **Un "non lo so" spiega il motivo.** Riporta il problema del modello (chiave non valida, quota, server
   irraggiungibile), oppure "nessun modello linguistico disponibile".
6. **Stesse regole su ogni superficie:**
   - app nativa, Telegram, `/api/anima` e `/api/anima/query` passano da `nucleo_anima_query`;
   - la chat conversazionale web (`/api/anima/chat`, `nucleo_anima_conv_chat`), se il modello manca o non
     risponde, passa da `nucleo_anima_query_no_model`: il dispositivo risponde, l'eventuale comando viene
     eseguito, e il turno resta nella conversazione.

## Forma del turno: chi possiede una richiesta

Prima che qualunque livello risponda, `turn_gate()` (in `nucleo_anima.c`) guarda la **forma** di una richiesta
che chiede di *fare* qualcosa. Una parola-comando dentro una frase non basta più a farla eseguire.
Il principio è la certezza: un'azione sbagliata o fatta a metà è peggio di nessuna azione.

| Forma | Esempio | Cosa succede |
|---|---|---|
| composta | "chiudi la musica e apri le note" | se L0 capisce **ogni** parte → un solo **piano** eseguito in ordine; altrimenti non si esegue niente |
| differita | "apri la musica tra 10 minuti", "…alle 18", "…domani" | mai eseguita subito |
| condizionale | "se domani piove ricordami…" | mai eseguita subito |
| ricorrente | "ogni mattina alle 8 dimmi il meteo" | mai eseguita subito |
| come si fa | "come si alza il volume?" | spiegata e offerta: un "sì" la esegue, un "no" la annulla |
| negata | "non aprire la musica" | "Va bene, non faccio niente." |
| lamentela | "il volume è troppo alto", "lo schermo è troppo luminoso" | un passo di correzione (−10 / +10) |

**Cosa L0 non sa fare con certezza va al modello.** Vale per i composti che L0 non sa pianificare e per le
richieste differite, condizionali e ricorrenti. In modalità *ibrida* e *locale* il modello si usa solo se è
utilizzabile, come ultima risorsa, e risponde con righe `ACT` che vengono validate. Se il modello manca, la
risposta spiega onestamente cosa funziona oggi (per esempio un promemoria). Se L0 sa pianificare un composto,
il modello non viene interpellato.

Non vengono mai spezzati:
- il testo di una nota o di un promemoria ("crea una nota con scritto apri la porta e chiudi la finestra");
- un elenco dentro un promemoria ("ricordami di comprare il pane e il latte");
- le domande ("chi erano Romolo e Remo").

Una forma di cortesia ("se puoi apri la musica") è una richiesta semplice. Un pronome riferito a un'altra
parte della frase ("alza il volume e poi abbassalo", "chiudi la musica e riaprila") è ambiguo dentro un
piano: non si esegue niente e la risposta chiede di dirlo per esteso.

### Piani

Un piano è una lista validata di azioni sul dispositivo (`anima_result_t.steps`, al massimo `ANIMA_PLAN_MAX` = 3):
- al massimo **un'app** da aprire (il sistema ne tiene aperta una alla volta). Due richieste per la stessa
  app ("apri la galleria e mostrami le foto") valgono come una;
- al massimo **una** nota o un promemoria (condividono il contenuto composto);
- niente contraddizioni (aprire e chiudere la stessa app);
- solo azioni sul dispositivo: app, volume, luminosità, chiusura della musica, note, promemoria.

Lo stesso validatore vale per L0 e per il modello. Un modello che risponde con più righe `ACT` produce lo
stesso piano. I permessi (`permissions.json`) prendono il passo più restrittivo, e un solo "procedo?" copre
tutto il piano. Un piano non valido proposto dal modello non viene eseguito.

Esecuzione: `nv_anima_os_run` esegue l'azione principale (se è uno strumento) e poi ogni passo, nell'ordine
chiesto. L'app del piano viene aperta per ultima dalla superficie, come ogni altra apertura. La risposta unisce
le parti ("Alzo il volume, apro Note e abbasso la luminosità."), e le superfici mostrano il nome localizzato
di ogni app citata (`nv_anima_pretty_reply`). Il JSON web aggiunge `"steps":[{"intent","arg"}…]`.

## Dove si vede

- `GET /api/anima/net` restituisce `mode`, `run`, `label`, `label_en`, `network`, `web`, `model` e `degraded`.
- App ANIMA nativa: `/mode` mostra la riga "adesso: …" con il gradino effettivo.

## Cosa NON cambia

- In modalità *ibrida* il modello resta l'ultima risorsa, solo per domande (`a_is_askable`): comandi e dati
  non arrivano mai a un generatore.
- Le conferme in sospeso, le immagini allegate e i chiarimenti vengono risolti prima di scegliere il gradino.
