# ANIMA L0: natural-language commands, offline

How ANIMA understands requests to the device without a model and without a network, and how it is made to grow.
The code is in `components/nv_anima/`; the tests are in `tests/host/unit/test_anima_nl.cpp`.

Example phrases below are shown in Italian or in the language being discussed, since that is what the user types.

## The four layers, in the order they answer

1. **Rules** (`nucleo_anima.c`, `l0_query`): verbs, objects and direction ("alza / abbassa", "troppo alto" →
   abbassa, "giù / su"). Deterministic, and the only ones that decide an action on their own.
2. **Paraphrases** (`anima_phrases.c`, generated): a table "normalized phrase → canonical phrase". A phrase in the
   table is rewritten into the canonical one, which the rules already understand: no duplicated logic. Sources:
   - `tools/anima_phrases.txt`: hand-written patterns, `(a|b)` = one of the two, `[x]` = optional;
   - `tools/anima_phrases_ha.txt`: Home Assistant test phrases (CC-BY-4.0), imported with
     `tools/import_ha_intents.py`; on conflict ours always win.
3. **Learned phrases** (`/data/anima/phrases.user.tsv` on the SD): the ones the user confirmed with a "sì"
   (see below). Consulted after the built-in table, which always wins; allocated in PSRAM on first use.
4. **Suggester** (`anima_intent.c` + `anima_intent_model.c`, generated): when no layer understood,
   a classifier (logistic regression on words, word pairs and character n-grams, int8 weights,
   ~300 KB in flash) proposes the closest request: *"Non sono sicuro di aver capito: intendi «alza il
   volume»? (sì/no)"*. **It never executes anything on its own.**
   - Thresholds: probability ≥ 0.45 and margin over the second ≥ 0.15, then the class's language.
   - Polarity filter: it never proposes the opposite direction of the words spoken ("più silenzio" will never
     be «alza il volume»).
   - "sì" → runs the canonical phrase and learns the phrase (layer 3); "no" → asks for other words and does not learn;
     anything else → the proposal lapses. Only the person who received it can answer, within a minute.

When nothing answers, the "I don't know" says what works offline: never an empty answer.

## Context between turns

A fragment that continues the previous turn is **rewritten into a complete request** before entering the
layers, the way a paraphrase enters with its canonical phrase (`nucleo_anima_query`, `anima_followup_math` in
`anima_solve.c`). Today this applies to calculations:

| Before | Then | Rewritten | Answer |
|---|---|---|---|
| "6x6" | "più 5?", "e meno 6", "+ 4" | "36 piu 5" | "Partendo da 36: fa 41." |
| "10 + 10" | "raddoppia", "il doppio?", "e la metà?" | "20 * 2" / "20 / 2" | |
| "12 per 12" | "la radice di quello", "al quadrato" | "la radice di 144" | |
| "6 times 6" | "plus 5?", "and times 2", "halve it" | "36 plus 5" | "Starting from 36: …" |

The same holds for the other domains (`anima_context.c`, `anima_ctx_rewrite`), rewriting the previous request:

| Before | Then | Rewritten |
|---|---|---|
| "che tempo fa a Roma?" | "e a Milano?", then "e domani?" | "che tempo fa a milano", "che tempo fa a milano domani" |
| "che ore sono a Tokyo" | "e a New York?", "e Londra?", "what about Paris?" | "che ore sono a new york" |
| "converti 5 km in miglia" | "e 10?", then "e in metri?" | "converti 10 km in miglia", "converti 10 km in metri" |
| "alza il volume" / "abbassa la luminosità" | "di più", "ancora di più", "a bit more" / "di meno" | same direction / "abbassa …" |

The rewritten request appears in `corrected` ("ho capito: …"). Cities and conversions are now verified
by resolving the rewrite ("e a Gotham?" does not turn into an answer); a place is never a pronoun or "casa".

Rules, so that a calculation does not swallow the next request:
- the previous substantial turn must be in the same domain, at most 3 turns ago (a "grazie" in between is fine,
  "apri le note" closes the topic);
- the fragment is short (≤ 8 words) and made **only** of numbers, operators and words from a closed vocabulary
  (operators, verbs like "togli", "raddoppia", anaphors like "quello", conjunctions like "e", "ora"): "per favore
  apri le note" stays a request to open Notes;
