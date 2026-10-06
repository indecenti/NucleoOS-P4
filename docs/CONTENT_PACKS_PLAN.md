# Contenuti di sistema scaricabili — piano

Stato (2026-10-06): **F0-F5 fatti**. Contenuti pubblicati (store `content/`, release `content-2026.10.1`); firmware 1.2.66 provato sulla scheda.

- F0: `teacher.json` in chiaro tolto dal mirror `sd/` (spostato in `%USERPROFILE%\.nucleo\anima-teacher.json`);
  `sync-sd.ps1` / `verify-sd.ps1` non toccano mai i file dell'utente; `tools/content/build.py` costruisce i
  pacchetti da allowlist e si ferma su qualunque credenziale (anche il formato Gemini `AQ.`), senza mai
  stamparla; licenze dentro ogni pacchetto. Test: `python tools/content/test_build.py`.
- F1: formato `nucleoos-data-v2` (`nv_store_pkg`), indice/ustar/journal (`nv_store_tree`), installazione
  con estrazione verificata file per file e scambio atomico, recupero all'avvio (`app_main`), riserva OTA
  64 MB anche per i `wiki-*`, chiave di riserva dello store (`store_signing_backup_pub.pem`, privata da
  spostare offline), mirror `url2`. I 5 difetti dello store corretti. Test: `tests/host` unit `content`
  (190 controlli, 64/32 bit, incluso un archivio fatto da Python letto dal codice del dispositivo) +
  fuzzer `content`; build firmware e budget di memoria OK.
- F2: servizio `nv_content` (coda persistente, attesa di SD / safe mode / conferma OTA (`nv_ota_confirmed`)
  / OTA in corso / store / rete, backoff 1-5-15-60 min, aggiornamento automatico SOLO dei pacchetti
  installati più vecchi di `kRequired`, mai sopra un web modificato con `/api/web/put`), indice
  `content/index-v1.json` (nomi in 5 lingue, scritto da `build.py`), verifica e riparazione con la firma
  tenuta sulla SD, adozione di alberi già presenti senza scaricarli, pulizia dei `.part` vecchi di 24 h,
  ANIMA rilascia e riapre i file attorno allo scambio, la cache web si ricostruisce dopo un aggiornamento,
  `GET/POST /api/content`, comando shell `content`, G6 in `tools/dist.py` (rifiuta un firmware che richiede
  una versione non pubblicata). Logica decisionale testata sul PC (`nv_content_plan`).
- F3: passo "Contenuti offline" nel wizard (dopo il Wi-Fi, un tocco, "Personalizza"), Impostazioni >
  Contenuti di sistema (scheda di stato + ogni pacchetto con la sua azione), nota quando manca qualcosa,
  pagina di riserva al posto del 404 del web, lo Store non mostra due volte i `dict-*`, 32 stringhe in 5 lingue.
