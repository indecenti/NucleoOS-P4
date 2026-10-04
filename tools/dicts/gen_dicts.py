#!/usr/bin/env python3
"""ANIMA offline dictionaries: download the sources and build the sorted TSV files the firmware reads.

    python tools/dicts/gen_dicts.py fetch            download the sources into tools/dicts/.cache
    python tools/dicts/gen_dicts.py build            build sd/data/anima/*.tsv (then tools/sync-sd.ps1)
    python tools/dicts/gen_dicts.py check            look words up the way the firmware does

Output (sd/data/anima/, generated, not in git):
    dict-it-en.tsv   IT key -> English translations     FreeDict/WikDict + Wiktionary (CC BY-SA)
    dict-{es,fr,de}-en.tsv, dict-en-{es,fr,de}.tsv, forms-{es,fr,de}.tsv
                     the same for Spanish, French, German   English Wiktionary (CC BY-SA 4.0)
    dict-en-it.tsv   EN key -> Italian translations     FreeDict/WikDict + Wiktionary, inverted (CC BY-SA)
    lex-it.tsv       key -> senses, synonyms, antonyms, headword   Italian Wiktionary (CC BY-SA 4.0)
    lex-en.tsv       key -> senses, synonyms, antonyms, headword   Open English WordNet 2025 (CC BY 4.0)
    forms-it.tsv     inflected form -> lemma(s)         English Wiktionary, Italian entries (CC BY-SA 4.0)
    forms-en.tsv     inflected form -> lemma(s)         regular rules + irregular verbs over OEWN lemmas
    DICTIONARIES.txt sources, versions, licences, counts

Every file is "key<TAB>value" sorted by key in byte order: the firmware bisects it on the SD
(anima_dict_get in components/nv_anima/nucleo_anima_lex.c). Keys are normalized exactly like the
firmware's tokenizer (norm_key below); a line never reaches MAX_LINE bytes.
"""
import argparse
import collections
import datetime
import gzip
import json
import os
import re
import sys
import tarfile
import urllib.request
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
CACHE = os.path.join(HERE, ".cache")
OUT = os.path.join(ROOT, "sd", "data", "anima")

SOURCES = {
    "kaikki-it-en.jsonl.gz": ("https://kaikki.org/dictionary/Italian/kaikki.org-dictionary-Italian.jsonl.gz",
                              "Wiktionary (English edition), Italian entries, via kaikki.org/wiktextract",
                              "CC BY-SA 4.0 / GFDL"),
    "itwiktionary.jsonl.gz": ("https://kaikki.org/itwiktionary/raw-wiktextract-data.jsonl.gz",
                              "Wikizionario (Italian edition), via kaikki.org/wiktextract", "CC BY-SA 4.0 / GFDL"),
    "oewn-2025.xml.gz": ("https://github.com/globalwordnet/english-wordnet/releases/download/2025-edition/"
                         "english-wordnet-2025.xml.gz", "Open English WordNet 2025", "CC BY 4.0"),
    "kaikki-es-en.jsonl.gz": ("https://kaikki.org/dictionary/Spanish/kaikki.org-dictionary-Spanish.jsonl.gz",
                              "Wiktionary (English edition), Spanish entries, via kaikki.org/wiktextract",
                              "CC BY-SA 4.0 / GFDL"),
    "kaikki-fr-en.jsonl.gz": ("https://kaikki.org/dictionary/French/kaikki.org-dictionary-French.jsonl.gz",
                              "Wiktionary (English edition), French entries, via kaikki.org/wiktextract",
                              "CC BY-SA 4.0 / GFDL"),
    "kaikki-de-en.jsonl.gz": ("https://kaikki.org/dictionary/German/kaikki.org-dictionary-German.jsonl.gz",
                              "Wiktionary (English edition), German entries, via kaikki.org/wiktextract",
                              "CC BY-SA 4.0 / GFDL"),
    "freedict-ita-eng.src.tar.xz": ("https://download.freedict.org/dictionaries/ita-eng/2025.11.23/"
                                    "freedict-ita-eng-2025.11.23.src.tar.xz",
                                    "FreeDict+WikDict ita-eng 2025.11.23", "CC BY-SA 3.0"),
    "freedict-eng-ita.src.tar.xz": ("https://download.freedict.org/dictionaries/eng-ita/2025.11.23/"
                                    "freedict-eng-ita-2025.11.23.src.tar.xz",
                                    "FreeDict+WikDict eng-ita 2025.11.23", "CC BY-SA 3.0"),
}

