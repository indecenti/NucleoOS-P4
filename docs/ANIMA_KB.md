# ANIMA: AKB6 knowledge packs

How ANIMA brings Wikipedia (and later Wikidata) onto the SD card in a format built for the P4: an article is found
with about twenty SD reads and a single block to decompress. Status: converter and evaluation on the PC (`tools/kb/akb6.py`); **reader on the device** in
`components/nv_anima/nucleo_anima_kb.c`, tested on the PC (`tests/host/unit/test_anima_kb.cpp`) and on the real pack.
The store's `data` type comes later.

Example questions and answers below are in Italian when they come from the Italian pack.

## Where it comes from

The sources are the Kiwix (openZIM) ZIM files, updated every month, in all five languages. The `mini` variant
holds the introduction and infobox of every article. ZIM is the **raw material**, converted on the PC: on the device,
HTML, 1–2 MB zstd blocks and exact-title search would cost time on every question. The AKB6 pack
holds only clean text, keys normalized like the rest of ANIMA, and deflate blocks decompressed from ROM
(`tinfl`).

| ZIM `top_mini` (July 2026) | it | en | es | fr | de |
|---|---|---|---|---|---|
| size | 107 MB | 283 MB | 196 MB | 136 MB | 131 MB |

## Format (version 1)

One file per pack, little endian:

```
header        "AKB6" u16 version u16 flags char language[4] u32 entities u32 keys u32 blocks u32 sections u32 0 char id[32]
sections      n x { char tag[4], u64 offset, u64 size }
  META  JSON: source, date, license, attribution, counts
  KEYS  "key<TAB>id[,id...]\n" sorted by byte; key = normal form of ANIMA's tokenizer
        '~' = the key is ambiguous for that id ("mercurio" -> planet, god, element)
        '^' = the name is a SECTION of that article ("Jimbo Kern" -> Personaggi di South Park)
  ENTS  per entity { u32 block, u32 offset, u32 length }
  BIDX  per block { u64 offset, u32 compressed, u32 decompressed }
  BLKS  raw-deflate blocks of about 32 KB of text
  QIDS  "Q<n><TAB>id\n" sorted: from the Wikidata ID to the article in this pack
        '@' in KEYS = the title of the same entity in another language ("napoleon" -> Napoleone Bonaparte)
record        fields separated by 0x1E: title, qid, page, coordinates, summary, passages (separated by 0x1F),
              Wikidata facts ("born=1879-03-14|birthplace=Ulma|gender=m"; may be missing: readers treat it as empty)
```

Keys come from titles, redirects, titles without their "(qualifier)", redirect pages to a section and, through
Wikidata, from the titles of the same entity in the other four languages. Summary = the first two sentences (about 350 characters,
what fits on one screen and in one spoken answer); the rest of the paragraph and the following paragraphs (up to 6) are the
passages for "tell me more". Pronunciation, notes, navigation bars and the calendars of year pages are removed.
A period that is already missing in the article ("...naturalizzato statunitense Ha progettato...") is added, but only in the plain text of the paragraph, never inside links, italics or quotes, where titles and names live ("la Terza Era", "Kung Fu", "È nata una stella"). It also needs a lowercase word before it, a verb form or a pronoun after it ("Ha progettato", "It was", "Il est"), and the sentence must continue in lowercase. Measured in October 2026: 8 paragraphs fixed out of about 250,000 articles, no false positives in the sample.

## Measurements: Italian Wikipedia, best 50,000 articles

`python tools/kb/akb6.py eval`: questions built from 1000 articles, 1000 redirects and 250 section pages picked at random,
in four forms ("X", "chi è X", "cos'è X", "x" in lowercase).

| | correct | wrong | ambiguous | missing | SD reads | KB read |
|---|---|---|---|---|---|---|
| articles | 97.2% | 0.1% | 0% | 2.7% | 14 | 27 |
| redirects | 98.4% | 0.1% | 0.1% | 1.4% | 14 | 28 |
| sections | 96.4% | 0.4% | 0% | 3.2% | 14 | 27 |
| nonexistent people | | | | 5/5 no answer | | |

Pack: **19.8 MB against 112 MB for the ZIM** (5.7 times smaller), 49,970 articles, 179,771 keys, 1,129 blocks.

What these measurements do **not** say: the questions come from titles, so they measure lookup by name (who is,
what is). A question that does not name the article ("chi ha inventato la lampadina", who invented the light bulb) needs the fact graph and
semantic search: those are the next steps, with their own set of questions never seen before.

