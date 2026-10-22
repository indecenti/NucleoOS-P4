# Libri: pipeline del catalogo (F4)

Costruisce in locale gli EPUB e gli indici del catalogo libri di NucleoOS secondo
[docs/BOOKS_PLAN.md](../../docs/BOOKS_PLAN.md) §2. **Non pubblica niente**: `out/` è pronto per F5
(Release `books-<lang>-<n>` + `books/` su Pages), che richiede l'OK dell'utente.

```
python books.py all            # tutto, in ordine (ore: i mirror sono lenti)
python books.py build          # solo validazione + indici + report, dai file già in cache
python -m unittest discover -s tests
```

Requisiti: Python 3.10+ con `requests`, `lxml`, `Pillow`; WSL con `rsync` (mirror Gutenberg) e un venv
`~/se-venv` con `pip install standardebooks` (build di Standard Ebooks). `gh` autenticato per l'elenco dei
repository di Standard Ebooks. Cache in `.cache/` e uscite in `out/` (fuori da git; variabili
`NV_BOOKS_CACHE`, `NV_BOOKS_OUT` per spostarle).

## Fonti

| Fonte | `src` | Come | Diritti |
|---|---|---|---|
| Project Gutenberg | `pg` | metadati dai cataloghi offline (`pg_catalog.csv`, `rdf-files.tar.bz2`); EPUB **solo via rsync dai mirror** (`gutenberg-epub` su ibiblio/pglaf), mai dal sito | PD USA dichiarato da PG + filtro date UE; licenza PG lasciata nel file così com'è; marchio mai nei titoli |
| Standard Ebooks | `se` | sorgenti CC0 su GitHub, EPUB costruito con il loro `se build` (il sito vieta il download agli agenti AI nel robots.txt e il feed OPDS è a pagamento) | CC0 + filtro date UE |
| Wikisource | `ws` | API MediaWiki (`action=parse`), capitoli in ordine, pulizia di tutto ciò che è `ws-noexport`; ws-export è dietro un muro anti-bot | testo PD (filtro date via Wikidata/categorie), trascrizione CC BY-SA 4.0 |
| Wikibooks | `wb` | lista curata `data/wikibooks.json`, stessa API; immagini incluse solo se la licenza di ciascun file è libera | CC BY-SA 4.0 |
| Manuali | `mn` | lista curata `data/manuals.json` (EPUB del progetto o HTML a pagina unica diviso in capitoli) | licenza letta sul documento, campo `evidence` |
| Fumetti | `mn` | Pepper&Carrot (JSON ufficiale, pagine ridimensionate a 1000 px) | CC BY 4.0 |
| archive.org | — | **escluso in v1**: le licenze sono dichiarazioni di chi carica (visti Atwood "CC0", manga, riviste), i testi "fuori diritti" sono scansioni con EPUB da OCR | — |

## Filtri

- **Diritti** (`bk/rights.py`): pubblico dominio solo se *ogni* autore/traduttore/illustratore/curatore è morto
  entro il 1955 **e** l'opera è pubblicata entro il 1930; dato mancante = escluso. Date da Gutenberg, poi
  Wikidata (via i link Wikipedia), poi Library of Congress per Standard Ebooks. Licenze libere ammesse: CC0,
  CC BY, CC BY-SA, GFDL, MIT/Apache/BSD, GPL, PSF; mai NC/ND.
- **Lingue**: solo lingue vive con alfabeto latino, greco o cirillico; niente latino/greco antico/inglese
  antico (scelta dell'utente), niente CJK/arabo/ebraico/devanagari (font).
- **Utilità** (`bk/quality.py`, `bk/curate.py`): via riviste, cataloghi, rapporti, atti, leggi, fette di
  enciclopedia, indici, erotica, medicina popolare d'epoca (consigli pericolosi oggi), testi < 20 KB.
  Inglese curato a ~9000 titoli (scelta dell'utente): tutti i pratici, Standard Ebooks, classici, saggistica
  ≥ 690 download/mese, narrativa ≥ 1200; le altre lingue tengono tutto ciò che passa i filtri.
- **Qualità del testo** sul file scaricato (lettere/caratteri, lunghezza parole, frammenti, simboli).
- **Doppioni** (`bk/dedup.py`): Standard Ebooks > Wikisource > manuali > Gutenberg; i volumi restano separati.
- **Validazione EPUB** (`bk/epubcheck.py`): ZIP valido, `container.xml`, OPF, spine con file presenti. Gancio
  per il parser C dell'app: `NV_BOOKS_EPUB_PARSER=<binario>` viene eseguito su ogni file (exit 0 = apre).

## Uscite

```
out/books/index-v1.json          -> Pages books/index-v1.json
out/books/<lang>/<k>.json        -> Pages books/<lang>/<k>.json  (≤500 righe, per popolarità)
out/releases/books-<lang>-<n>/   -> asset delle Release (hard link alla cache, ≤1000 per release)
out/release-map.json             -> id -> release: tiene stabili gli URL tra una build e l'altra (da conservare in F5)
out/reports/                     -> summary.md/json, <lang>.md (categorie, fonti, licenze, esclusi per motivo), sample-<lang>.md
out/CREDITS.md                   -> attribuzioni CC BY / CC BY-SA / GFDL / BSD / GPL
```

Campi opzionali omessi quando ignoti: `y`, `ad`, `d`, `u2`. `pop` resta interno (serve solo all'ordinamento).