- it must really refer to the result ("quanto fa 2 per 3" stands on its own) and the rewrite must be
  computable; otherwise the sentence goes in as it is;
- the answer says which number it started from, so a wrong context is seen at once.

### What a fragment refers to: one rule

Two kinds of memory, with two rules (`ctx_fresh`, `topic_set` in `nucleo_anima.c`):

| Memory | Examples | Holds as long as |
|---|---|---|
| **conversation** | "più 5?", "e a Milano?", "e Newton?", "dimmi di più", "spiegati meglio" | it is the **last substantial turn**, at most 3 turns ago |
| **device** | "chiudila" (the open app), "aprilo" (the last file), "ripeti" (the last action) | the device does not change it |

Dialog acts ("grazie", "sei sicuro?") do not move the last turn, so they do not break the thread; every other
request does. Before, each had its own window: the focus of "e Newton?" lasted 8 turns whatever was said
in between, and "dimmi di più" held forever on the last topic (even after a reboot). Now:
- "dimmi di più" without a fresh topic asks "Di più su cosa?" instead of fishing up an old one;
- only a "bare" request continues the topic: "dimmi di più su Einstein" and "voglio più volume" have their own.

Tests: "CONTEXT" section of `tests/host/unit/test_anima_nl.cpp`, including the cases that must **not** be read
as follow-ups.

## Spanish, French, German

The engine reasons in Italian and English. A request in Spanish, French or German is **read through
English** and the answer comes back in the user's language (`anima_lang.c`, the `nucleo_anima_query` wrapper):

```
"¿Cuánto es 6 por 7?"  -> fold characters -> phrase table / glossary -> "what is 6 times 7" -> engine
engine: "It's 42."     -> answer table                                  -> "Da 42."
```

1. **Folding**: lowercase, á ñ ü ö ä ß ç œ to ASCII, ¿ ¡ and apostrophes to spaces (`anima_lang_fold`, same as
   `xfold()` in `tools/gen_anima_phrases.py`).
2. **Whole phrase**: the paraphrase table has `es:` `fr:` `de:` keys with an **English** canonical phrase. Sources:
   `tools/anima_phrases_xl.txt` (by hand) and the Home Assistant test phrases (`tools/import_ha_intents.py`, CC BY 4.0).
3. **Glossary** (`GL_ES`, `GL_FR`, `GL_DE`): verbs, apps, settings, weather, math, dictionary; the longest
   match wins ("pon el volumen al" before "pon"). Words it does not know pass through unchanged:
   names, numbers, the text of a note. "a/à/um" before a number become "at" (the time of a reminder).
4. **Yes on its own**: "sí", "oui", "ja", "vale", "d'accord", "klar"... mean "yes" only if they are the whole sentence.
5. **Answers**: a table of English patterns with placeholders (`RP`), then dates and names of days and months
   ("Saturday, October 3, 2026" -> "sábado, 3 de octubre de 2026" / "samedi 3 octobre 2026" / "Samstag, 3. Oktober 2026").
   The `{value}` placeholders that the OS fills in later stay where they are. An answer the table does not know stays in
   English: that is what these users got before, never an invented translation.

| Example | Answer |
|---|---|
| "sube el volumen" / "monte le son" / "mach lauter" | Subo el volumen. / J'augmente le volume. / Ich mache lauter. |
| "abre la calculadora" / "ouvre la calculatrice" / "öffne den Taschenrechner" | Abro / J'ouvre / Ich öffne + the app name |
| "¿Cuánto es 6 por 7?" / "combien font 6 fois 7" / "wie viel ist 6 mal 7" | Da 42. / Ça fait 42. / Das ergibt 42. |
| "pon un temporizador de 5 minutos" | Temporizador de 5 min en marcha: sonará a las 21:51. |
| "¿qué hora es en Tokio?" | En Tokyo son las 04:46. |
| "6x6" then "más 5" | Partiendo de 36: Da 41. (context applies here too) |
| "traduce perro al inglés", "traduce dog al español", "traduci cane in spagnolo" | dog, hound / perro / perro (via English) |
| "traduci cane in tedesco", "traduci perro in italiano", "translate chien to english" | Hund (the Italian "cane", not the English walking stick) / cane (from Spanish) / dog (from French) |
| "come si dice gatto in tedesco" without the entry | Non ho "gatto" nel dizionario offline di tedesco. (never an IT<->EN answer in its place) |