MAX_LINE = 1900          # firmware line buffer is 2048 (LEX_LINE)
MAX_TOKENS = 4           # longest key kept (a phrase); the firmware handles up to 24
SENSES = 4               # senses kept per headword
SENSE_CHARS = 200        # one gloss, cut at a word boundary
LIST_ITEMS = 12          # synonyms / antonyms / translations kept

# ---- the firmware's normalization ------------------------------------------------------------------
# anima_dict_tokenize(): lowercase ASCII alnum; the lowercase Italian accented vowels fold to their base
# letter; every other character (uppercase accented letters included) is a separator; a token keeps at
# most 23 characters, a key at most 24 tokens.
FOLD = {"à": "a", "á": "a", "â": "a", "è": "e", "é": "e", "ê": "e", "ì": "i", "í": "i", "î": "i",
        "ò": "o", "ó": "o", "ô": "o", "ù": "u", "ú": "u", "û": "u"}


def tokens(s):
    out, cur = [], []
    for ch in s:
        c = FOLD.get(ch) or (ch.lower() if ch.isascii() and ch.isalnum() else None)
        if c:
            if len(cur) < 23:
                cur.append(c)
        elif cur:
            out.append("".join(cur)); cur = []
    if cur:
        out.append("".join(cur))
    return out[:24]


def norm_key(s):
    t = tokens(s)
    return " ".join(t) if 0 < len(t) <= MAX_TOKENS else ""


# Spanish, French, German: the device folds their letters to ASCII before reading (anima_lang_fold in
# components/nv_anima/anima_lang.c, mirrored char for char): "pequeño" -> "pequeno", "Straße" -> "strasse".
XFOLD = {}
for _src, _dst in (("àáâãäå", "a"), ("ç", "c"), ("èéêë", "e"), ("ìíîï", "i"), ("ñ", "n"), ("òóôõöø", "o"),
                   ("ùúûü", "u"), ("ýÿ", "y")):
    for _ch in _src:
        XFOLD[_ch] = _dst
        XFOLD[_ch.upper()] = _dst
XFOLD.update({"ß": "ss", "æ": "ae", "Æ": "ae", "œ": "oe", "Œ": "oe", "'": " ", "’": " "})


def xkey(s):
    return norm_key("".join(XFOLD.get(ch, ch) for ch in s).lower())


WIKILINK = re.compile(r"\[\[(?:[^\]|]*\|)?([^\]]*)\]\]")   # "[[d']]acqua" -> "d'acqua"


def clean(s):
    s = WIKILINK.sub(r"\1", s or "")
    s = re.sub(r"\s+", " ", s.replace("\t", " ")).strip()
    return s


def cut(s, n=SENSE_CHARS):
    s = clean(s).rstrip(" .;:")                                # the firmware adds its own punctuation
    if len(s) <= n:
        return s
    s = s[:n].rsplit(" ", 1)[0].rstrip(",;:")
    return s + "…"


def add_unique(lst, item, cap=LIST_ITEMS):
    item = clean(item)
    if item and len(lst) < cap and item.lower() not in (x.lower() for x in lst):
        lst.append(item)


def jsonl(path):
    with gzip.open(path, "rt", encoding="utf-8") as f:
        for line in f:
            try:
                yield json.loads(line)
            except ValueError:
                continue