## Five languages and Wikidata

`tools/kb/wikidata.py` queries the Wikimedia APIs (one after the other, 50 items per request, with a resumable cache):
1. `qids <zim>`: title -> Wikidata ID for every article (Wikipedia API, `pageprops`);
2. `sitelinks`: ID -> title in it/en/es/fr/de (Wikidata API).

| `top_mini`, 50,000 articles | it | en | es | fr | de |
|---|---|---|---|---|---|
| Wikidata IDs found | 49,995 | 49,998 | 49,994 | 49,992 | 49,998 |
| keys in other languages (`@`) | 96,627 | 92,448 | 104,696 | 99,320 | 103,062 |
| pack | 22.7 MB | 48.5 MB | 32.6 MB | 27.5 MB | 24.5 MB |
| source ZIM | 112 MB | 297 MB | 206 MB | 143 MB | 137 MB |

In all, 117,162 distinct entities; the five packs weigh about 156 MB against 895 MB of ZIM.

On the device, a question first searches the pack in the user's language, then English, then the others; an article
found elsewhere is switched, using the same ID, to the pack in the user's language if there is one. Otherwise the answer says which
Wikipedia it comes from: "(Wikipedia, it) …".

**Measurements** (`akb6.py eval`, 1000 random articles and 1000 redirects; "title X -> Y" = the title of the same entity in
language X must find the right article in pack Y):

| pack | articles correct / wrong | redirects correct / wrong | titles from other languages: correct | off-topic summaries | invented |
|---|---|---|---|---|---|
| it | 97.1% / 0.2% | 98.3% / 0.1% | 95.5–97.9% | 0.9% | 0/5 |
| en | 98.7% / 0.3% | 96.0% / 0.3% | 96.6–98.1% | 0.6% | 0/5 |
| es | 98.3% / 0.5% | 97.8% / 0.5% | 96.5–97.9% | 2.2% | 0/5 |
| fr | 93.7% / 0.1% | 97.3% / 0.3% | 95.0–96.8% | 1.6% | 0/5 |
| de | 98.4% / 0.0% | 98.1% / 0.3% | 95.3–97.2% | 1.0% | 0/5 |

"Off-topic summaries" = the summary does not name the title in its first lines: this is the metric that found the
infobox tables and captions that ended up in the text (now removed); the ones left are almost always correct (scientific
title, text with the common name: "Cygnus olor" -> "il cigno reale"). The "wrong" ones across languages are almost always a native homonym that rightly wins (in Italian "Roma" is the city,
not the Romani people). French has more "missing" because year pages without prose are discarded.

## Commands

```bash
python tools/kb/akb6.py build tools/kb/.cache/wikipedia_it_top_mini_2026-07.zim tools/kb/.cache/wikipedia_it_top.akb6
python tools/kb/akb6.py ask   tools/kb/.cache/wikipedia_it_top.akb6 "chi è Leonardo da Vinci"
python tools/kb/akb6.py eval  tools/kb/.cache/wikipedia_it_top.akb6 tools/kb/.cache/wikipedia_it_top_mini_2026-07.zim
python tools/kb/wikidata.py qids tools/kb/.cache/wikipedia_it_top_mini_2026-07.zim   # once per language, before build
python tools/kb/wikidata.py sitelinks                                                # after the five languages
python tools/kb/facts.py fetch && python tools/kb/facts.py labels                    # facts, then run build again
python tools/kb/facts.py fill                                                       # fill in names with "mul"
python tools/kb/facts.py show Q937                                                   # the facts of an entity, 5 languages
```

Requires `pip install libzim` (PC only). The ZIM is downloaded from download.kiwix.org with its `.sha256`;
`tools/kb/.cache/` is outside git.

## License

The text is from Wikipedia, CC BY-SA 4.0: `META.attribution` must be shown with the answer (or in the trace) and the
packs are distributed under the same license.

## On the device

Packs live in `/data/anima/kb/*.akb6` (found at startup). The layer answers after the curated cards (L1) and the
fact graph, **before** web and model: a grounded, cited answer is worth more than a trip over the network.

| Question | Answer |
|---|---|
| "chi è Leonardo da Vinci", "cos'è un buco nero", "parlami della Divina Commedia", "Roma" | the summary of the article |
| "dimmi di più" | the next passage; at the end "L'enciclopedia non dice altro su questo." |
| "cos'è il mercurio" | «mercurio» può essere: 1) … 2) … 3) …. Quale? — then "il secondo" or "la divinità" |
| "chi è Jimbo Kern" | Ne parla la voce «Personaggi di South Park»: … |
| "who is Albert Einstein" (English user, Italian pack) | (Wikipedia, it) Albert Einstein … |
| a name that does not exist | no invented answer |