- F4: `tools/dist.py content` (asset nella release + solo `content/` dello store), `build.py --reuse` (i file
  già pubblicati dai pacchetti v1 `dict-*` non si ricaricano: stessi sha, v1 e v2 si adottano a vicenda),
  `--only` (un pacchetto senza cambiare versione agli altri), `--index-only` (solo i testi dell'indice).
  Pubblicati: 7 pacchetti 2026.10.1, `dict-it` 2026.10.2 (store `7220003`, `959ee81`.., `244553c`).
- F5, verificato sulla scheda (build di prova 1.2.62-1.2.66, installate da SD):
  - G4: durante la probation il servizio aspetta (`waiting=ota`), carica l'elenco subito dopo la conferma.
  - set consigliato (web, ANIMA, dizionari it/en, 139 MB) installato in 6 min 45 s; adozione dei file già
    presenti (`dict-fr` in 24 s, `akb5` "already matches its index, kept").
  - G9: journal "swap" simulato prima del riavvio -> completato al boot (`0x4`), journal cancellato.
  - verifica: un file del web modificato e uno shard AKB5 danneggiato trovati; riparazione con scambio vero
    delle cartelle, ANIMA rilascia i file e ricarica ("offline data reloaded"), cache web ricostruita (305 file).
  - G12: `/api/web/put` accende `web_local`; un'installazione esplicita lo spegne.
  - ripresa dopo un riavvio a metà download ("resumes at 3145728/8579382 bytes").
  - wizard: lingua per prima, passo Contenuti in italiano e spagnolo, raccomandazioni per lingua, primo
    focus da tastiera su "Scarica", Invio avvia il download; cambio lingua a caldo dei nomi dei pacchetti.
  - corretti durante i test: estrazione lenta (buffer SD non allineati: 83 MB da ~450 s a <=170 s), cache
    web vuota dopo l'aggiornamento, ripresa persa dopo un riavvio (fsync), nomi nella lingua sbagliata,
    plurali, accenti dell'indice.
  - NON provati sulla scheda: pagina di riserva del web (richiede di togliere `web/`), nota "mancano
    contenuti" (richiede pacchetti consigliati mancanti), SD piena / riserva OTA, seme `skills` con un file
    modificato dall'utente, vista "Personalizza". Coperti da codice e test sul PC.
- Escluso per ora: `drivers-win` (driver Windows di terzi, diritti di ridistribuzione da chiarire).

Decisioni dell'utente (2026-10-06):
- Voce TTS **rimandata**: nessun pacchetto `voice-*` nella prima release (gli id restano riservati).
- AKB5 ed encoder ANIMA **pubblicabili** su GitHub pubblico, con `LICENSES.txt`.
- Wizard **a un tocco**: set consigliato preselezionato ("Scarica · ~140 MB") + "Personalizza".
- Dopo un OTA si aggiornano **da soli solo i pacchetti richiesti** dalla versione del firmware (es. `sys-web`);
  gli altri aggiornamenti vengono segnalati in Impostazioni > Contenuti.

## Il problema

Chi flasha NucleoOS dal web flasher ottiene una scheda a metà e non lo sa:

| Contenuto SD | Peso | Oggi come arriva | Senza |
|---|---|---|---|
| `web/` (web companion) | 8 MB (3.7 MB zip) | zip nella release, da scompattare a mano dal PC | `/` risponde 404 |
| `data/anima` L1: encoder + AKB5 (62 shard) + facets | ~87 MB | **mai** (solo da questo PC, gitignored) | ANIMA senza livello semantico |
| `data/anima` dizionari it/en/es/fr/de | ~100 MB | **mai** | "parola non nel dizionario offline", niente traduzioni |
| `data/tts/<lang>` | 3 MB a lingua | **mai** | nessuna voce, solo una riga di log |
| `nucleos/drivers/windows/*.exe` | 2.3 MB | **mai** (exe gitignored) | Second Screen USB senza driver |
| `data/anima/kb/*.akb6` (Wikipedia) | 23-50 MB a lingua | Store, ma solo se l'utente lo sa | ANIMA senza fatti enciclopedici |

Sul dispositivo non c'è nessun controllo e nessun messaggio: le funzioni spariscono in silenzio.

## Decisioni

### Dove stanno i file

- **Sorgenti** (web, skills, script di build, licenze): nella repo `NucleoOS-P4`, come oggi. I binari grandi
  (AKB5, dizionari, voci) **non entrano in git**, ma il modo per ricostruirli sì (`tools/content/`).
- **Distribuzione**: nello **store** (`nucleoos-p4-store`), con lo stesso meccanismo già usato e verificato
  sui pacchetti Wikipedia:
  - `data/<id>/pack.sig` su Pages: elenco dei file con sha256, dimensione e URL, firmato con la chiave dello store.
  - payload come **asset di una release GitHub** (`content-2026.10`, …): fino a 2 GB per file, CDN,
    gratis, nessun limite di Pages (1 GB per sito, 100 MB per file).
- Niente server Oracle: ha poca RAM e qui non serve.

Perché lo store e non la release del firmware: la firma, il download con ripresa, il controllo sha e il
controllo dello spazio esistono già lì (`nv_appstore.cpp` `install_data`). Inoltre i contenuti hanno una
vita propria: un dizionario si aggiorna senza un nuovo firmware.

### Garanzie di aggiornabilità (vincoli non negoziabili)

Il principio: **i contenuti non devono mai poter impedire un aggiornamento del firmware**, e nessun
dispositivo esistente deve accorgersi che esistono finché non ha il firmware che li conosce.

| # | Rischio trovato nell'analisi | Vincolo |
|---|---|---|
| G1 | I firmware già sul campo leggono `store2-<lang>.json`; una riga con `kind` sconosciuto viene trattata come **app** (`nv_appstore.cpp:717`) e l'installazione fallisce. Le destinazioni nuove (`web`, `tts`) vengono rifiutate dal loro parser. | I pacchetti di sistema **non entrano** nei cataloghi `store*.json`. Indice separato `content/index-v1.json`, letto solo dal firmware nuovo. I `wiki-*` restano dove sono e nel formato v1. |
| G2 | Un formato nuovo di `pack.sig` (righe `tar`) romperebbe i parser vecchi. | Dominio nuovo `nucleoos-data-v2`, usato solo dai pacchetti dell'indice nuovo. Il v1 resta invariato e supportato per sempre. |
| G3 | **L'OTA scarica l'immagine sulla SD** e richiede immagine + metà slot + 4 MB liberi (`nv_ota.cpp:478`, circa 20 MB). Riempire la SD di contenuti bloccherebbe gli aggiornamenti. | Ogni installazione lascia libera una **riserva OTA** (64 MB, calcolata dalla stessa formula moltiplicata per 2). Se lo spazio manca, il contenuto non si installa, mai il contrario. Nessun file `.part` lasciato a occupare spazio oltre 24 h senza progressi. |
| G4 | Durante la **probation** il firmware nuovo deve dimostrare di raggiungere il server degli update ("net"); un download pesante in parallelo può farla fallire e causare un rollback inutile. | `nv_content` parte solo a immagine confermata (`s_confirmed`, nuova API `nv_ota_confirmed()`), mai in safe mode, mai mentre `nv_ota_busy()`. |
| G5 | Due connessioni TLS in parallelo hanno già causato fame di SRAM e crash di esp_hosted. | Un solo lavoro di rete pesante alla volta: i contenuti usano il worker dello store (già seriale) e cedono il passo all'OTA. |
| G6 | Un firmware che richiede `sys-web >= X` pubblicato **prima** di X manderebbe ogni dispositivo in un ciclo di tentativi. | `dist.py firmware` si rifiuta di pubblicare se i pacchetti richiesti non sono già sullo store con la firma valida. Se comunque mancano: il dispositivo funziona, mostra la pagina di riserva e riprova con backoff. |
| G7 | Rollback a un firmware più vecchio (LKG) con un web più nuovo sulla SD. | Il percorso di aggiornamento è **solo nativo** (Impostazioni + recovery) e non dipende mai da file sulla SD. Il web dichiara le API che usa e degrada se `/api/info` è più vecchio; mai un downgrade automatico dei contenuti. |
| G8 | Un file di contenuto malformato letto all'avvio potrebbe mandare in crash ogni avvio. | Solo file con sha verificato vengono attivati; i consumatori (ANIMA L1, web cache) controllano già gli header; in safe mode non si leggono. Il caso viene provato apposta nei test HIL. |
| G9 | Interruzione di corrente durante lo scambio di cartella (`web` → `web.old`). | Journal `/sdcard/nucleos/content.swap` scritto prima dello scambio; all'avvio si completa o si annulla. Mai una cartella mancante. |
| G10 | ANIMA e la cache web tengono aperti i file mentre vengono sostituiti. | Hook prima e dopo lo scambio: ANIMA chiude e riapre L1 e i dizionari (come fa già per i `wiki-*`), nv_web ricostruisce la cache PSRAM. |
| G11 | Schede preparate a mano (la tua, i tester): 140 MB scaricati di nuovo per niente, o file sovrascritti. | **Adozione**: file presenti con sha giusto → si scrive solo il record `.pack`. Sha diverso → "versione diversa", mai sovrascritto in automatico. |
| G12 | Sulla scheda di sviluppo il web si aggiorna con `/api/web/put`: l'aggiornamento automatico cancellerebbe il lavoro in corso. | `/api/web/put` segna il web come "modificato localmente": l'aggiornamento automatico si mette in pausa e Impostazioni lo mostra. |
| G13 | Le `skills/` sono modificabili dall'utente. | Distribuite come file "seme": copiate solo se assenti, mai sovrascritte. |
| G14 | Dipendenza da un'unica chiave dello store e da un unico host. | Il firmware accetta la chiave dello store **e** una **chiave di riserva** (deciso 2026-10-06: la privata va tenuta offline dall'utente). `pack.sig` v2 ammette fino a 2 URL per file; per ora solo GitHub, il mirror si aggiunge senza OTA. |