def write_tsv(name, rows):
    """rows: {key: value}. Sorted by key bytes; lines over MAX_LINE are shortened, never dropped silently."""
    path = os.path.join(OUT, name)
    n = 0
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        for k in sorted(rows, key=lambda x: x.encode("utf-8")):
            v = rows[k]
            if not k or not v:
                continue
            line = f"{k}\t{v}"
            while len(line.encode("utf-8")) >= MAX_LINE:
                v = v[: int(len(v) * 0.8)].rsplit(" ", 1)[0] + "…"
                line = f"{k}\t{v}"
            f.write(line + "\n"); n += 1
    print(f"  {name}: {n} keys, {os.path.getsize(path) / 1e6:.1f} MB")
    return n


# ---- sources --------------------------------------------------------------------------------------

def fetch():
    os.makedirs(CACHE, exist_ok=True)
    for name, (url, what, _) in SOURCES.items():
        dst = os.path.join(CACHE, name)
        if os.path.exists(dst) and os.path.getsize(dst) > 0:
            print(f"  have {name} ({os.path.getsize(dst) / 1e6:.1f} MB)"); continue
        print(f"  get  {name}  <- {url}")
        tmp = dst + ".part"
        urllib.request.urlretrieve(url, tmp)
        os.replace(tmp, dst)
        print(f"       {os.path.getsize(dst) / 1e6:.1f} MB  ({what})")


def freedict(name):
    """FreeDict TEI -> {headword: [translations]} in entry order."""
    src = os.path.join(CACHE, f"freedict-{name}.src.tar.xz")
    ns = "{http://www.tei-c.org/ns/1.0}"
    out = collections.OrderedDict()
    with tarfile.open(src, "r:xz") as tf:
        member = next(m for m in tf.getmembers() if m.name.endswith(f"{name}.tei"))
        for _, el in ET.iterparse(tf.extractfile(member)):
            if el.tag != ns + "entry":
                continue
            orth = el.find(f"{ns}form/{ns}orth")
            if orth is not None and orth.text:
                lst = out.setdefault(orth.text.strip(), [])
                for q in el.iter(ns + "cit"):
                    if q.get("type") == "trans":
                        qt = q.find(ns + "quote")
                        if qt is not None and qt.text:
                            add_unique(lst, qt.text)
            el.clear()
    return out


GLOSS_SPLIT = re.compile(r"\s*[;,]\s*")
PAREN = re.compile(r"\([^)]*\)|\[[^\]]*\]")


def gloss_items(gloss, max_words=5):
    """'freezing, biting (of cold)' -> ['freezing', 'biting']: short translation-like pieces of a gloss."""
    g = PAREN.sub("", gloss or "")
    items = []
    for it in GLOSS_SPLIT.split(g):
        it = clean(it).strip(".:")
        if it.lower().startswith(("i.e", "e.g", "eg ", "ie ", "see ", "used ")):     # a note, not a translation
            continue
        if it and len(it.split()) <= max_words and not re.search(r"\b(of|form)\b.*\bof\b", it):
            items.append(it)
    return items


IT_POS = {"noun": "s.", "verb": "v.", "adj": "agg.", "adv": "avv.", "pron": "pron.", "prep": "prep.",
          "conj": "cong.", "intj": "inter.", "phrase": "loc.", "name": "n. pr.", "num": "num.", "det": "det.",
          "article": "art.", "prefix": "pref.", "suffix": "suff.", "abbrev": "abbr.", "particle": "part."}
EN_POS = {"n": "n.", "v": "v.", "a": "adj.", "s": "adj.", "r": "adv."}
SKIP_TAGS = {"obsolete", "archaic", "dated", "rare", "historical", "misspelling", "nonstandard"}
FORM_WORDS = r"(plurale|femminile|maschile|singolare|persona|participio|gerundio|indicativo|congiuntivo|" \
             r"condizionale|imperativo|infinito|forma)"
# Template residue in Wikizionario glosses: "casa ( approfondimento) f sing", "Pyrus ( tassonomia)".
IT_JUNK = re.compile(r"\(\s*(approfondimento|citazioni)\s*\)")
IT_TEMPLATE = re.compile(r"\s*\(\s*(tassonomia|approfondimento|citazioni)\s*\)")


