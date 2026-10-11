# Biblioteca offline: lettore ZIM

Stato al 2026-10-11:

- **F0 chiusa**: misure sulla scheda nel §1b, banco di prova in `ports/zim/bench`.
- **F1 chiusa in locale**: libreria in `ports/zim/nvzim`, risultati nel §4b. La CI su GitHub partirà al primo push.
- **F2 in corso**:
  - fatto: il programma `zim` (`apps/zim`, `ports/zim/cli`) e la conversione HTML→testo `nvhtml`, provati sul PC
    (`nvhost`, interpretato e AOT) e sulla scheda;
  - da fare: pubblicazione nello Store, aggancio ad ANIMA (firmware) e valutazione. Obiettivo: leggere sul P4 i file ZIM di Kiwix/openZIM
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
  scaricano quelle piccole. Il downloader di sistema in background (ABI 17) diventa necessario, perché un
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
- **Uscita:** numeri scritti qui; decisione su FASTSEEK ed exFAT, e su download dentro l'app o ABI 17 (F5).

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
- Se in F0 il download dentro l'app è lento, o se scaricare un GB con l'app aperta è inaccettabile: **ABI 17**, un
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
