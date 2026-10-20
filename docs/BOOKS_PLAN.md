# Libri: lettore di ebook e catalogo nello Store

Stato al 2026-10-20: piano approvato dall'utente, F1 in corso (branch `feat/books`, worktree `D:\nv_books`).

Obiettivo: un'app **Libri** completa per leggere sul P4 (EPUB, FB2, TXT, HTML, Markdown) con temi da lettore
vero (bianco, seppia, scuro, nero, simulazione e-ink, monocromatico), pagine sfogliate, capitoli e indice, e un
**catalogo di libri liberi nel repo dello Store** (`indecenti/nucleoos-p4-store`), scaricabili dall'app e
raggiungibili da una sezione Libri dello Store. Più lingue possibile, solo contenuti utili (classici, saggistica,
manuali veri), solo contenuti che possiamo distribuire.

## 1. Decisioni

### D1. Un catalogo solo, dentro l'app; lo Store ha l'ingresso, non un secondo catalogo

Valutate tre strade:

| Strada | Pro | Contro |
|---|---|---|
| Libri come righe dello Store (tipo "piattaforma", come Arduboy) | nessuna app nuova per sfogliare | `store2-<lang>.json` ha un tetto di 512 KB condiviso con le app: ~12-15k libri in tutto e poi lo Store **smette di caricarsi** anche per le app; nessuna riga "libro" installabile oggi (`kDataDests`, `asset_ok`); cerca solo per titolo; firmware nuovo per ogni miglioria |
| Store parallelo nel firmware (indice libri separato, UI LVGL nuova) | separato dalle app | due interfacce da mantenere (Store + app), firmware OTA per ogni cambiamento del catalogo, UI LVGL con font Latin-1 (niente greco/cirillico nei titoli) |
| **Catalogo nell'app Libri + sezione Libri nello Store che la apre** | un solo codice di catalogo; aggiornabile dallo Store come app, senza OTA; font completi (greco, cirillico, vietnamita); nessun limite del firmware; le schede con il firmware attuale lo hanno subito | serve l'app installata (lo Store la installa al primo tocco) |

Scelta: **la terza**. L'indice dei libri vive su Pages in `books/` (fuori da `store*.json`, quindi il firmware
vecchio non lo vede e non si rompe), gli EPUB nelle Release dello stesso repo. Nello Store:

- subito (nessun firmware): categoria nuova **Libri** in `server/appstore/catalog.json` con l'app Libri in vetrina;
- con un OTA (F6): un riquadro **Libri** nella pagina Scopri, come quello delle Console, che apre il catalogo
  dell'app (o la installa se manca).

### D2. Dove stanno i file

- **EPUB**: asset delle Release `books-<lang>-<n>` (max 1000 asset per release, < 2 GiB per file, nessun limite
  totale né di banda secondo la documentazione GitHub). Mai in git, mai su Pages.
- **Indici**: Pages `books/` (pochi MB anche con decine di migliaia di titoli).
- **Copertine**: Pages `books/covers/<id>.jpg`, miniature 132x192 JPEG (~5 KB), solo per i libri con una
  copertina vera (Standard Ebooks, scansioni/illustrazioni originali; non le copertine di testo generate da
  Gutenberg). Pages oggi pesa ~790 MB su 1 GB: copertine + indici entro ~120 MB, priorità ai libri più letti.
  Senza copertina l'app disegna una copertina tipografica (titolo/autore su colore generato); dopo il
  download usa quella dell'EPUB.
- **Sulla scheda**: `/home/books/` (visibile in File, `.epub` apribili anche da lì), stato dell'app in `/appdata`.

### D3. Cosa entra (diritti)

Un libro entra solo se è libero sia negli USA (dove sta GitHub) sia in Italia/UE (dove si usa la scheda):

- **pubblico dominio**: tutti gli autori morti entro il 1955 (UE: 70 anni) **e** opera pubblicata entro il 1930
  (USA: 95 anni). Se manca una data verificabile, il libro non entra.
- **licenze libere** che permettono la ridistribuzione anche fuori da uso personale: CC0, CC BY, CC BY-SA, GFDL,
  MIT/Apache/BSD (per i manuali). **Niente NC/ND** (NucleoOS ha anche licenze commerciali).
- **Project Gutenberg**: EPUB ridistribuiti gratis con la loro licenza dentro il file (verbatim). Nelle schede
  "Fonte: Project Gutenberg" con link (riconoscimento, permesso); mai il marchio nei titoli o come promozione.