def word_item(x):
    """A synonym/antonym is a word or a short phrase, never a note: "(per es. palazzo, scuola" is dropped."""
    x = clean(x)
    return x if x and "(" not in x and ")" not in x and len(x.split()) <= 4 else ""


IT_FORM_OF = re.compile(rf"^.*\b{FORM_WORDS}\b.*\b(?:di|del|della|dell')\s*([^\s,;.]+)\s*$", re.I)


# ---- Italian: forms + IT->EN glosses (English Wiktionary) --------------------------------------------

def read_kaikki(fname="kaikki-it-en.jsonl.gz", lc="it", nk=norm_key):
    """English Wiktionary entries of one language: forms -> lemma, word -> English glosses, lemmas, usage."""
    forms = collections.defaultdict(collections.Counter)      # form key -> Counter(lemma)
    it_en = collections.OrderedDict()                          # word -> [english]
    lemmas = set()
    weight = collections.Counter()                             # meanings: how much a word is used
    seen_entry = set()
    for d in jsonl(os.path.join(CACHE, fname)):
        if d.get("lang_code") != lc:
            continue
        w = d.get("word", "")
        if d.get("pos") == "name":                             # "a surname", "a town in ...": not a translation
            continue
        first_entry = w not in seen_entry
        seen_entry.add(w)
        weight[w] += sum(1 for s in d.get("senses", []) if not s.get("form_of"))   # meanings, not conjugations
        is_form = False
        nsense = 0
        for s in d.get("senses", []):
            fo = s.get("form_of") or []
            if fo:
                is_form = True
                for x in fo:
                    if x.get("word"):
                        forms[nk(w)][x["word"]] += 1
                continue
            if SKIP_TAGS & set(s.get("tags", [])) or nsense >= (2 if first_entry else 1):
                continue
            nsense += 1
            for g in s.get("glosses", [])[:1]:
                for item in gloss_items(g)[: (3 if first_entry else 1)]:
                    add_unique(it_en.setdefault(w, []), item, 6)
        if not is_form:
            lemmas.add(w)
            for f in d.get("forms", []):                       # conjugation / plural tables of the lemma
                tags = set(f.get("tags", []))
                if tags & {"canonical", "romanization", "table-tags", "inflection-template"}:
                    continue
                plain = (f.get("links") or [[None, f.get("form")]])[0][1] or f.get("form")
                k = nk(plain or "")
                if k and k != nk(w) and len(k.split()) == 1:
                    forms[k][w] += 1
    return forms, it_en, lemmas, weight


# ---- Italian lexicon (Italian Wiktionary) ------------------------------------------------------------

def read_itwikt(forms):
    lex = collections.OrderedDict()                            # key -> dict(senses, syn, ant, heads)
    it_en = collections.defaultdict(list)
    for d in jsonl(os.path.join(CACHE, "itwiktionary.jsonl.gz")):
        if d.get("lang_code") != "it":
            continue
        w = d.get("word", "")
        k = norm_key(w)
        if not k:
            continue
        pos = IT_POS.get(d.get("pos", ""), "")
        senses = []
        for s in d.get("senses", []):
            gl = " ".join(s.get("glosses", [])[:1])
            if not gl or IT_JUNK.search(gl):
                continue
            gl = IT_TEMPLATE.sub("", gl)
            m = IT_FORM_OF.match(gl)
            if m and len(gl) < 90:                              # "plurale di casa": a form, not a meaning
                forms[k][m.group(2)] += 1
                continue
            rank = 1 if (SKIP_TAGS & set(s.get("tags", [])) or s.get("topics")) else 0
            senses.append((rank, len(senses), f"{pos}: {cut(gl)}" if pos else cut(gl)))
        for t in d.get("translations", []):
            if t.get("lang_code") == "en" and t.get("word"):
                add_unique(it_en[w], t["word"])
        if not senses:
            continue
        e = lex.setdefault(k, {"senses": [], "syn": [], "ant": [], "heads": [], "groups": []})
        add_unique(e["heads"], w, 3)
        e["groups"].append([x[2] for x in sorted(senses)])
        for x in d.get("synonyms", []):
            add_unique(e["syn"], word_item(x.get("word", "")))
        for x in d.get("antonyms", []):
            add_unique(e["ant"], word_item(x.get("word", "")))
        for s in d.get("senses", []):
            for x in s.get("synonyms", []):
                add_unique(e["syn"], word_item(x.get("word", "")))
            for x in s.get("antonyms", []):
                add_unique(e["ant"], word_item(x.get("word", "")))
    # Parts of speech by number of senses: "andare" the verb before "l'andare" the noun. The commonest
    # use gets up to 3 senses, every other one its first sense.
    for e in lex.values():
        groups = sorted(e.pop("groups"), key=lambda g: -len(g))
        e["senses"] = groups[0][:3] + [g[0] for g in groups[1:]]
    return lex, it_en