### Come si scarica

Tre ingressi, un solo motore (`nv_content`):

1. **Wizard**: un passo "Contenuti" dopo il Wi-Fi.
2. **Impostazioni > Contenuti**: per chi ha saltato il passo, per chi cambia lingua e per riparare.
3. **Al momento del bisogno**: la funzione che non trova i suoi file lo dice e offre il download
   ("Voce italiana non installata · Scarica 3 MB").

Il download **continua in background**: l'utente finisce il wizard e usa la scheda. L'avanzamento sta
nella tendina e nell'icona di stato. Riprende da solo dopo un riavvio o una caduta del Wi-Fi.

### "Lo proponiamo solo se lo store risponde?"

Il passo compare sempre se il Wi-Fi è collegato, ma il contenuto cambia con lo stato:

| Stato | Cosa vede l'utente |
|---|---|
| Store raggiungibile, SD ok | pacchetti consigliati già spuntati, peso totale, spazio libero, **Scarica (consigliato)** / Più tardi |
| Store non raggiungibile | "Il server dei contenuti non risponde ora. Li scarico da solo appena torna raggiungibile." + Riprova / Continua |
| Nessuna SD o SD piena | "Inserisci una microSD (FAT32, almeno 1 GB liberi)" con il controllo in tempo reale |
| Wi-Fi saltato | il passo non compare; promemoria in Impostazioni e notifica alla prima connessione |

