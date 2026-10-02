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
| **llm (agente)** | il modello guida il turno | dispositivo + web, **comandi inclusi** | dispositivo, comandi inclusi |

`nucleo_anima_route()` restituisce il gradino del prossimo turno (`anima_run_t`):

| `run` | Significato |
|---|---|
| `ANIMA_RUN_DEVICE` | solo dispositivo |
| `ANIMA_RUN_WEB` | dispositivo + web, nessun modello |
| `ANIMA_RUN_LOCAL_LLM` | dispositivo + modello LAN come ultima risorsa |
| `ANIMA_RUN_HYBRID` | dispositivo + web + modello come ultima risorsa |
| `ANIMA_RUN_AGENT` | il modello guida |

`degraded` è vero quando la modalità è *llm* ma il modello non è utilizzabile.

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

## Dove si vede

- `GET /api/anima/net` restituisce `mode`, `run`, `label`, `label_en`, `network`, `web`, `model` e `degraded`.
- App ANIMA nativa: `/mode` mostra la riga "adesso: …" con il gradino effettivo.

## Cosa NON cambia

- In modalità *ibrida* il modello resta l'ultima risorsa, solo per domande (`a_is_askable`): comandi e dati
  non arrivano mai a un generatore.
- Le conferme in sospeso, le immagini allegate e i chiarimenti vengono risolti prima di scegliere il gradino.