# ---- English lexicon (Open English WordNet) ----------------------------------------------------------

IRREGULAR = """be was were been being am is are|have had has having|do did done does doing|go went gone goes going
say said|make made|get got gotten|know knew known|think thought|take took taken|see saw seen|come came|give gave given
find found|tell told|become became|leave left|feel felt|bring brought|begin began begun|keep kept|hold held|write wrote written
stand stood|hear heard|let|mean meant|set|meet met|run ran|pay paid|sit sat|speak spoke spoken|lie lay lain|lead led|read
grow grew grown|lose lost|fall fell fallen|send sent|build built|understand understood|draw drew drawn|break broke broken
spend spent|cut|rise rose risen|drive drove driven|buy bought|wear wore worn|choose chose chosen|seek sought|throw threw thrown
catch caught|deal dealt|win won|forget forgot forgotten|lay laid|sell sold|fight fought|eat ate eaten|teach taught|sing sang sung
fly flew flown|swim swam swum|sleep slept|drink drank drunk|ride rode ridden|shoot shot|hide hid hidden|freeze froze frozen
steal stole stolen|wake woke woken|bite bit bitten|shake shook shaken|forgive forgave forgiven|feed fed|bleed bled|blow blew blown
dig dug|hang hung|shine shone|sink sank sunk|spin spun|stick stuck|sting stung|strike struck|swear swore sworn|tear tore torn
child children|man men|woman women|person people|mouse mice|foot feet|tooth teeth|goose geese|ox oxen|life lives|knife knives
wife wives|leaf leaves|wolf wolves|half halves|good better best|bad worse worst|far farther further farthest furthest"""


def en_forms(lemma, pos):
    w = lemma
    out = set()
    if not re.fullmatch(r"[a-z]+", w):
        return out
    vow = "aeiou"
    if pos == "n" or pos == "v":
        if re.search(r"(s|x|z|ch|sh)$", w): out.add(w + "es")
        elif re.search(r"[^aeiou]y$", w): out.add(w[:-1] + "ies")
        else: out.add(w + "s")
    if pos == "v":
        stem = w[:-1] if w.endswith("e") and not w.endswith("ee") else w
        if re.search(r"[^aeiou]y$", w):
            out.update({w[:-1] + "ied", w + "ing"})
        else:
            out.update({stem + "ed" if not w.endswith("e") else w + "d", stem + "ing"})
            if len(w) >= 3 and w[-1] not in vow + "wxy" and w[-2] in vow and w[-3] not in vow:
                out.update({w + w[-1] + "ed", w + w[-1] + "ing"})
    if pos in ("a", "s") and len(w) <= 7:
        if w.endswith("e"): out.update({w + "r", w + "st"})
        elif re.search(r"[^aeiou]y$", w): out.update({w[:-1] + "ier", w[:-1] + "iest"})
        else:
            out.update({w + "er", w + "est"})
            if len(w) >= 3 and w[-1] not in vow + "wxy" and w[-2] in vow and w[-3] not in vow:
                out.update({w + w[-1] + "er", w + w[-1] + "est"})
    out.discard(w)
    return out