Mai una lista vuota o un errore tecnico. Se il download non parte nel wizard, il servizio riprova da solo
e manda **una** notifica quando ha finito (o se ha bisogno di qualcosa dall'utente, ad esempio spazio).

## Pacchetti

Pacchetti piccoli e per lingua, così si scarica solo ciò che serve. I nomi qui sotto sono provvisori.

| id | Contenuto | Peso | Consigliato |
|---|---|---|---|
| `sys-web` | web companion | ~4 MB | sempre |
| `anima-core-it` | encoder, AKB5 + shard, facets it/en, `commands.it.json` | ~87 MB | sempre (ANIMA usa l'encoder italiano per tutte le lingue) |
| `dict-it` | lex-it, forms-it, dict-it-en, dict-en-it | ~31 MB | se lingua it |
| `dict-en` | lex-en, forms-en | ~18 MB | sempre (inglese = lingua ponte) |
| `dict-es` / `dict-fr` / `dict-de` | dict-xx-en, dict-en-xx, forms-xx | 12-26 MB | se quella lingua |
| `voice-it` / `voice-en` | `tts/<lang>/{index.bin,clips.pcm}` | ~3 MB | **rimandato** |
| `drivers-win` | driver Windows Second Screen | ~2 MB | no (solo da Impostazioni o dalla pagina Second Screen) |
| `wiki-<lang>-top` | Wikipedia (esiste già) | 23-50 MB | proposto, non spuntato |

Totale consigliato per un utente italiano: circa 140 MB.

**Mai** nei pacchetti, con una allowlist invece di una denylist: `teacher.json` (contiene una vera chiave
API), `workspace.json`, `learned/` scritti dal dispositivo, `session.txt`, `conv/`, `memory.jsonl`, profili,
regole, timer. Non vanno nemmeno i file inutili che il firmware non apre: `tts/*_final`, `tts/*_merged`,
`anima-it-encoder.json`, `anima-it-index.bin` (fallback sostituito da AKB5).

## Lavoro

### F0 — Pulizia e sicurezza (prima di tutto)

- Togliere il `teacher.json` in chiaro dal mirror `sd/`: `sync-sd.ps1` oggi lo copia sopra la copia
  cifrata della card. Resta solo `teacher.json.example`.
- `tools/content/manifest.py`: allowlist esplicita per pacchetto, con controllo che fallisce se trova
  chiavi o token (`"key":`, `sk-`, `AIza`…).
- Licenze: un file `LICENSES.txt` dentro ogni pacchetto (Wiktionary CC BY-SA 4.0, WordNet CC BY 4.0, …).
  Le CC BY-SA richiedono attribuzione e la stessa licenza: il pacchetto lo dichiara.

### F1 — Motore dei pacchetti dati (nv_appstore / nv_store_pkg)

- Nuove destinazioni ammesse: `web`, `tts`, `anima`, `anima/kb`, `nucleos/drivers`.
- **Archivi**: una voce di tipo `tar` (ustar non compresso; i `.gz` del web sono già compressi) viene
  estratta in `<dest>.new/` e poi **scambiata atomicamente** con la cartella vecchia
  (`web` → `web.old`, `web.new` → `web`, cancellazione di `web.old`). Serve per web (547 file) e AKB5
  (62 shard): il limite di 64 righe di `pack.sig` resta, e un pacchetto a metà non rompe mai quello
  installato.
- Correggere i difetti trovati nell'analisi, perché ci appoggiamo a questo codice:
  1. `store remove wiki-*` dalla shell dice "removed" ma non toglie nulla (`term_sh.cpp:2509`).
  2. Gli aggiornamenti dei pacchetti dati non vengono mai segnalati (`abi` 99 viene scartato, `nv_appstore.cpp:1651`).
  3. Un `.part` completo ma corrotto blocca il pacchetto per sempre (`nv_appstore.cpp:513-533`).
  4. Il server di sviluppo non serve `/data/<id>/pack.sig`.
  5. Ogni export rifirma tutti i `pack.sig` (differenze inutili nella repo dello store).
- Test host: parser di `pack.sig` ed estrattore tar sotto libFuzzer (path traversal `../`, nomi lunghi,
  file troncati), come gli altri fuzzer in `tests/host`.

### F2 — Servizio `nv_content`

- **Manifest dei contenuti compilato nel firmware**: quali pacchetti esistono, quali sono consigliati per
  lingua e la **versione minima** che questo firmware richiede (ad esempio `sys-web >= 2026.10.3`).
  Dopo un OTA che richiede un web più nuovo, il servizio aggiorna `sys-web` da solo: il web companion resta
  sempre allineato al firmware, ed è la parte oggi più fragile (non c'è nessun controllo di versione).
- Stato per pacchetto: `ok` / `mancante` / `da aggiornare` / `danneggiato` / `in download x%`.
- Coda persistente su SD (`/sdcard/nucleos/content.queue`): riprende dopo un riavvio, aspetta il Wi-Fi
  (`net_up`) e lo store, con backoff (1, 5, 15, 60 min).
- Verifica a richiesta: ricalcola lo sha256 dei file installati e confronta con il `.pack` firmato.
- API:
  - C: `nv_content_status`, `nv_content_install`, `nv_content_verify`, `nv_content_need(feature)` per gli avvisi.
  - REST: `GET /api/content`, `POST /api/content/install|verify`.
  - Shell: `content status|install|verify|repair`.
  - ANIMA (caps): sa cosa manca e può dirlo ("non ho il dizionario francese, vuoi scaricarlo?").

### F3 — Interfaccia

- **Wizard**: passo `ST_CONTENT` dopo `ST_WIFI`, con gli stati della tabella sopra. Il pulsante principale è
  uno solo ("Scarica · 140 MB"); "Personalizza" apre la lista con le spunte.
- **Impostazioni > Contenuti** (nuova voce nel rail):
  - card in alto con lo stato generale ("Tutto installato" / "3 pacchetti mancanti · Scarica tutto").
  - lista per pacchetto: nome, peso, versione, stato, Scarica / Aggiorna / Rimuovi.
  - Verifica e Ripara.
  - cambiare lingua propone il dizionario di quella lingua.
- **Avvisi al bisogno**: dizionario o L1 mancante (ANIMA), driver mancante (Second Screen); voce quando i pacchetti voce esisteranno.
  Una riga con il pulsante Scarica, mai un errore tecnico.
- **Web companion senza `web/`**: invece del 404 una piccola pagina compilata nel firmware: "Il web companion
  non è ancora installato · Installa ora" (chiama `/api/content/install`).
- Tastiera e mouse su tutto, come da regola del progetto (skill keyboard-support).

### F4 — Pubblicazione

- `tools/content/build.py`: costruisce i pacchetti da `sd/` e dalle cache dei builder, versione `YYYY.MM.N`.
- `tools/dist.py content`: carica gli asset nella release `content-*` e scrive solo le righe `data/<id>` dello
  store (innesto mirato: un export completo trascina le app non pubblicate delle altre sessioni).
- Release del firmware: `nucleoos-p4-sdcard.zip` diventa **l'intero set consigliato**, per chi non ha il
  Wi-Fi o preferisce preparare la SD dal PC. Il dispositivo lo riconosce come installato, perché ogni
  pacchetto porta il suo record `.pack`.
- README e pagina del web flasher: "Al primo avvio la scheda scarica da sola ciò che le serve".

### F5 — Verifica su hardware (prima di qualsiasi OTA pubblico)

Factory reset + SD vuota, poi:

1. Wizard completo: download in background, controllo di ANIMA L1, dizionari e web companion.
2. Wi-Fi staccato a metà download: riprende.
3. Riavvio a metà download: riprende dalla coda.
4. SD piena: messaggio chiaro, nessun file a metà.
5. Nessuna SD.
6. Store irraggiungibile (`store_url` falso): messaggio, poi recupero automatico.
7. `.part` corrotto: viene scaricato di nuovo.
8. OTA che richiede un `sys-web` più nuovo: aggiornamento automatico.
9. Cambio lingua: proposta dei pacchetti di quella lingua.
10. Verifica e Ripara dopo aver corrotto a mano un file.
11. **Aggiornabilità** (G1-G14):
    - un firmware vecchio (1.2.58) con lo store nuovo pubblicato: catalogo, app e `wiki-*` invariati;
    - SD quasi piena: i contenuti si fermano alla riserva, l'OTA successivo passa;
    - OTA nuovo in probation: nessun download finché l'immagine non è confermata;
    - corrente staccata durante lo scambio della cartella `web`;
    - rollback LKG a un firmware vecchio con web nuovo sulla SD: aggiornamento da Impostazioni funzionante;
    - scheda preparata a mano: adozione senza download;
    - `/api/web/put` sulla scheda di sviluppo: niente aggiornamento automatico sopra.

Più i test host (fuzz del tar e del `pack.sig`) e lo smoke `tools/hil/smoke.py`. Rilascio OTA >= 1.2.61.