**With a model.** The first pass is on the device only (`s_xl_device_only`): "sube el volumen" does not wait for
the network. If the device does not understand and a usable model exists, the model receives the **original** sentence.

**Where the language comes from.** The ANIMA app and Telegram use `nv_anima_lang()` (the system language); the web passes
`lang`. The offline voices speak Italian and English: an answer in Spanish, French or German stays on screen.

**Dictionaries** (see below): `dict-{es,fr,de}-en.tsv`, `dict-en-{es,fr,de}.tsv` and `forms-{es,fr,de}.tsv` from
Wiktionary; Italian <-> Spanish/French/German goes through English.

**Limits**: monolingual definitions exist in Italian and English; in Spanish, French and German "what does X
mean" gives the English translation. A model's answers follow the language of its prompt. The values the
OS writes into `{value}` (network, memory, capacity) are in English.

## Offline dictionaries: definitions, synonyms, antonyms, translations

An L0 tool (`nucleo_anima_lex.c`) answers from dictionaries on the SD, without network and without a model. Like the
translator, it is grounded by construction: a word is in the file or it is not, and the answer says where it comes from.

| Request | Example | Answer |
|---|---|---|
| definition | "cosa significa effimero", "che vuol dire procrastinare?", "definizione di serendipità", "what does ephemeral mean" | up to 3 senses with the part of speech, then 4 synonyms |
| synonyms / antonyms | "sinonimi di veloce", "il contrario di felice", "synonyms of happy", "opposite of cold" | up to 6 words |
| inflected forms | "cosa significa andavamo", "traduci correvano in inglese", "translate went to italian" | goes through the lemma and says so: «andavamo» è una forma di «andare» |
| word from the other language | "cosa significa ephemeral", "what does gatto mean" | the definition, with the language and the translation |
| no definition | "cosa significa casa" | says so, and gives the translation |
| "cos'è X" that no layer knows | "cos'è un ornitorinco" | the definition, before the "I don't know" |

Not a dictionary request: "sono contrario alla guerra" (missing "di"), "che significa questo?" (a
pronoun), "cosa significa 404", "cosa vuoi dire". A missing definition lets the question pass to L1 and to the
model; missing synonyms and antonyms give an honest "it's not in the dictionary". "Sinonimi di pioggia" is not the weather.

### Files on the SD (`/data/anima/`, generated, outside git)

| File | Content | Source | License | Entries |
|---|---|---|---|---|
| `lex-it.tsv` | senses, synonyms, antonyms, spelling | Wikizionario | CC BY-SA 4.0 | 67,000 |
| `lex-en.tsv` | senses, synonyms, antonyms, spelling | Open English WordNet 2025 | CC BY 4.0 | 126,000 |
| `forms-it.tsv` | inflected form -> lemma | Wiktionary (Italian entries) | CC BY-SA 4.0 | 526,000 |
| `forms-en.tsv` | inflected form -> lemma | rules + irregular verbs on the WordNet lemmas | CC BY 4.0 | 80,000 |
| `dict-{es,fr,de}-en.tsv`, `dict-en-{es,fr,de}.tsv`, `forms-{es,fr,de}.tsv` | translations and forms | Wiktionary (Spanish, French, German entries) | CC BY-SA 4.0 | see `DICTIONARIES.txt` |
| `dict-it-en.tsv` | IT -> EN translations | Wiktionary + Wikizionario + FreeDict/WikDict | CC BY-SA 4.0 | 122,000 |
| `dict-en-it.tsv` | EN -> IT translations | FreeDict/WikDict + inverted Wiktionary | CC BY-SA 4.0 | 97,000 |
| `DICTIONARIES.txt` | sources, versions, licenses, counts | | | |

About 50 MB in all. Each file is `key<TAB>value` sorted by byte: the firmware looks it up by bisection
directly on the SD (`anima_dict_get`, about twenty reads, one 2 KB buffer on the heap, nothing resident).
Keys are normalized like the firmware's tokenizer (`anima_dict_tokenize`, shared by all the
dictionaries), and the generator writes them the same way.