def read_oewn():
    entries = collections.OrderedDict()                        # key -> list of (pos, [sense ids], [synsets], head)
    sense_lemma, sense_ants = {}, collections.defaultdict(list)
    synset_def, synset_members = {}, collections.defaultdict(list)   # synset -> lemmas, in entry order
    with gzip.open(os.path.join(CACHE, "oewn-2025.xml.gz"), "rb") as f:
        for _, el in ET.iterparse(f):
            tag = el.tag.split("}")[-1]
            if tag == "LexicalEntry":
                lem = el.find("Lemma")
                w, pos = lem.get("writtenForm"), lem.get("partOfSpeech")
                ss = []
                for s in el.findall("Sense"):
                    sense_lemma[s.get("id")] = w
                    ss.append(s.get("synset"))
                    synset_members[s.get("synset")].append(w)
                    for r in s.findall("SenseRelation"):
                        if r.get("relType") == "antonym":
                            sense_ants[w].append(r.get("target"))
                k = norm_key(w)
                if k:
                    entries.setdefault(k, []).append((pos, ss, w))
                el.clear()
            elif tag == "Synset":
                d = el.find("Definition")
                synset_def[el.get("id")] = d.text if d is not None else ""
                el.clear()
    lex, forms = {}, collections.defaultdict(collections.Counter)
    order = {"a": 0, "s": 0, "v": 1, "n": 2, "r": 3}             # only breaks ties in the number of senses
    for k, lst in entries.items():
        senses, syn, ant, heads = [], [], [], []
        # the part of speech with most senses is the commonest use ("fast": adjective, not "a fast")
        for gi, (pos, ss, w) in enumerate(sorted(lst, key=lambda x: (-len(x[1]), order.get(x[0], 9)))):
            add_unique(heads, w, 3)
            for sid in ss[: (3 if gi == 0 else 1)]:
                if len(senses) < SENSES and synset_def.get(sid):
                    senses.append(f"{EN_POS.get(pos, '')}: {cut(synset_def[sid])}")
            for sid in ss[:3]:
                for other in synset_members.get(sid, []):
                    if norm_key(other) != k:
                        add_unique(syn, other)
            for tgt in sense_ants.get(w, []):
                if sense_lemma.get(tgt):
                    add_unique(ant, sense_lemma[tgt])
            for f in en_forms(w, pos):
                forms[f][w] += 1
        lex[k] = {"senses": senses, "syn": syn, "ant": ant, "heads": heads}
    for line in IRREGULAR.split("|"):
        ws = line.split()
        for f in ws[1:]:
            forms[f][ws[0]] += 5                               # an irregular form beats a rule's guess
    return lex, forms


# ---- assemble ----------------------------------------------------------------------------------------

def lex_rows(lex):
    rows = {}
    for k, e in lex.items():
        if not e["senses"] and not e["syn"]:
            continue
        head = " / ".join(e["heads"]) if any(h != k for h in e["heads"]) else ""   # "pero / però"
        rows[k] = "\t".join([" | ".join(e["senses"][:SENSES]), ", ".join(e["syn"]), ", ".join(e["ant"]), head])
    return rows


def form_rows(forms, lemma_keys, nk=norm_key):
    rows = {}
    for f, c in forms.items():
        k = nk(f)
        if not k or " " in k:
            continue
        lem = [l for l, _ in c.most_common() if nk(l) != k and nk(l) in lemma_keys][:3]
        if lem:
            rows[k] = ", ".join(lem)
    return rows


