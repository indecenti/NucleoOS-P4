# Biblioteca offline: lettore ZIM

Stato al 2026-10-12:

- **F0 chiusa**: misure sulla scheda nel §1b, banco di prova in `ports/zim/bench`.
- **F1 chiusa in locale**: libreria in `ports/zim/nvzim`, risultati nel §4b. La CI su GitHub partirà al primo push.
- **F2 chiusa in locale**: `zim` 1.1.0 è nello Store; ANIMA lo usa dal firmware `b07d75a` (risultati nel §4c).
- **F3 chiusa**: l'app Biblioteca 1.0.0 è nello Store (store `7303ad3`, categoria Conoscenza); provata sulla scheda
  con il firmware locale 1.2.113 (risultati nel §4d).
- **F4 chiusa**: immagini e formule nella Biblioteca 1.1.0, nello Store dal 2026-10-12 (store `fcca9e6`);
  aggiornamento dallo Store provato sulla scheda (1.2.115 locale; risultati nel §4e).
- **F5 chiusa in locale** (branch `f5dl` sopra `feat/office`): downloader di sistema `nv_dl` (ABI 18) e schermata
  "Scarica raccolte" nella Biblioteca 1.2.0. Provata sulla scheda con il firmware locale 1.2.142, non ancora pubblica
  (risultati nel §4g).

Obiettivo: leggere sul P4 i file ZIM di Kiwix/openZIM
(Wikipedia completa, Wiktionary, Wikivoyage, medicina, iFixit, Stack Exchange, DevDocs, Gutenberg...) con un'app
grafica dello Store, e dare ad ANIMA e al Terminale lo stesso accesso con un programma `zim`.

Un solo lettore apre **3.631 raccolte** del catalogo Kiwix, 382 in italiano, aggiornate ogni mese, senza che
dobbiamo convertire nulla.

## 1. Cosa sappiamo già (verificato, non ipotesi)

### Il formato, misurato sui file reali

Sonda sui file in `tools/kb/.cache` (`wikipedia_it_top_mini_2026-07.zim`, `wikipedia_en_top_mini_2026-09.zim`):