- **Standard Ebooks**: CC0 sul loro lavoro, testi PD USA → stesso filtro date.
- **archive.org**: solo collezioni curate e con controllo qualità del testo; niente OCR automatico da scansioni
  (EPUB illeggibili) né caricamenti di utenti marcati "Public Domain" a caso (visti: García Márquez marcato PD).
- **Wikisource**: CC BY-SA 4.0, attribuzione nella scheda del libro e nel file.

### D4. Cosa entra (utilità): curato, non "tutto"

- **Classici e narrativa**: le opere canoniche per lingua (download Gutenberg, selezione Standard Ebooks, liste di
  classici di Wikisource), niente doppioni (vince l'edizione migliore: Standard Ebooks > Wikisource > Gutenberg).
- **Saggistica**: filosofia, storia, scienze, economia, poesia, teatro, viaggi.
- **Manuali veri**: programmazione, Linux/Unix, matematica, elettronica, lingue, cucina, primo soccorso, orto, fai
  da te, con licenza libera verificata. Niente manuali di elettrodomestici, cataloghi, opuscoli, documenti sparsi.
- Filtri automatici di qualità: lunghezza minima, testo leggibile (rapporto parole vere/caratteri), EPUB che il
  nostro parser apre davvero (si prova ogni file con il motore dell'app compilato sul PC).

### D5. Lingue

- Interfaccia dell'app nelle 5 lingue di sistema (it, en, es, fr, de), segue `nv_lang()`.
- Catalogo: tutte le lingue con libri utili (it, en, es, fr, de, pt, la, el, ru, nl, ...). Filtro per lingua con
  default = lingua di sistema + inglese, modificabile.
- Sillabazione per la lingua del libro (pattern di Liang, `hyph-utf8`, licenze permissive da verificare per
  ognuna); senza pattern: niente sillabazione, giustificato lo stesso.
- Font: Noto Sans/Serif con latino esteso, greco, cirillico. CJK/arabo/devanagari: non nella prima versione
  (font da 10+ MB): libri in quelle lingue esclusi dal catalogo finché non c'è un pacchetto font.

## 2. Formato del catalogo (contratto tra pipeline e app)

Base: `https://indecenti.github.io/nucleoos-p4-store/books/`

`books/index-v1.json`:

```json
{
  "format": 1,
  "generated": "2026-10-20",
  "langs": [ { "code": "it", "count": 1840, "shards": 4 } ],
  "cats": [ { "id": "novel", "names": { "it": "Romanzi", "en": "Novels", "es": "Novelas", "fr": "Romans", "de": "Romane" } } ]
}
```

`books/<lang>/<k>.json` (k = 1..shards, max 500 righe, ordinate per popolarità):

```json
{ "format": 1, "lang": "it", "part": 1, "parts": 4, "books": [
  { "id": "pg1012", "t": "La Divina Commedia", "a": "Dante Alighieri", "ad": "1265-1321",
    "y": 1320, "c": ["poetry", "classic"], "src": "pg", "lic": "pd",
    "s": 912345, "h": "<sha256 hex>",
    "u": "https://github.com/indecenti/nucleoos-p4-store/releases/download/books-it-1/pg1012.epub",
    "u2": "https://www.gutenberg.org/ebooks/1012.epub3.images",
    "d": "Breve descrizione nella lingua del libro (max 240 caratteri)" } ] }
```

- `id`: `[a-z0-9-]{1,40}`, prefisso della fonte (`pg`, `se`, `ws`, `ia`, `mn` = manuali); nome file = `<id>.epub`.
- `s` + `h` obbligatori: l'app scarica con `nv_dl_start({"url","size","sha256","dest":"/books/<id>.epub"})`,
  il downloader di sistema verifica, riprende, sopravvive ai riavvii; `u2` = mirror se GitHub non risponde.
- `lic`: `pd`, `cc0`, `cc-by`, `cc-by-sa`, `gfdl`, `mit`, ...; `src` + `lic` mostrati nella scheda del libro.
- `cv`: 1 quando esiste `books/covers/<id>.jpg` (132x192).
- Ogni campo nuovo è opzionale; `format` sale solo se cambia qualcosa di incompatibile.

## 3. L'app Libri

Codice in `ports/books/` (riusa da `ports/zim/biblioteca`: `font.c`, `gfx.c`, `kbd.c`, `img.c`, `nvhtml`;
miniz 3.1.2 da `ports/_src` per lo ZIP). App WASI con canvas 1024x600 `fit` (PPA), ABI 18, permessi
`gfx fs home net log`, `file_types` epub/fb2.

- **Formati**: EPUB 2/3 (OPF, spine, NCX/nav, CSS ridotto a quello che conta: corsivi, grassetti, centrato,
  rientri, a capo), FB2 e `.fb2.zip`, TXT (UTF-8/UTF-16/CP1252/Latin-1 riconosciuti), HTML, Markdown.
  PDF no (resta `pdftotext` nel Terminale).
- **Impaginazione**: pagine vere (non scorrimento), giustificato con sillabazione, controllo vedove/orfane,
  capolettera opzionale, margini/interlinea/dimensione/font regolabili. Posizione salvata come
  (capitolo, offset nel testo): resta giusta se cambia il carattere. Impaginazione per capitolo, al bisogno.
- **Temi**: Bianco, Seppia, Scuro, Nero, **E-ink** (16 grigi, carta, immagini con retinatura, sfogliata con lampo
  di refresh come un e-reader, refresh completo ogni N pagine, leggera "ombra" della pagina prima, nessuna
  animazione), **Monocromatico** (1 bit, testo a soglia, immagini con retinatura Atkinson).
- **Sfogliare**: tocco a destra/sinistra, swipe, tasti (frecce, PgSu/PgGiù, spazio), animazioni scorri / copri /
  nessuna; in e-ink e mono solo il lampo.
- **Navigazione**: indice dei capitoli, barra di avanzamento con i capitoli, vai a %, cerca nel libro, segnalibri,
  note a piè di pagina in finestra (EPUB3 `noteref`), link interni, indietro.
- **Biblioteca**: `/home/books`, "Continua a leggere", ordinamenti (recenti, titolo, autore, avanzamento),
  copertine estratte, filtri per lingua, elimina, statistiche di lettura.
- **Catalogo**: indice da Pages (cache in `/appdata`), categorie, lingue, cerca titolo/autore, scheda libro con
  fonte e licenza, scarica (coda `nv_dl`, prosegue ad app chiusa), stato scaricato/in lettura.
- **Lettura ad alta voce** (`nv_speak`, voce offline): frase per frase con evidenziazione. Dopo F4.
- **Tastiera e mouse** completi (skill `keyboard-support`), interfaccia in 5 lingue.

## 4. Fasi

| Fase | Contenuto | Fatto quando |
|---|---|---|
| F0 | questo piano | approvato |
| F1 | motore: ZIP+EPUB+FB2+TXT, impaginazione giustificata, temi, sfogliare, indice; test sul PC (ASan/UBSan, fuzzer ZIP/OPF/FB2, screenshot) | 20 EPUB reali aprono e impaginano senza errori sul PC |
| F2 | biblioteca, impostazioni, segnalibri, note, cerca, sillabazione; app sulla scheda | provata sulla scheda: apertura, sfogliata, cambio tema misurati |
| F3 | catalogo + download (`nv_dl`), schede libro | download vero sulla scheda da un indice di prova |
| F4 | pipeline del catalogo `tools/books/` (fonti, filtri diritti/qualità, dedup, categorie, Release, indici) | indici e Release pronti in locale, rivisti con l'utente |
| F5 | pubblicazione: app nello Store (categoria Libri), Release libri, `books/` su Pages | OK dell'utente, provato dalla scheda |
| F6 | firmware: riquadro Libri in Scopri + apertura/installazione dell'app, `.epub` in File | OTA pubblicato con OK dell'utente |
| F7 | lettura ad alta voce, evidenziazioni, dizionario offline (pacchetti `dict-*`) | dopo F5 |

F4 è indipendente dall'app (rispetta solo il §2): può andare in parallelo.

## 5. Regole del lavoro

- Worktree `D:\nv_books`, commit con file espliciti, autore solo l'utente, niente firme di Claude.
- Export dello Store mai completo dal tree condiviso: export in scratch + innesto solo delle righe nostre
  (vedi `kb-packs-store`); avvisare le altre sessioni prima.
- Screenshot per lo Store 512x300 senza barra di stato con rete/città.
- Pubblicazioni (Store, Release, OTA) solo con OK esplicito dell'utente.