def build(args):
    for name in SOURCES:
        if not os.path.exists(os.path.join(CACHE, name)) and not name.startswith(("kaikki-es", "kaikki-fr", "kaikki-de")):
            sys.exit(f"missing {name}: run 'gen_dicts.py fetch' first")
    os.makedirs(OUT, exist_ok=True)
    counts = {}

    print("FreeDict ...")
    fd_it_en, fd_en_it = freedict("ita-eng"), freedict("eng-ita")
    print("English Wiktionary, Italian entries ...")
    forms_it, kk_it_en, kk_lemmas, weight = read_kaikki()
    print("Wikizionario ...")
    lex_it, iw_it_en = read_itwikt(forms_it)
    print("Open English WordNet ...")
    lex_en, forms_en = read_oewn()

    # IT -> EN: Wiktionary glosses (sense order), Wikizionario translations, then FreeDict.
    it_en = collections.defaultdict(list)
    for src in (kk_it_en, iw_it_en, fd_it_en):
        for w, lst in src.items():
            k = norm_key(w)
            for x in lst:
                if k:
                    add_unique(it_en[k], x, 6)
    # EN -> IT: FreeDict first, then the IT->EN glosses inverted (early senses and items rank first).
    inv = collections.defaultdict(dict)
    for w, lst in kk_it_en.items():
        if w not in kk_lemmas:
            continue
        for rank, item in enumerate(lst[:4]):
            e = re.sub(r"^(to|a|an|the)\s+", "", item, flags=re.I)
            k = norm_key(e)
            if k and len(k.split()) <= 3:
                inv[k][w] = min(inv[k].get(w, 99), rank)
    # One score per Italian candidate: two sources agreeing (FreeDict lists it AND it is an early Wiktionary
    # gloss) beats either alone; then how early the gloss/listing is; then how much the word is used.
    en_it = {}
    for k in set(inv) | {norm_key(w) for w in fd_en_it}:
        if not k:
            continue
        fd = []
        for w, lst in ((w, fd_en_it[w]) for w in (k,) if w in fd_en_it):
            fd = [x for x in lst if norm_key(x) != k]
        score = collections.Counter()
        for i, x in enumerate(fd):
            score[x] += 6 - min(i, 5)
        for w, r in inv.get(k, {}).items():
            if norm_key(w) == k:                         # "go" -> "go" (the board game) says nothing
                continue
            score[w] += (8 if w in fd else 0) + (6 - 2 * r) + min(weight[w], 6)
        best = [w for w, _ in sorted(score.items(), key=lambda x: (-x[1], len(x[0])))][:6]
        if best:
            en_it[k] = best
    counts["dict-it-en.tsv"] = write_tsv("dict-it-en.tsv", {k: ", ".join(v) for k, v in it_en.items()})
    counts["dict-en-it.tsv"] = write_tsv("dict-en-it.tsv", {k: ", ".join(v) for k, v in en_it.items()})
    counts["lex-it.tsv"] = write_tsv("lex-it.tsv", lex_rows(lex_it))
    counts["lex-en.tsv"] = write_tsv("lex-en.tsv", lex_rows(lex_en))
    it_lemma_keys = set(lex_it) | {norm_key(w) for w in kk_lemmas} | set(it_en)
    counts["forms-it.tsv"] = write_tsv("forms-it.tsv", form_rows(forms_it, it_lemma_keys))
    counts["forms-en.tsv"] = write_tsv("forms-en.tsv", form_rows(forms_en, set(lex_en) | set(en_it)))

    # Spanish, French, German <-> English (English Wiktionary): word -> glosses, and glosses inverted.
    for lc, name in (("es", "Spanish"), ("fr", "French"), ("de", "German")):
        fname = f"kaikki-{lc}-en.jsonl.gz"
        if not os.path.exists(os.path.join(CACHE, fname)):
            print(f"  (no {fname}: {name} dictionaries skipped)"); continue
        print(f"English Wiktionary, {name} entries ...")
        xforms, x_en, xlemmas, xweight = read_kaikki(fname, lc, xkey)
        rows = collections.defaultdict(list)
        for w, lst in x_en.items():
            k = xkey(w)
            for item in lst:
                if k:
                    add_unique(rows[k], item, 6)
        xinv = collections.defaultdict(dict)
        for w, lst in x_en.items():
            if w not in xlemmas:
                continue
            for rank, item in enumerate(lst[:4]):
                k = norm_key(re.sub(r"^(to|a|an|the)\s+", "", item, flags=re.I))
                if k and len(k.split()) <= 3:
                    xinv[k][w] = min(xinv[k].get(w, 99), rank)
        en_x = {}
        for k, cand in xinv.items():
            best = sorted(((w, (6 - 2 * r) + min(xweight[w], 6)) for w, r in cand.items() if xkey(w) != k),
                          key=lambda x: (-x[1], len(x[0])))[:6]
            if best:
                en_x[k] = [w for w, _ in best]
        counts[f"dict-{lc}-en.tsv"] = write_tsv(f"dict-{lc}-en.tsv", {k: ", ".join(v) for k, v in rows.items()})
        counts[f"dict-en-{lc}.tsv"] = write_tsv(f"dict-en-{lc}.tsv", {k: ", ".join(v) for k, v in en_x.items()})
        lemma_keys = {xkey(w) for w in xlemmas} | set(rows)
        counts[f"forms-{lc}.tsv"] = write_tsv(f"forms-{lc}.tsv", form_rows(xforms, lemma_keys, xkey))

    with open(os.path.join(OUT, "DICTIONARIES.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write(f"ANIMA offline dictionaries — built {datetime.date.today()} by tools/dicts/gen_dicts.py\n\n")
        for name, (url, what, lic) in SOURCES.items():
            f.write(f"- {what}\n  {url}\n  licence: {lic}\n")
        f.write("\nlex-en.tsv and forms-en.tsv derive from Open English WordNet (CC BY 4.0, "
                "https://en-word.net). Every other file derives from Wiktionary / WikDict and is shared under "
                "CC BY-SA 4.0 (https://creativecommons.org/licenses/by-sa/4.0/).\n\n")
        for k, v in counts.items():
            f.write(f"{k}: {v} keys\n")
    print("done:", OUT)


# ---- check: the firmware's lookup, in Python -----------------------------------------------------------

def lookup(path, key):
    """Same bisect as anima_dict_get: proves the files are sorted the way the firmware needs."""
    with open(path, "rb") as f:
        f.seek(0, 2); hi = f.tell(); lo = 0
        kb = key.encode()
        while hi - lo > 4096:
            mid = lo + (hi - lo) // 2
            f.seek(mid); f.readline()
            ls = f.tell()
            line = f.readline()
            if ls >= hi or not line:
                hi = mid; continue
            k = line.split(b"\t", 1)[0]
            if k < kb: lo = f.tell()
            else: hi = mid
        f.seek(lo)
        scanned = 0
        while scanned < 3 * 2048 + 8192:
            line = f.readline()
            if not line: break
            scanned += len(line)
            k, _, v = line.rstrip(b"\n").partition(b"\t")
            if k == kb: return v.decode()
            if k > kb: break
    return None


def check(_args):
    probes = [("dict-es-en.tsv", "perro"), ("dict-en-es.tsv", "dog"), ("dict-fr-en.tsv", "chien"),
              ("dict-de-en.tsv", "hund"), ("forms-es.tsv", "perros"), ("forms-de.tsv", "ging"),
              ("dict-it-en.tsv", "cane"), ("dict-it-en.tsv", "andare"), ("dict-en-it.tsv", "dog"),
              ("dict-en-it.tsv", "go"), ("lex-it.tsv", "effimero"), ("lex-it.tsv", "veloce"),
              ("lex-en.tsv", "ephemeral"), ("lex-en.tsv", "fast"), ("forms-it.tsv", "andavo"),
              ("forms-it.tsv", "case"), ("forms-en.tsv", "went"), ("forms-en.tsv", "dogs")]
    bad = 0
    for name, key in probes:
        v = lookup(os.path.join(OUT, name), key)
        print(f"  {name:15} {key:10} -> {(v or 'MISSING')[:150]}")
        bad += v is None
    for name in sorted(f for f in os.listdir(OUT) if f.endswith(".tsv")):
        prev, n = b"", 0
        with open(os.path.join(OUT, name), "rb") as f:
            for line in f:
                k = line.split(b"\t", 1)[0]
                if k <= prev or len(line) >= MAX_LINE + 1:
                    print(f"  {name}: order/length broken at {k[:40]!r}"); bad += 1; break
                prev = k; n += 1
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["fetch", "build", "check"])
    a = ap.parse_args()
    {"fetch": lambda a: fetch(), "build": build, "check": check}[a.cmd](a)