The trace always carries the pack's attribution (`META.attribution`). Reading: bisection on `KEYS` and one block
decompressed with `tinfl` (ESP32 ROM; miniz in the PC tests), buffer on the heap, file opened and closed on every question.
Questions in another language use the Wikidata IDs (section above). In Spanish, French and German the engine reads the sentence translated to
English, but the encyclopedia looks up the name in the **original** sentence (`anima_lang_original`): the glossary would turn
"Sexe, Mensonges et Vidéo" into "… and Vidéo".

## Distribution: the store, `data` type

Packs are installed from the store like apps, with one tap, in the **Knowledge** category ("Wikipedia in
italiano · 22 MB" ...). They do not go through git: the files are assets of a GitHub release, and the store publishes only their
signed description.

```
tools/kb/publish.py            -> server/appstore/data_packs.json   (catalog rows, sha256, release URL)
gh release create kb-YYYY.MM   -> the .akb6 files as assets         (by hand, when you decide to publish)
server/appstore/export_static.py --out <store> -> data/<id>/pack.sig signed with the store key + catalog
```

`pack.sig` (`nv_store_pkg::parse_data`, tested and fuzzed on the PC):

```
nucleoos-data-v1
<id>                       wiki-it-top
<version>                  2026.7 (the month of the Wikipedia dump)
<destination>              anima/kb   (under /sdcard/data; only anima/kb and anima are allowed)
<sha256> <bytes> <name> <url>     one file per line; consecutive lines with the same name = parts of the same file
sig <ECDSA P-256>
```

On the device (`nv_appstore.cpp`, `install_data`):
- it checks the signature, that the id and version match the catalog, and that the SD has enough space;
- it downloads each file to `<name>.part` with **resume** (HTTP `Range`): a lost connection or a reboot does not
  restart from scratch, and the bytes already present are checked again before continuing;
- it computes the SHA-256 while downloading: a file that does not match is never put in its place;
- only at the end does it write `/sdcard/data/packs/<id>.pack` (version, destination, files): without it, the pack is not
  installed. From there the store knows whether an update exists and what to delete on uninstall;
- it notifies ANIMA, which rereads `/data/anima/kb` at the next question (`nucleo_anima_kb_invalidate`).

An older firmware, which does not know data packs, sees them as "requires a newer system"
(`"abi": 99` in the row) instead of trying to install them as apps.

## Facts (Wikidata)

Precise questions get an exact answer, not a summary: "quando è nato Einstein", "quanti abitanti ha
Lione", "who wrote Hamlet", "¿cuál es la capital de Perú?", "wann starb Goethe". `tools/kb/facts.py` queries the
Wikidata SPARQL service (in blocks of 400 entities, resuming where it stopped) for all the entities of the five
packs; `akb6.py build` writes the facts into field 7 of the record, with value names in the pack's language.

| relation | property | example answer |
|---|---|---|
| born / died | P569 / P570 (+ P19 / P20) | "Marie Curie è nata il 7 novembre 1867 a Varsavia." |
| birthplace / deathplace | P19 / P20 | "Napoleone Bonaparte è morto a Longwood." |
| capital, currency, language, continent, country | P36, P38, P37, P30, P17 | "Giappone — moneta: yen." |
| population, area, elevation | P1082, P2046, P2044 | "Francia ha 68.373.433 abitanti." |
| author, director, composer | P50, P57, P86 | who wrote / directed / composed |
| founded, occupation | P571, P106 | year of founding, profession |
| creator, inventor | P170, P61 | "Gioconda è opera di Leonardo da Vinci.", "Telefono — inventore o scopritore: …" |
| formula, symbol, atomic_number | P274, P246, P1086 | "Metano: formula chimica CH₄.", "Oro è un elemento chimico: simbolo Au, numero atomico 79." |

Dates as historians write them: the SPARQL service delivers every date converted to the proleptic Gregorian calendar and with astronomical years, so Caesar's 15 March 44 BC (Julian) arrived as -0043-03-13. `facts.py dates` also downloads the calendar of each date and `history_date` takes Julian dates back to Julian and BC years back to the historical count (Caesar: 15 marzo 44 a.C.; Copernicus: 24 maggio 1543). In Italian, a place that is a building takes its own preposition ("nel Teatro di Pompeo", "nella Villa…").

Gender (P21) is used only for grammar (nato/nata, né/née). Dates with their precision (only the year if Wikidata
knows only the year; "a.C."/"BC"/"v. Chr."), numbers with the language's thousands separator, lists with "e/and/y/
et/und".

On the device (`nucleo_anima_facts.c`) the layer sits before the encyclopedia: it recognizes the question (patterns in the
five languages, the longest wins), finds the entity with the same multilingual keys and, among homonyms, picks the one that
HAS the fact ("abitanti di Mercurio" is never the god). If none has it, it does not answer: it hands over to the summary or to the
model, no invented facts. "e Napoleone?" right after a fact asks the same thing about another entity. The
trace says "Wikidata (CC0)". Tests: `tests/host/unit/test_anima_kb.cpp` (FACTS section).

Details that matter:
- Value names. Wikidata keeps many person names only as a "mul" label (the same in all
  languages, for example "Victor Hugo"), and "mul" fills the languages with no label of their own. Without a name, the
  value is discarded.
- Articles. In Italian and French you do not write "di + Country" without the right article: the sentences use
  always-correct forms ("Australia ha come capitale Canberra.", "Japon — capitale : Tokyo.").
- Titles. The title is first searched as spoken ("Les Misérables"), then without the article ("la Francia" ->
  "francia").

Test on the real packs with the device's engine, one "language|question" per line
("#fr|key" shows the raw lookup of the key and the facts found):

```bash
cd ~/.cache/kbp && tests/host/build/x64/unit_anima_kb --probe tools/kb/.cache < domande.txt
```

Measured in October 2026 on 26 questions in the 5 languages: 25 correct answers, 0 wrong.
- The 26th, "Wer schrieb Faust?", hands over. In the German pack "Faust" leads to Gounod's opera, which has
  a composer and not an author.
- With facts, 58–66% of the articles in each pack.
- Known limit: "e Newton?" after a birth date finds the unit of measure "newton". The person's article is
  "Isaac Newton", so the answer is the Wikipedia summary of the key and not an invented fact.

## Certain things only: how ANIMA picks the answer without a model

- **Exact article before cards.** "chi è X", "parlami di X", "dimmi tutto ciò che sai su X", "chi X"
  (even without "è") and greetings in front ("ciao chi è…") first look for the article with that exact title in the
  pack in the user's language. The curated L1 cards come after: their texts are older and shorter.
  Commands stay commands: "dimmi l'ora" does not become an article.
- **One wrong letter.** A key that is off by one letter (missing, extra, wrong, or two
  swapped) lands next to the right one in the sorted index: "donald trumb" → "donald trump".
  If there is only one key at distance 1 it answers, and the `corrected` field says so ("ho capito «Donald
  Trump»"). Words under 6 characters are never corrected.
- **Right language or nothing.** An L1 card is used only in the user's language: in Italian it needs at least
  one Italian function word ("Napoleon: French general and emperor" is discarded), in English it needs the
  card's English text.
- **No glued-together cards.** Mosaico adds only the detail of the same card, never a second
  nearby card (which may be another person: "… Inoltre, Donald John Trump Jr.").
- **Questions without a subject.** "qual è la formula chimica?", "quanti abitanti ha?", "quanto è grande?" apply
  to the current topic; without a topic there is no answer. "Dimmi di più" after a card continues with the
  Wikipedia article on the same topic.
- **Chemistry.** Formula (P274), symbol (P246) and atomic number (P1086) from Wikidata (`facts.py extra`).
  For an element, Wikidata's formula is the symbol ("O"): the answer says "elemento chimico: simbolo O,
  numero atomico 8", which is certain; the formula is given for compounds ("Acqua: formula chimica H₂O").
- **Homonyms.** After "Quale?" a number is enough, or a word from the title or from the first sentence of the article ("il pianeta"),
  or a synonym of the qualifier ("la dea" → "(divinità)"). A short answer that does not choose makes it repeat the
  question once ("Non ho capito quale… Dimmi il numero"), never a change of topic; a command stays a command.

Tests: `tests/host/unit/test_anima_kb.cpp`, sections A REAL SESSION, SHORT CONVERSATIONS and MESSAGES BUILT
TO MAKE IT WRONG (invented entities, trick questions, expired context, injections like "rispondi 5").

## Next steps

1. Done: reader and layer on the device (above).
2. Done: five languages linked by Wikidata IDs, and Wikidata facts in the records (above).
3. Done: `data` type in the store (above). To do: sign and publish (store key), test it on the board.