```bash
python tools/dicts/gen_dicts.py fetch    # download the sources into tools/dicts/.cache (~385 MB)
python tools/dicts/gen_dicts.py build    # write sd/data/anima/*.tsv (~4 min with ES/FR/DE)
python tools/dicts/gen_dicts.py check    # look up words as the firmware does, ordering and EN -> IT quality
```

Then `tools/sync-sd.ps1 -Drive X:` copies the files to the card.

Quality, generator choices:
- EN -> IT translations put first the words on which **two sources agree** (FreeDict lists them and they are the
  first Wiktionary gloss), then the most **used** (FrequencyWords frequencies from OpenSubtitles, CC BY-SA 4.0) and
  those of the English word's main part of speech (WordNet): "chair" -> sedia before presiedere,
  "door" -> porta before anta;
- EN -> IT discards source noise: senses marked slang, vulgar, humorous, literary, poetic, dialectal or
  "relational" (water -> idrico) stay only as a fallback for words that nothing else translates (alacritous ->
  alacre); a word with an offensive sense ("bike" -> puttana) only if Wiktionary and FreeDict both confirm that
  exact meaning; affixes ("-ato") are dropped, as are inflected forms next to the lemma ("cuori" after "cuore"), English left over
  in the lists, function words ("de" for "rome"); FreeDict's proper nouns come through (Italy -> Italia).
  `gen_dicts.py check` verifies this on a list of words (the first translation and those that must not appear);
- in each entry the part of speech with the most senses comes first ("andare" the verb, "fast" the adjective);
- archaic, rare or specialist senses go at the bottom; Wikizionario template leftovers are discarded. Some
  common entries ("casa") have no extractable definition: in that case it answers with the translation.
- the word is read first in the speaker's language: for an Italian speaker "cane" is the dog, not the English *cane*
  (stick); then English as written, then Italian;
- an entry with a sense of its own is a lemma even if it also has a "form of" sense ("música" = music and feminine of
  "músico"): before, it was left out of EN -> ES;
- EN -> ES/FR/DE inverts the displayed glosses; later senses (up to the fifth) fill in only the English words
  that no first sense translates ("cat" -> Katze from the third sense), without displacing "queen" -> Königin.

The PC tests use miniature dictionaries written by the test (`write_lexicon_fixture` in
`tests/host/unit/test_anima_nl.cpp`): same format, same lookup.

## Tools (on the PC)

| Command | What it does |
|---|---|
| `python tools/gen_anima_phrases.py [--check]` | regenerate the table and tests from the paraphrases (`--check`: fails if out of date, as in CI) |
| `python tools/import_ha_intents.py <checkout home-assistant/intents>` | re-import the Home Assistant phrases |
| `python tools/import_nucleo_evals.py <checkout NucleoOs>` | re-import the Cardputer ANIMA action corpora as tests |
| `python tools/augment_anima_intent.py [--verify]` | **GPU**: a local LLM (Ollama, default `qwen3.5:9b`) generates paraphrases and "hard" negatives; `--verify` acts as judge and keeps only the right ones (rejects in `.rejected`) |
| `python tools/train_anima_intent.py [--eval-only]` | train the suggester, evaluate it on the unseen set and write the C weights + the golden cases |
| `python tools/dicts/gen_dicts.py fetch\|build\|check` | download the sources, generate and verify the offline dictionaries (see above) |
| `python tools/anima_misses.py [--promote]` | from the board: phrases not understood, proposals made, phrases learned; `--promote` moves the latter into the built-in table |

The three training scripts require `numpy`, `scikit-learn`, `pyyaml` (PC only; what goes
into the firmware is generated C, committed).

## The improvement loop

1. The board logs every turn (`telemetry.ndjson`) and the confirmed phrases (`phrases.user.tsv`).
2. `anima_misses.py` shows what was not understood and what the suggester would propose.
3. The right phrases become patterns in `anima_phrases.txt` (or `--promote` for the confirmed ones).
4. `augment_anima_intent.py` + `--verify` widen the training data with the GPU.
5. `gen_anima_phrases.py`, `train_anima_intent.py`, `make -C tests/host`: table, weights and tests.

## The measurements (160 hand-written phrases, never seen)