| Cosa | Valore |
|---|---|
| versione | 6.3, intestazione di 80 byte little endian, magic `0x044D495A` |
| namespace | schema nuovo: `C` contenuti, `M` metadati (13), `W` `mainPage` (redirect), `X` indici |
| voci | IT 195.173, di cui 121.276 redirect; dirent media 32 byte |
| indice dei titoli | `titleIdxPos = -1` (niente lista v0): l'ordine per titolo esiste **solo** in `X/listing/titleOrdered/v1`, array di `uint32` (indici di voce) |
| ordinamento titoli | **per byte**, quindi sensibile alle maiuscole; la lista contiene anche i redirect (IT 62%, EN 93% del campione) |
| cluster | 276 zstd (tipo 5) e 4 non compressi (tipo 1, da 23 a 30 MB l'uno: indici Xapian e liste) |
| frame zstd | livello 19, **finestra dichiarata 8 MiB, dimensione del contenuto assente** |
| cluster decompresso | circa 2 MiB e 186 blob (mediana); compresso circa 160 KB nella variante `mini` |
| tipi MIME | `text/html`, SVG (formule e icone), WebP, CSS, JS |
| checksum | MD5 negli ultimi 16 byte |
| ricerca di Kiwix | `X/fulltext/xapian` e `X/title/xapian` (Xapian, GPL): non utilizzabili da noi |

I file vecchi (versione 5) usano xz (tipo 4). Le offset dei blob sono a 4 byte, o a 8 se il bit `0x10` del byte
di informazione del cluster è acceso.

### Il catalogo

- OPDS: `https://opds.library.kiwix.org/catalog/v2/entries?lang=ita` (`library.kiwix.org` risponde 301 verso qui).
  Per ogni voce: nome, titolo, lingua, variante, numero di voci, illustrazione 48x48, dimensione e link `.meta4`.
- Il `.meta4` (Metalink) contiene la dimensione, lo **SHA-256**, uno **SHA-1 per ogni pezzo da 4 MiB** e i mirror
  HTTPS (nluug.nl, fau.de, accum.se, dumps.wikimedia.org...). Il download si può riprendere pezzo per pezzo,
  verificato.

| Raccolta | Dimensione |
|---|---|
| `wikipedia_it_top_mini` (introduzioni, sorgente di AKB6) | 107 MB |
| `wikipedia_it_medicine_nopic` | 92 MB |
| `wikipedia_it_top_nopic` | 898 MB |
| `wikipedia_it_all_mini` | 2,3 GB |
| `wikipedia_it_all_nopic` | 8,4 GB |
| `wikipedia_it_all_maxi` | 30 GB |
| `wiktionary_it_all_nopic` | 357 MB |
| `wikivoyage_it_all_maxi` | 617 MB |
| `gutenberg_it_all` | 824 MB |
| `ifixit_it_all` | 3,2 GB |
| `unix.stackexchange.com_en_all` | 1,2 GB |
| `devdocs_en_python`, `_c`, `_cpp`, `_git`, `_lua`... | da 0,4 a 7 MB l'una |

### I vincoli del nostro sistema

1. **Offset a 32 bit sopra FatFs.** FatFs ha exFAT e LBA64 accesi (`components/fatfs/src/ffconf.h:255,284`), ma
   newlib ha `off_t` = `long`. Il `os_lseek` di WAMR e `__wrap_pread` (`components/nv_wasm/nv_wasm_wasi.c:336`)
   lo restringono, quindi **un'app non legge oltre 2 GiB** di un file.
2. **Seek su FAT32.** `pread` è `f_lseek` + `f_read`. Su FAT32 un seek all'indietro ripercorre la catena dei
   cluster dall'inizio, e `CONFIG_FATFS_USE_FASTSEEK` è spento (`sdkconfig:1926`). Un lettore ZIM fa molte letture
   casuali, quindi va misurato. Su exFAT i file contigui si posizionano in tempo costante.
3. **Testo.** `gfx_text` è un font 5x7 con sole maiuscole ASCII. L'app deve portarsi i suoi font, come già fanno
   luaapp e puzzles (atlanti a 4 bit generati al build).
4. **Immagini.** Il sistema non decodifica immagini per le app: decodifica l'app (openhasp ha già lodepng).
5. **Rete per le app.** `nv.http_req` mette in buffer tutta la risposta (massimo 1 MB), non segue i redirect, non
   espone le intestazioni e ha un timeout di 30 s. Il downloader dello Store (`nv_appstore`) riprende, calcola
   SHA-256 e segue i redirect, ma accetta solo pacchetti firmati verso cartelle in whitelist, e il suo `fseek` è
   `long` (2 GiB).
6. **File visibili.** Con il permesso `home` un'app WASI vede `/sdcard/home`. La libreria va in `~/zim/`, condivisa
   tra l'app, il Terminale e l'app File.
7. **Memoria.** `ram_budget` arriva al massimo a 16 MB.
8. **ANIMA ha già AKB6.** `docs/ANIMA_KB.md` ha scelto apposta di non leggere uno ZIM a ogni domanda: AKB6 resta
   la via veloce, lo ZIM è il livello sotto.

## 1b. Misure F0 sulla scheda (1.2.100, SD da 16 GB, AOT)

Il file system della SD non è esposto da nessuna API. Lo deduciamo FAT32 dal costo che cresce con la distanza
dall'inizio del file, cosa che un file contiguo su exFAT non fa.

Banco: `ports/zim/bench/zimbench.c` (programma WASI, `bash ports/zim/bench/build.sh`), lanciato nel Terminale.
Campioni in `D:\zimtest\fixtures` (fuori dal repo, SHA-256 verificati sul `.meta4`).

**Dove stanno gli indici.** In tutti i file le dirent e le liste di puntatori occupano gli **ultimi 2-8%** del file
(Wiktionary: 28 MB in fondo a 374 MB). Le dirent sono scritte **in ordine di percorso** (100% dei casi in
Medicina e Wiktionary): un intervallo di indici è un tratto contiguo del file.

**Letture dalla SD** (file di prova da 64 MB):

| Misura | `pread` | `lseek` + `read` |
|---|---|---|
| lettura sequenziale a blocchi da 1 MiB | **0,74 MB/s** | **8,55 MB/s** |
| 4 KiB in un punto a caso | 14,2 ms in media (6,7 ms a 0 MB, 18 ms a 57 MB) | 6,9 ms |
| 4 KiB, letture ordinate in avanti | 13,9 ms | **1,5 ms** |
| 64 byte in un punto a caso | 9,6 ms (1,4 ms a 0 MB, 13,9 ms a 57 MB) | 6,9 ms |
| scrittura sequenziale | 3,7 MB/s | |

- Il `pread` di ESP-IDF fa `f_lseek` + `f_read` e poi torna alla posizione precedente: ogni lettura riparte
  dall'inizio della catena dei cluster di FAT32.
- Su un file da 1 GB scritto apposta (scrittura a 4,35 MB/s), 64 byte con `pread` costano 8 ms all'inizio e
  120 ms nell'ultimo decimo: lineare, circa **0,12 ms per MB** di distanza dall'inizio. Vuol dire circa 50 ms
  in fondo a Wiktionary e circa 250 ms in fondo a una parte da 2 GiB.
- Con `lseek` + `read` la posizione resta dove si è letto. Sullo stesso file da 1 GB:
  - letture ordinate in avanti: **3 ms**;
  - letture a caso: 46 ms in media, perché tornare indietro ripercorre ancora la catena.

**zstd in AOT**: **6,6 MB/s**, circa **290 ms** per un cluster da 2 MiB (sul PC 1,5 GB/s). È la voce più cara
all'apertura di un articolo.
- Decodificando in streaming nel buffer stabile (`ZSTD_d_stableOutBuffer`) e fermandosi appena il blob è
  completo, si decodifica in media il 44-57% del cluster (PC e scheda).
- Sulla scheda servono **176 ms** invece di 292 (−40%), con un contesto di 221 KB e senza la finestra da 8 MiB.

**ZIM su ScoutWiki** (16 MB, 5.256 voci):

| Misura | Risultato |
|---|---|
| ricerca per percorso, senza cache | 58 ms (`lseek`) o 88 ms (`pread`), 11 sonde e 23 letture |
| ricerca per percorso, cache di 64 pagine da 4 KiB | 2,4 ms di mediana, 1,8 letture |
| lista dei titoli trovata | 50 ms |
| prefisso di titolo a freddo | 31-106 ms |
| prefisso di titolo a caldo | 2 ms |
| articolo a freddo (ricerca, cluster, decodifica completa) | 430-570 ms |

**Rete**: Wi-Fi della scheda a **-86 dBm**. HTTPS da Kiwix 128 KB/s, HTTP sulla rete di casa 130-140 KB/s (sia
`wget` sia `nv.http_req` a pezzi da 1 MiB). Il limite è il collegamento radio, non il server né la SD: 92 MB
richiedono circa 12 minuti, una raccolta da 900 MB circa 2 ore. Un mirror (ftp.fau.de) rifiuta la connessione
TLS della scheda; download.kiwix.org e mirror.download.kiwix.org funzionano.

**Formato vecchio** (`icd10_fr_all_2012-01`, ZIM 5.0): cluster xz, namespace `A` (articoli `.html`), `I`, `-`,
`M`, pagina principale dall'intestazione e lista dei titoli v0 nell'intestazione.

**Cosa cambia nel piano**:

- **D12 (nuova):** il lettore usa `lseek` + `read`, mai `pread`, con una cache di pagine da 4 KiB.
  - Le ricerche binarie **memorizzano i livelli alti** (12 di default): le prime sonde sono identiche per ogni
    ricerca, quindi dalla seconda in poi arrivano dalla RAM, senza costi all'apertura.
  - Quando restano 64 voci, i loro puntatori si leggono in una lettura e le loro dirent (contigue, perché in
    ordine di percorso) in una seconda.
- **D13 (nuova, firmware):** `CONFIG_FATFS_USE_FASTSEEK=y`. ESP-IDF crea la mappa dei cluster quando un file viene
  aperto in sola lettura e, se il file è troppo frammentato, continua senza. Aiuta anche video, musica e
  gallerie. Va misurato il costo di apertura dei file grandi e quello in SRAM (256 byte per file aperto).
- **D4 aggiornata:** decodifica parziale in streaming al posto del colpo unico.
- **F5 aggiornata:** con un Wi-Fi debole le raccolte grandi passano dal PC (lettore di schede); dalla scheda si
  scaricano quelle piccole. Il downloader di sistema in background (ABI 18) diventa necessario, perché un
  download di ore non può dipendere dall'app aperta.

## 2. Decisioni

| # | Decisione | Perché |
|---|---|---|
| D1 | Lettore **nostro** in C (`nvzim`), scritto dalla specifica | libzim e Xapian sono GPL-2.0: incompatibili con le licenze commerciali |
| D2 | Tre pezzi sulla stessa libreria: `nvzim` (C, testabile sul PC), `zim` (WASI, Terminale e ANIMA), `biblioteca` (app WASM dello Store) | niente app nativa: la partizione firmware è stretta, lo Store rilascia senza OTA, e l'ABI 16 basta (`gfx_blit` + `gfx_persist`) |
| D3 | File oltre 2 GiB divisi in **parti** `.zimaa`, `.zimab`... sotto i 2 GiB, lette come un file unico | è lo schema di libzim, quindi Kiwix legge le stesse parti; funziona su FAT32 ed exFAT senza toccare il firmware |
| D4 | zstd **in un colpo solo** (`ZSTD_decompressDCtx`) in un buffer da 2 MiB, che cresce a 4 o 8 MiB se serve | senza dimensione nel frame, lo streaming allocherebbe una finestra da 8 MiB; il colpo unico usa l'uscita come finestra |
| D5 | Dei cluster non compressi si legge **solo il blob** che serve, per offset | sono da 23-30 MB |
| D6 | Ricerca per **prefisso di titolo** sulla lista v1, con varianti (come scritto, iniziale maiuscola, tutto minuscolo, iniziali maiuscole) unite e senza doppioni | la lista è ordinata per byte; i redirect coprono già molte grafie. Il testo completo (Xapian) resta fuori dalla v1 |
| D7 | Impaginazione in **modalità lettura**, motore nostro (`nvhtml`), non litehtml | una pagina Kiwix carica più di 10 CSS di MediaWiki: un motore CSS costerebbe secondi e memoria sul P4. La modalità lettura dà tipografia coerente ed è riusabile per EPUB, RSS, Markdown e mail |
| D8 | Font Noto Sans e Noto Sans Mono (OFL), sottoinsieme latino esteso, greco e cirillico, atlanti a 4 bit in 3 dimensioni (normale, grassetto, corsivo) | copre le 5 lingue del sistema e i nomi propri di Wikipedia; un glifo mancante diventa un riquadro |
| D9 | ANIMA: prima AKB6, poi `zim` per i dettagli e per le raccolte specialistiche | rispetta la scelta di `ANIMA_KB.md`; la riga dello strumento va nel prompt solo se `~/zim` contiene raccolte, e le elenca |
| D10 | Nome "Biblioteca" / "Library", mai "Kiwix" | il marchio è loro; nella scheda si scrive "legge i file ZIM di Kiwix/openZIM" |

## 3. Architettura

```
SD   ~/zim/*.zim  |  *.zimaa, *.zimab...   (FAT32 o exFAT, parti < 2 GiB)
          |
nvzim (C)   intestazione, MIME, dirent, cluster zstd/xz, redirect, titoli, metadati, parti,
            cache di pagine da 4 KiB allineate, LRU di 2 cluster decompressi
          |                                   |
zim (WASI, programma di sistema)        biblioteca (WASM, Store)
Terminale e ANIMA                       libreria, ricerca, lettore, catalogo e download
html -> testo con nvhtml                nvhtml: tokenizer -> blocchi -> righe -> atlante glifi
                                        -> framebuffer RGB565 -> gfx_blit (persist, solo righe cambiate)
```

## 4. Fasi

Ogni fase si chiude con la verifica sulla scheda, non prima.

### F0: misure e campioni (piccola, niente firmware)

- Campioni (da scaricare dopo l'ok): `wikipedia_it_medicine_nopic` (92 MB), `wiktionary_it_all_nopic` (357 MB),
  `devdocs_en_c` (1,2 MB), `scoutwiki.org_it_all` maxi (16 MB, con immagini WebP) e un vecchio ZIM xz dall'archivio.
  Le parti e il file oltre 2 GiB si provano con file sintetici, senza scaricare 2 GB.
- Benchmark sul P4 con un'app WASI di prova in AOT:
  - zstd in un colpo solo su 2 MiB;
  - `pread` casuali su un file da 2 GiB, su FAT32 e su exFAT;
  - ricerca completa di una dirent;
  - velocità di `nv.http_req` a pezzi da 1 MiB (HTTPS, mirror del `.meta4`).
- Inventario dei tag e delle classi HTML negli articoli completi (`nopic`), per fissare il sottoinsieme della
  modalità lettura.
- **Uscita:** numeri scritti qui; decisione su FASTSEEK ed exFAT, e su download dentro l'app o ABI 18 (F5).

### F1: libreria `nvzim` e test (media, niente firmware)

- API: `zim_open(parti)`, `zim_meta(chiave)`, `zim_find_path(ns, path)`, `zim_find_title(prefisso, n)`,
  `zim_get(voce)` (vista sul blob), `zim_resolve(redirect)`, `zim_main()`, `zim_random()`, `zim_illustration(48)`,
  `zim_check()` (MD5, incrementale).
- Letture in pagine da 4 KiB allineate, per non pagare il rimbalzo di un settore per comando della SD (lezione di
  `sd-dma-alignment`).
- Test in `tests/host/unit`. Fuzz `fuzz_zim` e `fuzz_zimhtml` in `tests/host/Makefile`, nella CI.
- **Test differenziale** contro python-libzim sul PC (solo come strumento, non distribuito): 10.000 voci di IT ed EN
  con titolo, MIME, redirect e hash del contenuto identici.
- **Uscita:** CI verde, test differenziale a zero differenze.

### F1: risultati (§4b)

**Cosa c'è**:
- `ports/zim/nvzim` (`nvzim.h`, `nvzim.c`, `nvzim_md5.c`), in C99 e senza variabili globali (a parte le tabelle
  CRC di XZ Embedded, in sola lettura);
- `fetch.sh`, che scarica zstd 1.5.7 e XZ Embedded con i checksum fissati;
- gli strumenti in `ports/zim/tools`;
- `ports/zim/README.md`.

**Funzioni**:
- intestazione, tipi MIME, voci e redirect (catene e cicli);
- ricerca per percorso con livelli memorizzati e lettura in blocco degli ultimi 64;
- pagina principale e metadati;
- contenuti da cluster zstd (decodifica parziale che riprende), non compressi (letti sul posto) e xz (ZIM 5);
- puntatori a 8 byte, file divisi `.zimaa`…;
- lista dei titoli v1 e indice v0 dei file vecchi;
- suggerimenti con varianti di maiuscole e accenti latini, voce a caso;
- verifica MD5.

**Prove**:

| Prova | Esito |
|---|---|
| Confronto con python-libzim (`tools/diff_libzim.py`): 10.000 voci di Wikipedia IT e EN top mini e di Wiktionary IT (758.797 voci), 3.000-5.000 di Medicina, DevDocs C, ScoutWiki e ICD-10 2012 (xz); percorso, titolo, tipo, redirect, dimensione e MD5 del contenuto, pagina principale, metadati, ordine della lista dei titoli | **0 differenze** |
| `tests/host/unit/test_nvzim.cpp` su file sintetici (`tests/host/gen_zim.py`): ogni voce per ogni via, configurazione predefinita e minima, file divisi, backend in memoria, file vecchio, troncamenti e byte alterati | **18.105 controlli, 0 falliti**, a 64 e 32 bit con ASan e UBSan |
| `tests/host/fuzz/fuzz_nvzim.cpp` | 2 minuti per architettura (~50.000 e ~30.000 esecuzioni): nessun crash, leak o timeout |
| Sulla scheda (WASI + AOT, ScoutWiki 16 MB) | vedi sotto |

Misure sulla scheda:

| Misura | Risultato |
|---|---|
| 200 ricerche per percorso | **4,1 ms** di media, 21 ms la peggiore, 1,7 letture ciascuna (il banco F0 senza cache: 58 ms) |
| suggerimenti a freddo | 77-92 ms (32-39 letture) |
| `zim get` di un articolo, avvio del programma compreso | 1,3 s |
| verifica MD5 di 16 MB | 5,9 s |

**Corretto strada facendo**:
- con il buffer stabile, zstd segnala l'uscita piena come errore senza conservare il progresso. Il buffer cresce
  e si riparte, e l'input entra 16 KiB alla volta, così ci si ferma davvero vicino al blob;
- un cluster oltre `cluster_max` ora dà `NVZIM_EUNSUPPORTED` e non "file danneggiato".

**Limite noto**: la lista dei titoli è ordinata per byte. Una ricerca che non indovina le maiuscole di una voce
senza redirect ("IPHONE" per "iPhone") non la trova. L'indice laterale del §8 lo risolverà.

### F2: risultati (§4c)

**Store**: `zim` 1.0 pubblicato il 2026-10-11 (store `dd8858d`), poi 1.1.0 (`a5f7876`). Programma di sistema
dal firmware 1.2.108: ogni scheda lo installa da sola.

**Firmware** (`3c6aa48`, provato come build locale 1.2.106 e 1.2.108 sulla scheda di prova):
- `zim` in `kSystemApps`;
- nel blocco AMBIENTE di ogni turno, quando `~/zim` contiene raccolte, una riga con i loro nomi di file e l'uso
  (`zim search`, `zim get --intro`, poi `--sections`/`--section`, "di' quale raccolta hai usato");
- l'intento "cosa sai fare" scattava per "comandi", "elenca" o "aiuto" in qualunque frase; ora solo nelle
  richieste fino a 8 parole (nuovo caso in `test_anima.cpp`).

**ANIMA sulla scheda**: modello online del dispositivo; raccolte Wikimed (97 MB), ScoutWiki, DevDocs C, ICD-10.

| Domanda | Esito |
|---|---|
| "Che raccolte ho nella biblioteca offline?" | ✓ `zim ls`, elenco esatto |
| "Usa zim per leggere cosa dice Wikimed sull'insulina" | ✓ `zim search` + `zim get --intro`, riassunto corretto |
| "Quante persone hanno il diabete nel mondo secondo la biblioteca offline?" | ✓ dopo il correttivo: `--intro`, poi `--section Epidemiologia`, dato e raccolta citati |
| "Cosa dice l'enciclopedia medica offline sull'insulina? … elenca i comandi" | ✗ prima (intento "cosa sai fare", poi comando ripetuto per l'output tagliato), ✓ dopo i correttivi |
| "In the offline C documentation, what does fprintf return?" | ✗ prima (titoli di DevDocs sbagliati), ✓ dopo: `zim get fprintf` trova `io/fprintf` |
| "A cosa serve il paracetamolo? … da dove hai preso l'informazione" | ✗ nessun comando eseguito, ma la risposta cita `zim get` e la raccolta ICD-10: inventato |

**Correttivi venuti dalle prove** (in `zim` 1.1.0, senza firmware):
- una parola che non è un comando vale `get`;
- ricerca dentro titoli e percorsi (`nvzim_scan`), poi per la parola più lunga;
- `--intro` finisce con le sezioni;
- `--max` limita l'output intero (ANIMA legge 2000 byte per comando);
- `-z` accetta il nome del file;
- cache `~/zim/.zim-index` (`zim get` da 2,1 a 1,6 s).

**Build locale 1.2.109** (2026-10-12):
- **Commit**: `b07d75a` su `main`.
- **Contenuto**:
  - titoli e lingua delle raccolte nella riga di ANIMA (da `~/zim/.zim-index`);
  - `zim` di sola lettura per la grammatica `ACT sh` (prima chiedeva "procedo?" ai modelli senza strumenti nativi);
  - spinta "eseguilo adesso" quando una risposta cita `zim get`/`search`/`ls` senza aver eseguito nulla;
  - `--max 1800`;
  - nuovi casi in `test_anima.cpp` e `test_anima_agent.cpp`, 64 e 32 bit verdi.
- **Sulla scheda**:
  - il paracetamolo ora è letto davvero (`zim get Paracetamolo`) e la fonte citata è Wikimed: niente più fonti
    inventate;
  - la risposta è arrivata dopo 344 s, perché il modello ha ripetuto più volte la stessa lettura.
- **Guasto trovato (firmware, non `zim`)**: quando ANIMA lancia un programma da terminale senza schermo, il primo
  lancio di ogni chat (non la prima chat dopo l'avvio) muore 23-25 ms dopo l'istanziazione con
  `wasm: Exception: native stack overflow`.
  - Visto 3 volte su 1.2.109.
  - Gli altri lanci della stessa chat vanno bene, e dal Terminale (anche in pipe) non succede mai.
  - È troppo presto per una ricorsione del programma: sembra un limite dello stack nativo sbagliato per quel lancio
    (`run_worker`, `wasm_runtime_set_native_stack_boundary`).

**Build diagnostica locale 1.2.111** (2026-10-12, log di task, sp, base e limite a ogni lancio da terminale):
- 5 chat ANIMA con il modello locale (Ollama, qwen3.5:9b, niente TLS), 6 lanci di `zim` dal task `sh`: l'errore non
  è tornato.
- `zim` usa circa **1 KB** dei 256 KB di stack nativo: il limite sta 230 KB sotto lo sp, e il minimo libero a fine
  lancio è ~254 KB. Una ricorsione vera è esclusa: se l'errore torna, è un limite sbagliato o uno stack sporcato.
- Le 3 volte su 1.2.109 il modello era quello online (TLS): il sospetto è un legame con le chiamate HTTPS in corso,
  non dimostrato.
- Su `main` resta `f8ef52c`: nessun log nei lanci normali; se un lancio da terminale muore per "native stack
  overflow", un WARN dice task, sp, base, limite e minimo libero. Minimo libero alto = limite sbagliato, basso =
  uso vero.

**Prossimi passi prima di un firmware pubblico**:
- ripetere le chat con un provider online (chiavi sulla scheda) e leggere il WARN di `f8ef52c` in `/api/logs`;
- valutazione ripetibile: casi `zim` in `tools/anima_eval.py`, invece delle domande a mano di questa sessione;
- meno letture ripetute: il paracetamolo ha richiesto 344 s perché il modello ha riletto la stessa voce.

### F3: risultati (§4d)

**Cosa c'è**:
- L'app `apps/biblioteca` (WASI, `app.wasm` 0,97 MB, AOT 1,5 MB) con i sorgenti in `ports/zim/biblioteca`:
  - `doc.c`: l'impaginazione;
  - `font.c`: Noto Sans e Noto Sans Mono (OFL, sottoinsieme latino, greco e cirillico in `fonts/`),
    rasterizzati da stb_truetype a qualunque dimensione;
  - `gfx.c`: il frame buffer;
  - `kbd.c`: la tastiera fisica;
  - `lib.c`: raccolte, copertine, cronologia, segnalibri;
  - `i18n.c`: 5 lingue;
  - `app.c`: le schermate.
- Le icone sono disegnate da `gen_icons.py`, nessuna immagine di terzi.
- `nvhtml` ha una modalità "rich" (`opts.rich`) con marcatori in linea per grassetto, corsivo, codice, apici e
  pedici, link, righe e celle di tabella, voci di elenco, testo preformattato e ancore dei titoli. Le regole sono
  verificate riga per riga da `tests/host/unit/nvhtml_rich_check.h`, nei test e nel fuzzer. Il testo semplice per
  ANIMA non cambia (3632 controlli).
- Le tabelle dentro una cella diventano righe loro, poi la cella riprende (le tabelle di DevDocs).
- **Schermate**:
  - libreria con copertine e "Continua a leggere";
  - lettore con link, ancore, indietro e avanti;
  - ricerca mentre si scrive, con "Cerca dentro i titoli" quando nessun titolo inizia così;
  - sezioni, segnalibri, cronologia;
  - informazioni sulla raccolta con la verifica MD5;
  - tastiera sullo schermo (lettere, accenti, simboli);
  - tema chiaro e scuro, 5 dimensioni del testo, 5 lingue dell'interfaccia.
- **Test sul PC**: `bash ports/zim/biblioteca/test.sh` esegue 7 copioni di tocchi e tasti sulle raccolte di prova.
  - Ogni copione gira due volte: con ASan e UBSan, e con il tetto di memoria del dispositivo.
  - Gli screenshot finiscono in `ports/zim/_out/bib_shots`.
  - Picco di memoria dell'app: 5,8 MiB su 12.
- Con `/appdata/bench.txt` (`FILE<TAB>PERCORSO`) l'app apre la voce e la scorre da sola: è il banco di prova
  ripetibile dello scorrimento sulla scheda.

**Misure sulla scheda** (firmware locale 1.2.113, Wikimed 97 MB su FAT32; tempi nel log di sistema):

| Misura | Risultato | Obiettivo §5 |
|---|---|---|
| libreria, 4 raccolte dalla cache | 234-283 ms | |
| apertura di una raccolta | 26-90 ms | < 300 ms |
| voce a freddo | 200 ms (Paradenti, 15 KB); 550 ms (Paracetamolo, 79 KB: lettura 344, HTML 53, impaginazione 120) | < 1,5 s |
| suggerimenti | primo tasto 254 ms, poi 1-76 ms | < 150 ms a caldo |
| scorrimento | 17-20 fps | >= 30 fps |
| memoria | picco 5,8 MiB (PC, con tetto); `ram_budget` 12 MiB | <= 12 MiB |

**Trovato e corretto sulla scheda**:
1. **Memoria lineare.** WAMR la fa crescere con un realloc del blocco intero in PSRAM. Con la PSRAM frammentata la
   crescita fallisce: ENOMEM, o zstd che fallisce un'allocazione e sembra un file danneggiato, in modo diverso a
   ogni avvio.
   - L'app all'avvio alloca e libera un blocco grande, così la memoria cresce una volta sola, quando il blocco in
     cui l'app è stata caricata è ancora libero.
   - Il `ram_budget` passa da 16 a 12 MiB: con 16 l'app a volte non partiva ("allocate linear memory failed").
2. **nvzim.** `slot_grow` teneva insieme il buffer vecchio e quello nuovo (12 MiB per un cluster da 8). Ora libera
   prima il vecchio: il contenuto viene comunque decodificato di nuovo.
3. **Scorrimento**, da 8 a 20 fps:
   - il memmove del frame buffer (1,1 MB in 88 ms) è sostituito da una tabella di puntatori alle righe che ruota;
   - il canvas 1:1 passava dal render software di LVGL: ora `canvas_scale: fit` va al pannello con il PPA;
   - il firmware `756bff7` dà a queste app anche il puntatore LVGL (mouse e `/api/ui/tap`, prima arrivava solo il
     touch grezzo) e copia le righe del blit con memcpy.
4. **Link parola per parola** (Wikizionario): si fondevano in un link solo. Ora ogni `</a>` chiude il suo link.

**Aperti**:
- **30 fps.** Il limite è la copia del canvas dall'app all'host: 34 ms per 1,1 MB, banda della PSRAM, non il
  codice. Due leve nel firmware, entrambe una nuova ABI:
  - uno scroll fatto dall'host con il PPA, con cui l'app invia solo la striscia nuova;
  - una presentazione senza copia.
- **Firmware, per tutte le app WASI grandi.** Prenotare la memoria lineare massima all'istanziazione (o fare il
  pregrow nel runtime), invece di affidarsi a ogni app.
- **Tastiera.** I tasti di `/api/ui/hid` passano dalla coda di LVGL e non dallo stato letto da `nv_kbd_state`: la
  tastiera è verificata dai test sul PC, va provata con una tastiera vera.
- Le immagini (F4) e lo Store (F6).

### F4: risultati (§4e)

**Cosa c'è**:
- **`nvhtml`**: il testo ricco segna le immagini con `NVHTML_IMAGE`, poi sorgente, dimensioni dichiarate (px o ex),
  `vertical-align` delle formule, flag (formula, formula su riga propria, dentro una figura), testo alternativo.
  Il testo semplice per ANIMA non cambia: una formula resta il suo TeX.
- **`img.c`**:
  - decodifica WebP (libwebp 1.6.0, solo il decoder, scalato già durante la decodifica);
  - PNG (LodePNG);
  - JPEG, GIF al primo fotogramma, BMP (stb_image 2.30);
  - SVG (NanoSVG).
  - Le immagini sono decodificate alla dimensione mostrata e tenute in una cache di 3 MiB (RGB565 + alfa).
  - Mai durante il disegno: una coda, un'immagine per fotogramma a pagina ferma; intanto c'è un segnaposto.
- **Formule**:
  - Gli SVG di MathJax (Wikipedia) usano `<defs>`/`<use>` e `currentColor`, che NanoSVG non conosce: prima del
    parsing ogni `<use transform x y>` diventa il suo path con `transform translate(x y)`, come dice la specifica
    (i pedici sono `scale(0.707)`).
  - Le formule si disegnano come maschera nel colore del testo, quindi si leggono anche col tema scuro.
  - Si allineano alla linea di base del testo con il loro `vertical-align`; 1 ex vale 0,536 em, l'altezza della x
    di Noto Sans.
- **Visualizzatore**: un tocco su un'immagine senza link la mostra a tutto schermo con la descrizione, fino a 3
  volte la dimensione nella pagina. Le formule vettoriali restano nitide su fondo chiaro.
- **Font**: aggiunti l'IPA (le pronunce del Wikizionario), i diacritici combinanti e il greco politonico: 758 KB in
  tutto.
- **Fuzzer** `ports/zim/biblioteca/test/fuzz.sh` (decoder e riscrittura degli SVG, ASan e UBSan):
  - ha trovato due overflow da float a int in NanoSVG (un lato quasi orizzontale fa esplodere la pendenza);
  - li corregge `nanosvgrast.patch`, applicata da `fetch.sh` su una copia: il file scaricato resta verificabile con
    lo SHA;
  - dopo la patch, 99.689 esecuzioni in 5 minuti senza errori.

**Misure sulla scheda** (1.2.115 locale; tempi nel log):

| Misura | Risultato |
|---|---|
| foto WebP 250x333 (ScoutWiki) | 211 ms |
| formula SVG 64x25 ... 221x30 (Wikimed) | 41-48 ms con il cluster già decodificato, 98-338 ms se va decodificato |
| formula ingrandita 663x90 | 297 ms |
| memoria dell'app | picco 7,8 MiB (PC con tetto), budget 12 |
| app.wasm / app.aot | 1,37 MB / 2,43 MB (limiti 2 e 4) |

**Trovato e corretto**:
1. **"Continua a leggere" spariva**: il sistema chiude l'app senza farle finire il ciclo, quindi il salvataggio
   finale non avveniva. Ora la posizione si salva a ogni pagina aperta e dopo 2 s di pagina ferma.
2. **Font**: senza l'IPA le pronunce (ɪ) erano riquadri.
3. **Cluster in memoria**: 2 invece di 1, così la pagina e le sue immagini non si scacciano a vicenda.

4. **Ordine della coda**: prima le immagini il cui cluster non costa decompressione, perché è già in memoria o
   non è compresso (`nvzim_cluster_cached`). Con le 7 formule di Paracetamolo in coda insieme, le due nei cluster
   già pronti passano avanti. Il guadagno qui è piccolo (1,2-1,5 s in tutto): stanno in circa 5 cluster, e ognuno
   costa circa 0,2 s di zstd a 6,6 MB/s.

**Aperti**:
- Una pagina con molte formule le mostra una dopo l'altra, circa 0,2 s per ogni cluster da decomprimere.
- Le immagini senza link non si raggiungono da tastiera: il visualizzatore si apre col tocco o col mouse.
- Una GIF animata mostra solo il primo fotogramma; un PNG o JPEG oltre 1 Mpixel resta un riquadro.

### F5: progetto (§4f)

Dati reali (12 ottobre 2026):
- **Catalogo**: OPDS v2 su `https://opds.library.kiwix.org/catalog/v2/entries?lang=ita&count=N&start=K`, con
  filtri `category=` e `q=`. Il vecchio `library.kiwix.org` risponde 301, e l'ABI 12 non segue i redirect.
  Sono circa 1,3 KB di XML Atom per voce e 382 raccolte in italiano.
- **Ogni voce** porta nome, titolo, riassunto, lingua, data, `articleCount`, `tags` (`_pictures:yes`...), la miniatura
  PNG 48x48 e il link `.meta4` su `lb.download.kiwix.org` con `length` (risposta 200 diretta, senza redirect).
- **`.meta4`** (MirrorBrain, RFC 5854):
  - `size`;
  - `md5`, `sha-1` e `sha-256` del file intero;
  - `<pieces length="4194304" type="sha-1">`, cioè un hash per ogni pezzo da 4 MiB;
  - circa 8 mirror HTTPS con priorità, ordinati per vicinanza.
  - Pesa circa 60 byte per pezzo: 15 KB per un file da 1 GB.

Decisioni:
- **Downloader di sistema, `components/nv_dl` + ABI 18**. Un download di ore non può dipendere dall'app aperta, e
  l'ABI 12 ha risposte da 1 MB, niente redirect e tutto legato all'esecuzione dell'app.
  - Un task suo, con stack in PSRAM: scrive solo sulla SD. Gira un download alla volta e gli altri stanno in coda.
  - Non blocca lo Store: il worker dello Store resta libero, e i due TLS possono stare insieme perché mbedTLS
    alloca in PSRAM.
  - Un flusso solo con `Range: bytes=N-` dal punto verificato. Ogni pezzo da 4 MiB è verificato con SHA-1 appena
    completo.
  - Il registro del job sulla SD si aggiorna a ogni pezzo verificato. Dopo un'interruzione, un riavvio o un mirror
    caduto si riparte dall'ultimo pezzo buono.
  - Un pezzo sbagliato fa cambiare mirror e lo riscarica; il terzo errore sullo stesso pezzo ferma il job.
  - Senza pezzi nel `.meta4` (o con un URL semplice) vale lo SHA-256 del file intero, riletto alla fine.
- **File grandi**: oltre 2 GiB si scrive in parti `nome.zimaa`, `nome.zimab`... da 2 GiB meno 4 MiB, cioè un
  multiplo dei pezzi, così un pezzo non sta mai in due file. `nvzim` le legge come un file solo.
- **Scrittura**: in `nome.part` e rinomina alla fine, solo dopo la verifica completa. Prima di partire si
  controlla lo spazio libero: la dimensione più la riserva dell'OTA (64 MB).
- **Sicurezza**:
  - solo `https`, solo indirizzi pubblici (`nv_net_policy`), redirect seguiti solo verso `https` pubblici;
  - destinazione dentro ciò che l'app vede (`home` -> `/sdcard/home`, `fs` -> la sua cartella dati), senza `..`;
  - un'app vede e controlla solo i suoi download;
  - User-Agent `NucleoOS/<versione> (+https://indecenti.github.io/nucleoos-p4-store)`.
- **ABI 18** (la 17 è dei programmi a schermo intero, "tty"), permesso `net`:
  - `dl_start(spec)` con spec `{"meta4": URL}` oppure `{"url": URL, "size": n, "sha256": hex}`, più `"dest"` e
    `"title"`;
  - `dl_info(id)` e `dl_list()` rispondono in JSON: stato, byte fatti, velocità, errore;
  - `dl_pause(id, on)` e `dl_cancel(id)` (che cancella anche la parte scaricata).
- **Fine del download**: una notifica ("Biblioteca: Wikipedia scaricata"), e il tocco apre l'app.
- **Shell**: `dl` elenca, mette in pausa, riprende, annulla; `dl get URL.meta4 DEST` serve a persone e prove HIL.
- **Biblioteca**:
  - schermata "Scarica" con i Consigliati (per nome) e la ricerca per lingua, categoria e testo, con miniature,
    dimensione, data e numero di voci;
  - "Aggiornamento disponibile" quando c'è lo stesso nome con una data più nuova; la raccolta vecchia si toglie solo
    dopo la verifica della nuova, e solo se l'utente lo chiede;
  - elenco dei download con l'avanzamento.
- **Test**:
  - nucleo puro `nv_dl_core` (parser del `.meta4`, registro dei job, parti, percorsi) con test unitari e fuzzer in
    `tests/host`;
  - **HIL** `tools/hil/dl_resume.py`: circa 900 MB con il Wi-Fi interrotto e un riavvio a metà, poi la verifica e
    l'apertura della raccolta.

### F5: risultati (§4g)

**Cosa c'è**:
- **`components/nv_dl`**: il downloader di sistema descritto nel §4f.
  - Il nucleo puro `nv_dl_core.c` (Metalink 4, registro dei job, parti, percorsi delle app) ha 95 controlli in
    `tests/host` e un fuzzer, che ha trovato un registro leggibile ma non riscrivibile (corretto).
  - `nv_net_policy` passa in `nv_kernel`.
- **ABI 18** (la 17 è dei programmi "tty" di `feat/office`): `dl_start`, `dl_info`, `dl_list`, `dl_pause`,
  `dl_cancel`, `dl_free`.
- **Shell**: `dl ls`, `dl get`, `dl pause`, `dl resume`, `dl cancel`, `dl wait`.
- **Notifica alla fine**: il tocco apre l'app che ha chiesto il download.
- **Biblioteca 1.2.0** (`cat.c`, `get.c`):
  - catalogo OPDS in cache per un giorno, Consigliate per nome, categorie, ricerca;
  - edizioni raggruppate, spazio disponibile, elenco dei download con pausa, ripresa e annulla a due tocchi;
  - "Elimina dalla scheda" nelle informazioni della raccolta;
  - 39 frasi nuove in 5 lingue.
- **Test sul PC**:
  - `test/cat_test.c`: 133 controlli su 8 voci reali del catalogo;
  - `test/fuzz_cat.sh`: 105.000 esecuzioni pulite;
  - `test/get.script`: tutto il percorso con catalogo e downloader simulati, compreso il caso offline;
  - `test/delete.script`: la cancellazione.
- **HIL**: `tools/hil/dl_resume.py`.

**Misure sulla scheda** (1.2.142 locale, Wi-Fi a -85 dBm):

| Misura | Risultato |
|---|---|
| Vikidia nopic, 14,8 MB, 4 pezzi, 5 mirror | 61 s, SHA-256 uguale a quello del `.meta4` |
| Il meglio di Wikipedia nopic, 941.812.270 byte, 225 pezzi, 6 mirror | 71 min con un `wifi off` di 45 s a 56 MB e un riavvio a 173 MB; SHA-256 uguale |
| velocità | 80-390 KB/s, media circa 220 KB/s |
| dopo l'interruzione | ripreso a 58.720.256 byte, cioè dall'ultimo pezzo verificato (14 x 4 MiB) |
| catalogo italiano (382 voci, 466 KB) e miniature | catalogo in una richiesta; miniature una alla volta, poi dalla scheda |
| apertura della raccolta scaricata | pagina principale subito, immagini in 0,3-1,5 s |
| RAM interna statica | +88 byte (586 liberi su 1.002 con `feat/office`) |
| memoria dell'app | picco 5 MiB nella schermata dei download (PC con tetto) |

**Trovato e corretto sulla scheda**: `dl ls` diceva "waiting for: net" per tutta l'ora in cui un job girava
(l'etichetta si aggiornava solo tra un job e l'altro), e una ripresa non lasciava traccia nel log.

**Aperti**:
- La pagina principale di Wikipedia "top" è una griglia di schede: esce come immagini e link in colonna.
- Il primo scan di una raccolta appena scaricata legge i metadati (2,5 s per 942 MB), poi la cache.
- Pubblicazione: prima `feat/office` in main, poi `f5dl`, il firmware pubblico, poi la Biblioteca 1.2.0 nello
  Store (`abi` 18, permesso `net`).

### F2: `zim` nel Terminale e in ANIMA (media; firmware solo per l'aggancio ad ANIMA)

- Comandi:
  - `zim ls` e `zim info ID`;
  - `zim search [-z ID] [-n N] TESTO`;
  - `zim get [-z ID] TITOLO [--intro | --section NOME | --html | --links]`;
  - `zim random [-z ID]` e `zim check ID`.
- Il testo esce da `nvhtml` (paragrafi, elenchi, tabelle a righe), senza passare da `html2text`.
- Prima nello Store come programma da terminale. Con il primo OTA utile: `kSystemApps`, la riga dinamica in
  `ANIMA_SH_TOOLS` e la citazione della fonte ("Wikipedia IT, luglio 2026, offline").
- Valutazione: 10 casi nuovi in `tools/anima_eval.py` (medicina, iFixit, DevDocs, Wiktionary, "dimmi di più su X"),
  misurati prima e dopo.
- **Uscita:** verificato sulla scheda, nessuna regressione nella valutazione.

### F3: app Biblioteca, il lettore (grande, niente firmware)

- Schermate:
  - **Libreria:** copertina 48x48, titolo, lingua, data, numero di voci, spazio occupato.
  - **Ricerca:** suggerimenti mentre si scrive.
  - **Lettore:** indice delle sezioni, link interni, indietro e avanti, cronologia, segnalibri, voce a caso,
    dimensione del testo, tema chiaro o scuro.
  - **Informazioni sulla raccolta:** autore, editore, fonte, licenza, dai metadati.
- `nvhtml`:
  - blocchi: `h1`-`h6`, `p`, `ul`/`ol`/`li`, `dl`, `blockquote`, `pre`, `table` (semplificata, infobox a coppie),
    `figure`/`figcaption`, `hr`, `details`/`summary`;
  - in linea: `b`, `i`, `a`, `sup`/`sub`, `code`, `br`, `small`;
  - si scartano `script`, `style` e la navigazione; le note si raccolgono in fondo.
- Ogni voce si impagina una volta; si disegna solo la parte visibile, lo scorrimento avviene nel framebuffer
  dell'app e si inviano solo le righe cambiate.
- Tastiera e mouse obbligatori (skill `keyboard-support`):
  - Tab, frecce, PagSu/PagGiù, Spazio, Home/Fine;
  - `/` o Ctrl+F per cercare;
  - Alt+freccia sinistra/destra per indietro/avanti;
  - Esc.
- 5 lingue dell'interfaccia.
- Associazione `.zim`: `"file_types": [{"ext": "zim", "mime": "application/x-zim", "kind": "other"}]` più `"opens"`.
  Un file fuori da `~` va spostato in `~/zim/`.
- **Uscita:** obiettivi del §5 misurati sulla scheda, screenshot con la skill `ui-screenshot`.

### F4: immagini e formule (media, niente firmware)

- Formati:
  - WebP (decoder di libwebp);
  - PNG (lodepng);
  - JPEG (decoder software);
  - SVG con nanosvg (formule matematiche e icone);
  - GIF (primo fotogramma).
- Decodifica solo quando l'immagine entra nella vista, scalata alla colonna, con una cache di massimo 4 MiB.
- **Uscita:** pagine corrette di Wikivoyage maxi e ScoutWiki, memoria entro il budget.

### F5: catalogo e download (media; firmware solo se F0 lo chiede)

- Catalogo:
  - OPDS filtrato per lingua, categoria e dimensione;
  - "Consigliati" scelti per nome: Wikipedia top/medicina, Wiktionary, Wikivoyage, Gutenberg, DevDocs;
  - controllo dello spazio libero prima di partire.
- Download:
  1. `.meta4`;
  2. mirror HTTPS;
  3. richieste Range da 1 MiB;
  4. verifica SHA-1 di ogni pezzo da 4 MiB;
  5. ripresa dall'ultimo pezzo buono;
  6. scrittura in parti sotto i 2 GiB e rinomina finale.
- Aggiornamenti: se c'è la stessa raccolta con una data più recente, si propone l'aggiornamento; la vecchia si
  cancella solo dopo la verifica della nuova.
- Se in F0 il download dentro l'app è lento, o se scaricare un GB con l'app aperta è inaccettabile: **ABI 18**, un
  downloader di sistema in background per qualsiasi app, ricavato da quello di `nv_appstore`, con hash fissato dal
  chiamante e notifica alla fine. Servirebbe anche a podcast, RSS e mappe.
- Mai download automatici. User-Agent riconoscibile, mirror dal `.meta4`.
- Dal PC: lettore di schede più `tools/zim_split.py` per i file oltre 2 GiB.
- **Uscita:** 900 MB scaricati con un'interruzione del Wi-Fi e un riavvio a metà, ripresi e verificati.

### F6: Store e rifiniture (piccola)

- `GUIDE.md` e `GUIDE.en.md`, scheda dello Store con QR, righe in `THIRD_PARTY.md`, categoria `knowledge` (oggi
  vuota).
- Screenshot ripuliti dai dati personali (SSID, città) prima di pubblicarli.
- **Uscita:** pacchetto firmato, installato dallo Store sulla scheda e verificato.

## 5. Obiettivi misurati

Sulla scheda, con `wikipedia_it_top_nopic` (898 MB), su exFAT e su FAT32.

| Misura | Obiettivo |
|---|---|
| apertura di una raccolta | < 300 ms |
| suggerimenti a ogni tasto, a caldo | < 150 ms |
| apertura di una voce, a freddo | < 1,5 s |
| apertura di una voce con il cluster già in cache | < 300 ms |
| scorrimento | >= 30 fps |
| memoria dell'app | <= 12 MiB (tetto del manifest 16) |
| `zim get --intro` per ANIMA | < 1 s |
| download con buon Wi-Fi | >= 1 MB/s, ripresa dopo un riavvio |

Budget di memoria previsto:

| Voce | MiB |
|---|---|
| contesto zstd | 0,15 |
| 2 cluster decompressi | 4 |
| cache delle pagine d'indice | 0,25 |
| atlanti dei font | circa 1 |
| impaginazione di una voce lunga | 1-2 |
| framebuffer 1024x600 RGB565 | 1,2 |
| immagini (F4) | fino a 4 |
| **totale** | **circa 12** |

## 6. Rischi

| Rischio | Contromisura |
|---|---|
| Seek su FAT32 lenti (catena dei cluster, FASTSEEK spento): decine di ms per lettura | misura in F0; consigliare exFAT; un file aperto per regione (indice, dirent, cluster) per evitare seek all'indietro; FASTSEEK nel firmware se serve |
| File oltre 2 GiB | parti (D3); gli offset WASI a 64 bit sono un lavoro firmware a parte, utile anche a sqlite e zstd |
| Finestra zstd da 8 MiB | colpo unico (D4) |
| Scritture non latine (CJK, arabo, devanagari) | riquadro nella v1; font aggiuntivi più avanti, su richiesta |
| Contenuto non fidato | fuzz di parser e impaginazione, sandbox WASM, nessun JavaScript eseguito mai |
| HTML diverso a seconda dello scraper | v1 garantita per mwoffliner (Wikimedia), DevDocs, Stack Exchange (sotoki), Gutenberg, iFixit; i siti fatti con zimit (pieni di JS) solo al meglio |
| Carico sui server Kiwix | mirror dal `.meta4`, User-Agent riconoscibile, nessun download automatico |
| Altre sessioni sullo stesso albero | slot di versione e regola di coordinamento prima di ogni rilascio |

## 7. Licenze

| Componente | Licenza | Note |
|---|---|---|
| `nvzim`, `nvhtml`, app, `zim` | nostra | |
| zstd 1.5.7 | BSD-3-Clause | già in `ports/cli` |
| liblzma (xz 5.8.4) | 0BSD | già in `ports/cli` |
| lodepng | zlib | |
| libwebp, solo decoder | BSD-3-Clause | |
| nanosvg | zlib | |
| Noto Sans, Noto Sans Mono | OFL 1.1 | |
| libzim, Xapian | GPL-2.0 | **non usati**; python-libzim solo per i test sul PC |
| contenuti | per raccolta (Wikipedia CC BY-SA 4.0...) | mostrata dall'app a partire dai metadati |

## 8. Dopo la v1

- **Ricerca nel testo completo** con un indice laterale nostro (`.zim.nvx`): costruito sul PC per le raccolte
  consigliate, o sul dispositivo per quelle piccole.
- **Server sulla rete di casa:** `nv_web` serve le raccolte ai browser di telefono e PC, come kiwix-serve (firmware).
- **Offset WASI a 64 bit e FASTSEEK** (firmware): via le parti.
- **Riuso di `nvhtml`** per EPUB, RSS e mail.