The set `tools/anima_intent_heldout.txt` has 160 phrases: 30 are already in the table (L0 understands them), the others measure
the suggester. A contamination check removes from training every phrase equal to a test one.

| Variant tried | Correct proposals | Wrong | Non-command phrases with a proposal | Outcome |
|---|---|---|---|---|
| LLM phrases judged by the 9B | 62/95 | 5 | 5/35 | replaced |
| **LLM phrases reviewed by hand + state/past/negation filters** | **62/95** | **2** | **3/35** | **in use** |
| Conformal prediction (CICC) | fewer proposals, 0 wrong | | | dropped: with the confirmation, a missed proposal costs more than a wrong one |
| Energy score for off-topic | AUROC 0.82 against 0.975 for the "-" class | | | dropped |
| Filter by agreement between models | 31 against 37 (57-phrase set) | | | dropped: removes the hard negatives, the most useful data |
| Hard negatives and confusable pairs generated | 33-35 against 37 (57-phrase set) | | | dropped: too many wrong labels |
| Bigger weights (up to 4.8 MB) | within noise | | | dropped: the limit is the data |

Suggester filters (identical in Python and in C): direction opposite to the words spoken; a STATE with no verb or
comparative ("il volume è basso") is a complaint and inverts the direction, like "troppo"; a story in the past or
a negation ("ho alzato il volume ieri", "non toccare la musica") never gets a proposal.

LLM judge: `--judge-model qwen3.6:35b-a3b-mtp-q4_K_M --num-gpu 16` (35B mixture-of-experts, 3B active, half the layers on the
8 GB GPU, the rest in RAM) is much stricter than the 9B but slow (~1 h for 2,400 phrases). For this round the phrases
were reviewed by hand: 408 discarded out of 1,373 (for example "go home" as directions to a house, "resume" as
resuming a conversation, "non vedo niente" labeled as lowering the light).

## Why not a semantic encoder for commands

`tools/anima_intent_heldout.txt` contains phrases that no layer has ever seen: an honest measurement, never to be used
for training. The test fails if a new model gets fewer of them right or more of them wrong.

"Static" sentence encoders (the one L1 already uses, distilled from multilingual-e5-small, and also
potion-multilingual-128M and static-similarity-mrl-multilingual) were measured on the same phrases: they give
**opposites** ("apri / chiudi la musica" 0.83) a higher similarity than real paraphrases ("alza il
volume / aumenta l'audio" 0.31). They are fine for finding knowledge, not for choosing a command: that is why
the suggester is a trained classifier, with confirmation and a polarity filter.

## Memory: only what is active is in PSRAM

None of the above takes memory when ANIMA is not working:

| What | Where | When |
|---|---|---|
| Paraphrase table, suggester weights | flash (`const`), ~0.3 MB | always mapped, 0 bytes of RAM |
| Learned phrases (up to 2048) | PSRAM heap, ~360 KB | on first use of ANIMA |
| L1 encoder (3 MB int8) | PSRAM heap | on first use, only if ≥ 8 MB remain free |
| Knowledge index and shards (`nucleo_anima_l1.c`, `l1_fopen`) | PSRAM heap, up to 24 MB, files up to 12 MB | on first use, only if ≥ 8 MB remain free; LRU |

The AKB5 router picks the shards a question needs, the way a mixture-of-experts picks its experts:
the active ones stay in PSRAM. The memory broker (`nv_mem_reclaimer_add("anima-l1-mirrors")`) frees all the
copies when a heavy app (camera, video) asks for memory; they are reloaded at the next question. The *static*
PSRAM (`.ext_ram.bss`, watched by `tools/ci/check_budgets.py`) stays almost untouched: everything
large is dynamic.

## Tests

`make -C tests/host` (Linux/WSL, clang) also runs `anima_nl`:
- **natural-language phrases:** Italian and English, with the regressions found by testing on the board;
- **paraphrases:** each paraphrase must give the same intent and argument as its canonical phrase;
- **Cardputer corpus:** the curated one is blocking, including the phrases that *mention* an action without
  asking for it and must never execute it; the raw one is only a metric that cannot go down;
- **suggester:** C/Python parity on the golden cases, thresholds on the unseen set and the
  proposal → yes → executes → learns flow.
