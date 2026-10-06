#!/usr/bin/env python3
"""ANIMA offline dictionaries: download the sources and build the sorted TSV files the firmware reads.

    python tools/dicts/gen_dicts.py fetch            download the sources into tools/dicts/.cache
    python tools/dicts/gen_dicts.py build            build sd/data/anima/*.tsv (then tools/sync-sd.ps1)
    python tools/dicts/gen_dicts.py check            look words up the way the firmware does
    python tools/dicts/gen_dicts.py publish          LICENSE-dict-*.txt + store rows dict-{it,en,es,fr,de}
                                                     (server/appstore/data_packs.json), prints the release steps

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
import math
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
    "freq-it-50k.txt": ("https://raw.githubusercontent.com/hermitdave/FrequencyWords/master/content/2018/it/it_50k.txt",
                        "FrequencyWords, Italian 50k (OpenSubtitles 2018), Hermit Dave: ranks EN -> IT translations",
                        "CC BY-SA 4.0"),
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
# Senses that never become an EN -> IT translation: a dictionary answering "dog" says cane, not the humorous
# "loppide", the poetic "can", the dialectal "pene" (bread) or the relational adjective "idrico" (water).
NOISE_TAGS = {"slang", "vulgar", "offensive", "derogatory", "pejorative", "humorous", "jocular", "literary",
              "poetic", "apocopic", "dialectal", "regional", "childish", "relational", "euphemistic", "ironic",
              "taboo", "blasphemous"}
# A word with a sense tagged like this is kept from FreeDict's lists only where a clean sense says the key.
OFFENSIVE_TAGS = {"vulgar", "offensive", "derogatory", "pejorative", "taboo", "blasphemous"}
FUNCTION_POS = {"prep", "article", "det", "pron", "conj", "particle", "prefix", "suffix", "interfix", "character",
                "symbol", "punct", "contraction", "num"}
KK_POS = {"noun": "n", "verb": "v", "adj": "a", "adv": "r"}
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
    """English Wiktionary entries of one language: forms -> lemma, word -> English glosses, lemmas, usage,
    and word -> (sense index, english) over more senses, for the EN -> word inversion."""
    forms = collections.defaultdict(collections.Counter)      # form key -> Counter(lemma)
    it_en = collections.OrderedDict()                          # word -> [english]
    deep = collections.defaultdict(list)                       # word -> [(sense index, english)]
    clean = collections.defaultdict(list)                      # word -> [(sense index, english)], no NOISE_TAGS
    marked = collections.defaultdict(list)                     # word -> [(english, relational?)], NOISE_TAGS senses
    support = collections.defaultdict(set)                     # word -> keys its clean senses translate to
    offensive, pos = set(), collections.defaultdict(set)       # words with an offensive sense; word -> POS
    pos_raw = collections.defaultdict(set)                     # word -> Wiktionary parts of speech, as written
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
        meaning = False                                        # a sense of its own, not only "form of"
        nsense = nclean = 0
        pos_raw[w].add(d.get("pos"))
        if KK_POS.get(d.get("pos")):
            pos[w].add(KK_POS[d["pos"]])
        for s in d.get("senses", []):
            fo = s.get("form_of") or []
            if fo:
                is_form = True
                for x in fo:
                    if x.get("word"):
                        forms[nk(w)][x["word"]] += 1
                continue
            stags = set(s.get("tags", []))
            if stags & OFFENSIVE_TAGS:
                offensive.add(w)
            if SKIP_TAGS & stags:
                continue
            meaning = meaning or bool(s.get("glosses"))
            for g in s.get("glosses", [])[:1]:
                items = gloss_items(g)
                # Translations lead a gloss; a description ends the list: "chimaera, a kind of shark of the
                # genus..." gives chimaera, "native or inhabitant of the region of Veneto, Italy" gives nothing.
                lead = []
                for x in GLOSS_SPLIT.split(PAREN.sub("", g)):
                    if len(x.split()) > 5:
                        break
                    lead.append(norm_key(x))
                inv_items = [x for x in items if norm_key(x) in lead]
                if stags & NOISE_TAGS:                             # a fallback: "alacritous" -> alacre (literary)
                    if nclean < 4:
                        marked[w] += [(item, "relational" in stags) for item in inv_items[:2]]
                    continue
                support[w].update(norm_key(re.sub(r"^(to|a|an|the)\s+", "", x, flags=re.I)) for x in items)
                if nclean < (4 if first_entry else 1):
                    for item in inv_items[:3]:
                        clean[w].append((nclean, item))
            if not stags & NOISE_TAGS:
                nclean += 1
            # "Katze": house cat | female house cat | cat (any member of the genus Felis). Only the first
            # senses make the displayed translation, but "cat" -> Katze needs the third one.
            if first_entry and 2 <= nsense < 5:                # the senses past the displayed ones
                for g in s.get("glosses", [])[:1]:
                    for item in gloss_items(g)[:2]:
                        deep[w].append((nsense, item))
            if nsense >= (2 if first_entry else 1):
                nsense += 1
                continue
            nsense += 1
            for g in s.get("glosses", [])[:1]:
                for item in gloss_items(g)[: (3 if first_entry else 1)]:
                    add_unique(it_en.setdefault(w, []), item, 6)
        # "música" is music AND the feminine of "músico": a word with a meaning of its own is a lemma.
        if meaning or not is_form:
            lemmas.add(w)
            for f in d.get("forms", []):                       # conjugation / plural tables of the lemma
                tags = set(f.get("tags", []))
                if tags & {"canonical", "romanization", "table-tags", "inflection-template"}:
                    continue
                plain = (f.get("links") or [[None, f.get("form")]])[0][1] or f.get("form")
                k = nk(plain or "")
                if k and k != nk(w) and len(k.split()) == 1:
                    forms[k][w] += 1
    return forms, it_en, lemmas, weight, deep, {"clean": clean, "marked": marked, "support": support, "offensive": offensive,
                                                 "pos": pos, "pos_raw": pos_raw}


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
    en_pos = {}                                                # key -> its commonest part of speech (n v a r)
    en_proper = set()                                          # keys WordNet writes capitalised ("Italy")
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
        n = collections.Counter()
        for p_, ss, _w in lst:
            n["a" if p_ == "s" else p_] += len(ss)
        en_pos[k] = n.most_common(1)[0][0]
        if any(_w[:1].isupper() for _p, _s, _w in lst):
            en_proper.add(k)
    for line in IRREGULAR.split("|"):
        ws = line.split()
        for f in ws[1:]:
            forms[f][ws[0]] += 5                               # an irregular form beats a rule's guess
    return lex, forms, en_pos, en_proper


def read_freq():
    """Italian word -> frequency rank (1 = commonest), FrequencyWords 50k from OpenSubtitles."""
    rank = {}
    with open(os.path.join(CACHE, "freq-it-50k.txt"), encoding="utf-8") as f:
        for i, line in enumerate(f, 1):
            w = line.split(" ", 1)[0].strip().lower()
            if w and w not in rank:
                rank[w] = i
    return rank


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


def en_it_rows(fd_en_it, kk, kk_lemmas, en_pos, en_proper, forms_it, freq):
    """EN key -> ranked Italian translations (dict-en-it.tsv)."""
    # EN -> IT: FreeDict, then the IT->EN glosses of the clean senses inverted (earlier senses rank first).
    inv = collections.defaultdict(dict)
    for w, lst in kk["clean"].items():
        if w not in kk_lemmas:
            continue
        for rank, item in lst:
            e = re.sub(r"^(to|a|an|the)\s+", "", item, flags=re.I)
            k = norm_key(e)
            if k and len(k.split()) <= 3:
                inv[k][w] = min(inv[k].get(w, 99), rank)

    # Senses Wiktionary marks (literary, slang, relational...) only answer a key nothing else translates; a
    # relational adjective only an adjective ("anatomic" -> anatomico, never "water" -> idrico).
    inv_m = collections.defaultdict(dict)
    for w, lst in kk["marked"].items():
        if w not in kk_lemmas:
            continue
        for item, relational in lst:
            k = norm_key(re.sub(r"^(to|a|an|the)\s+", "", item, flags=re.I))
            if k and len(k.split()) <= 3:
                inv_m[k][w] = relational

    def frank(w):                                      # a phrase is as common as its rarest word
        r = [freq.get(t) for t in norm_key(w).split()]
        return max(r) if r and None not in r else None

    # One score per Italian candidate: two sources agreeing (FreeDict lists it AND it is an early clean
    # Wiktionary gloss) beats either alone; then how early the gloss/listing is; then how common the word is
    # (OpenSubtitles frequency: "chair" -> sedia before presiedere) and whether it is the English word's
    # commonest part of speech. When common words exist, the rare ones are left out.
    fd_by_key = collections.defaultdict(list)          # FreeDict headwords keep capitals: "Italy" -> Italia
    fd_caps = set()                                    # keys FreeDict writes capitalised: their names may be too
    for w, lst in fd_en_it.items():
        if norm_key(w) and w[:1].isupper():
            fd_caps.add(norm_key(w))
        for x in lst:
            if norm_key(w):
                add_unique(fd_by_key[norm_key(w)], x, 12)
    en_it = {}
    for k in set(inv) | set(fd_by_key) | set(inv_m):
        if not k:
            continue
        fd = [x for x in fd_by_key.get(k, []) if norm_key(x) != k]
        same = [x for x in fd_by_key.get(k, []) + list(inv.get(k, {})) if norm_key(x) == k]
        score = collections.Counter()
        for i, x in enumerate(fd):
            score[x] += 6 - min(i, 5)
        for w, r in inv.get(k, {}).items():
            if norm_key(w) == k:                         # "go" -> "go" (the board game) says nothing
                continue
            score[w] += (8 if w in fd else 0) + (6 - 2 * r)
        for w in score:
            fr = frank(w)
            score[w] += (10 * (1 - math.log(fr) / math.log(50001)) if fr else 0) + \
                (3 if en_pos.get(k) in kk["pos"].get(w, ()) else 0)
        if not score:
            for w, relational in inv_m.get(k, {}).items():
                if norm_key(w) != k and (not relational or en_pos.get(k) == "a"):
                    score[w] = 0
        if not score and same:                           # "acne" -> acne: the same word, when nothing else
            score[same[0]] = 0
        ranked = [w for w, _ in sorted(score.items(), key=lambda x: (-x[1], len(x[0])))]
        # A word with an offensive sense somewhere ("bike" -> puttana, "german" -> frocio) only when a clean
        # sense of it says this key AND FreeDict agrees (red -> rosso, bird -> uccello), or nothing else does.
        fd_low = {x.lower() for x in fd}

        def decent(w):                                 # a phrase is as offensive as its words ("porco Dio")
            if not any(t in kk["offensive"] for t in [w] + w.lower().split()):
                return True
            return k in kk["support"].get(w, ()) and w.lower() in fd_low
        if any(decent(w) for w in ranked):
            ranked = [w for w in ranked if decent(w)]
        kept_norms, seen, keep = set(), set(), []
        for w in ranked:
            lw = w.lower()
            if w.startswith("-") or w.endswith("-") or lw in seen:   # affixes ("mal-"), "Dottore" after "dottore"
                continue
            if w != lw and k in en_pos and k not in en_proper and k not in fd_caps:
                continue                                             # capitals only for names: Italy -> Italia
            if (w != lw or " " in w) and w not in kk["pos"] and lw not in kk["pos"] and norm_key(w) in en_pos:
                continue                                             # English left in a list ("Art Night")
            known = kk["pos_raw"].get(w) or kk["pos_raw"].get(lw)
            if known and known <= FUNCTION_POS and k in en_pos:
                continue                                             # "de" (dialectal "of") for "rome"
            if " " not in w and w == lw and not known and                     any(kk["pos_raw"].get(x) or kk["pos_raw"].get(x.lower()) for x in ranked):
                continue                                             # FreeDict-only "silvio" for "english"
            lem = forms_it.get(norm_key(w), {})
            if any(norm_key(l) in kept_norms and norm_key(l) != norm_key(w) for l in lem):
                continue                                 # "cuori" after "cuore", "diciamo" after "dire" (not
            kept_norms.add(norm_key(w))                  # "zucchero" after nothing: it is also "io zucchero")
            seen.add(lw)
            keep.append(w)
        ranked = keep
        # Rare words go when common ones exist, unless both sources agree on them (ephemeral -> effimero).
        if any(frank(w) for w in ranked):
            ranked = [w for w in ranked if frank(w) or (w in fd and w in inv.get(k, {}))]
        if ranked:
            en_it[k] = ranked[:5]
    return en_it


def build(args):
    for name in SOURCES:
        if not os.path.exists(os.path.join(CACHE, name)) and not name.startswith(("kaikki-es", "kaikki-fr", "kaikki-de")):
            sys.exit(f"missing {name}: run 'gen_dicts.py fetch' first")
    os.makedirs(OUT, exist_ok=True)
    counts = {}

    print("FreeDict ...")
    fd_it_en, fd_en_it = freedict("ita-eng"), freedict("eng-ita")
    print("English Wiktionary, Italian entries ...")
    forms_it, kk_it_en, kk_lemmas, weight, _, kk = read_kaikki()
    print("Wikizionario ...")
    lex_it, iw_it_en = read_itwikt(forms_it)
    print("Open English WordNet ...")
    lex_en, forms_en, en_pos, en_proper = read_oewn()
    freq = read_freq()

    # IT -> EN: Wiktionary glosses (sense order), Wikizionario translations, then FreeDict.
    it_en = collections.defaultdict(list)
    for src in (kk_it_en, iw_it_en, fd_it_en):
        for w, lst in src.items():
            k = norm_key(w)
            for x in lst:
                if k:
                    add_unique(it_en[k], x, 6)
    en_it = en_it_rows(fd_en_it, kk, kk_lemmas, en_pos, en_proper, forms_it, freq)
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
        xforms, x_en, xlemmas, xweight, xdeep, _ = read_kaikki(fname, lc, xkey)
        rows = collections.defaultdict(list)
        for w, lst in x_en.items():
            k = xkey(w)
            for item in lst:
                if k:
                    add_unique(rows[k], item, 6)
        # Inverted: the displayed glosses ranked by position. The later senses (xdeep) only fill an English
        # word nothing else translates ("cat" -> Katze), never outrank a first sense ("queen" stays Königin).
        xinv = collections.defaultdict(dict)
        for deep_pass, pairs in ((False, [(w, list(enumerate(lst[:4]))) for w, lst in x_en.items()]),
                                 (True, list(xdeep.items()))):
            direct = set(xinv)
            for w, lst in pairs:
                if w not in xlemmas:
                    continue
                for rank, item in lst:
                    k = norm_key(re.sub(r"^(to|a|an|the)\s+", "", item, flags=re.I))
                    if k and len(k.split()) <= 3 and not (deep_pass and k in direct):
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
    # EN -> IT quality: the plain word first, never the slang/vulgar/dialectal/relational noise of the sources.
    first = {"dog": "cane", "cat": "gatto", "chair": "sedia", "door": "porta", "say": "dire", "tell": "dire",
             "water": "acqua", "red": "rosso", "bird": "uccello", "cow": "vacca", "sun": "sole", "bread": "pane",
             "italy": "Italia", "rome": "Roma", "german": "tedesco", "ephemeral": "effimero", "slow": "lento"}
    never = {"dog": ["can", "loppide"], "bike": ["puttana"], "queen": ["checca"], "water": ["acqueo", "idrico"],
             "bread": ["pene"], "german": ["frocio"], "christ": ["porco Dio"], "ship": ["-ato"],
             "evening": ["Art Night"], "english": ["silvio"], "rome": ["de"], "heart": ["cuori"]}
    for key in sorted(set(first) | set(never)):
        v = lookup(os.path.join(OUT, "dict-en-it.tsv"), key) or ""
        got = [x.strip() for x in v.split(",")]
        ok = (key not in first or got[0] == first[key]) and not set(never.get(key, ())) & set(got)
        if not ok:
            print(f"  dict-en-it.tsv  {key:10} -> {v}   QUALITY: want {first.get(key)} first, never {never.get(key)}")
            bad += 1
    for name in sorted(f for f in os.listdir(OUT) if f.endswith(".tsv")):
        prev, n = b"", 0
        with open(os.path.join(OUT, name), "rb") as f:
            for line in f:
                k = line.split(b"\t", 1)[0]
                if k <= prev or len(line) >= MAX_LINE + 1:
                    print(f"  {name}: order/length broken at {k[:40]!r}"); bad += 1; break
                prev = k; n += 1
    sys.exit(1 if bad else 0)


# ---- store packs ---------------------------------------------------------------------------------------
# One store pack per language, ids shared with the system content packs (docs/CONTENT_PACKS_PLAN.md), so a
# v2 install finds these very files already in place. Each file lands in /sdcard/data/anima (dest "anima").

STORE_REPO = "indecenti/nucleoos-p4-store"
DATA_PACKS = os.path.join(ROOT, "server", "appstore", "data_packs.json")
PACK_FILES = {
    "dict-it": ["lex-it.tsv", "forms-it.tsv", "dict-it-en.tsv", "dict-en-it.tsv"],
    "dict-en": ["lex-en.tsv", "forms-en.tsv"],
    "dict-es": ["dict-es-en.tsv", "dict-en-es.tsv", "forms-es.tsv"],
    "dict-fr": ["dict-fr-en.tsv", "dict-en-fr.tsv", "forms-fr.tsv"],
    "dict-de": ["dict-de-en.tsv", "dict-en-de.tsv", "forms-de.tsv"],
}
PACK_SOURCES = {                    # SOURCES keys each pack derives from (its LICENSE file lists them)
    "dict-it": ["kaikki-it-en.jsonl.gz", "itwiktionary.jsonl.gz", "freedict-ita-eng.src.tar.xz",
                "freedict-eng-ita.src.tar.xz", "freq-it-50k.txt"],
    "dict-en": ["oewn-2025.xml.gz"],
    "dict-es": ["kaikki-es-en.jsonl.gz"],
    "dict-fr": ["kaikki-fr-en.jsonl.gz"],
    "dict-de": ["kaikki-de-en.jsonl.gz"],
}
PACK_NAMES = {
    "dict-it": {"it": "Dizionario italiano", "en": "Italian dictionary", "es": "Diccionario italiano",
                "fr": "Dictionnaire italien", "de": "Italienisches Wörterbuch"},
    "dict-en": {"it": "Dizionario inglese", "en": "English dictionary", "es": "Diccionario inglés",
                "fr": "Dictionnaire anglais", "de": "Englisches Wörterbuch"},
    "dict-es": {"it": "Dizionario spagnolo", "en": "Spanish dictionary", "es": "Diccionario español",
                "fr": "Dictionnaire espagnol", "de": "Spanisches Wörterbuch"},
    "dict-fr": {"it": "Dizionario francese", "en": "French dictionary", "es": "Diccionario francés",
                "fr": "Dictionnaire français", "de": "Französisches Wörterbuch"},
    "dict-de": {"it": "Dizionario tedesco", "en": "German dictionary", "es": "Diccionario alemán",
                "fr": "Dictionnaire allemand", "de": "Deutsches Wörterbuch"},
}
PACK_DESC = {
    "dict-it": {
        "it": "ANIMA senza rete: definizioni, sinonimi, contrari e forme dell'italiano, traduzioni italiano-inglese "
              "(\"traduci cane in inglese\"). Serve anche per tradurre verso spagnolo, francese e tedesco. "
              "{mb} MB sulla SD.",
        "en": "ANIMA offline: Italian definitions, synonyms, opposites and word forms, plus Italian-English translation. "
              "Also needed to translate Italian into Spanish, French and German. {mb} MB on the SD.",
        "es": "ANIMA sin conexión: definiciones, sinónimos, contrarios y formas del italiano, y traducción "
              "italiano-inglés. {mb} MB en la SD.",
        "fr": "ANIMA hors ligne : définitions, synonymes, contraires et formes de l'italien, et traduction "
              "italien-anglais. {mb} Mo sur la carte SD.",
        "de": "ANIMA offline: italienische Bedeutungen, Synonyme, Gegenteile und Wortformen sowie Übersetzung "
              "Italienisch-Englisch. {mb} MB auf der SD-Karte.",
    },
    "dict-en": {
        "it": "ANIMA senza rete: definizioni, sinonimi, contrari e forme dell'inglese (\"what does ephemeral mean\"). "
              "L'inglese fa da ponte per le traduzioni tra le altre lingue: da installare insieme a quelli che usi. "
              "{mb} MB sulla SD.",
        "en": "ANIMA offline: English definitions, synonyms, opposites and word forms. English is the bridge for "
              "translating between the other languages: install it with the ones you use. {mb} MB on the SD.",
        "es": "ANIMA sin conexión: definiciones, sinónimos, contrarios y formas del inglés, puente para traducir "
              "entre los demás idiomas. {mb} MB en la SD.",
        "fr": "ANIMA hors ligne : définitions, synonymes, contraires et formes de l'anglais, langue pont pour traduire "
              "entre les autres langues. {mb} Mo sur la carte SD.",
        "de": "ANIMA offline: englische Bedeutungen, Synonyme, Gegenteile und Wortformen, die Brücke für "
              "Übersetzungen zwischen den anderen Sprachen. {mb} MB auf der SD-Karte.",
    },
}
for _id, _l in (("dict-es", {"it": "spagnolo", "it_of": "dello spagnolo", "en": "Spanish", "es": "español",
                             "fr": "espagnol", "fr_of": "de l'espagnol", "de": "Spanisch"}),
                ("dict-fr", {"it": "francese", "it_of": "del francese", "en": "French", "es": "francés",
                             "fr": "français", "fr_of": "du français", "de": "Französisch"}),
                ("dict-de", {"it": "tedesco", "it_of": "del tedesco", "en": "German", "es": "alemán",
                             "fr": "allemand", "fr_of": "de l'allemand", "de": "Deutsch"})):
    PACK_DESC[_id] = {
        "it": f"ANIMA senza rete: traduzioni {_l['it']}-inglese e forme {_l['it_of']} (\"traduci cane in {_l['it']}\"). "
              "Con il dizionario italiano traduce anche da e verso l'italiano. {mb} MB sulla SD.",
        "en": f"ANIMA offline: {_l['en']}-English translation and {_l['en']} word forms. With the Italian dictionary "
              "it also translates to and from Italian. {mb} MB on the SD.",
        "es": f"ANIMA sin conexión: traducción {_l['es']}-inglés y formas del {_l['es']}. {{mb}} MB en la SD.",
        "fr": f"ANIMA hors ligne : traduction {_l['fr']}-anglais et formes {_l['fr_of']}. {{mb}} Mo sur la carte SD.",
        "de": f"ANIMA offline: Übersetzung {_l['de']}-Englisch und {_l['de']}e Wortformen. {{mb}} MB auf der SD-Karte.",
    }


def sha256_of(path):
    import hashlib
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def keys_of(path):
    with open(path, "rb") as f:
        return sum(1 for _ in f)


def license_text(pid, names):
    lines = [f"{PACK_NAMES[pid]['en']} for ANIMA (NucleoOS) — store pack {pid}, built by tools/dicts/gen_dicts.py",
             "", "Sources:"]
    for key in PACK_SOURCES[pid]:
        url, what, lic = SOURCES[key]
        lines += [f"- {what}", f"  {url}", f"  licence: {lic}"]
    lines.append("")
    if pid == "dict-en":
        lines.append("lex-en.tsv and forms-en.tsv derive from Open English WordNet 2025 (https://en-word.net) and are "
                     "shared under CC BY 4.0 (https://creativecommons.org/licenses/by/4.0/).")
    else:
        lines.append("These files derive from Wiktionary / WikDict and are shared under CC BY-SA 4.0 "
                     "(https://creativecommons.org/licenses/by-sa/4.0/). Wiktionary text is also available under "
                     "the GFDL.")
    lines.append("")
    for n in names:
        lines.append(f"{n}: {keys_of(os.path.join(OUT, n))} keys")
    return "\n".join(lines) + "\n"


def publish(a):
    """Write LICENSE-<id>.txt next to the dictionaries and the store rows (kind "data") for each pack in
    server/appstore/data_packs.json. Other rows there (wiki-*) are kept. Nothing is uploaded: the release
    and store commands are printed at the end."""
    tag = a.tag or "dict-" + datetime.date.today().strftime("%Y.%m")
    version = a.version or "{0}.{1}".format(*map(int, datetime.date.today().strftime("%Y %m").split()))
    missing = [n for ns in PACK_FILES.values() for n in ns if not os.path.exists(os.path.join(OUT, n))]
    if missing:
        sys.exit(f"missing {', '.join(missing)}: run 'gen_dicts.py build' first")
    only = [x.strip() for x in a.packs.split(",") if x.strip()] if a.packs else list(PACK_FILES)
    unknown = [x for x in only if x not in PACK_FILES]
    if unknown:
        sys.exit(f"unknown pack(s): {', '.join(unknown)}")
    rows, uploads = [], []
    for pid, names in PACK_FILES.items():
        if pid not in only:
            continue
        lic = f"LICENSE-{pid}.txt"
        with open(os.path.join(OUT, lic), "w", encoding="utf-8", newline="\n") as f:
            f.write(license_text(pid, names))
        files = []
        for n in names + [lic]:
            p = os.path.join(OUT, n)
            files.append({"name": n, "size": os.path.getsize(p), "sha256": sha256_of(p),
                          "url": f"https://github.com/{STORE_REPO}/releases/download/{tag}/{n}"})
            uploads.append(p)
        mb = (sum(f["size"] for f in files) + (1 << 19)) >> 20
        rows.append({
            "id": pid, "version": version, "dest": "anima", "lang": pid[5:], "category": "knowledge",
            "author": "Wiktionary · WikDict/FreeDict · Open English WordNet",
            "license": "CC BY 4.0" if pid == "dict-en" else "CC BY-SA 4.0",
            "source": "https://github.com/indecenti/NucleoOS-P4/tree/main/tools/dicts",
            "names": PACK_NAMES[pid],
            "descriptions": {ul: d.format(mb=mb) for ul, d in PACK_DESC[pid].items()},
            "featured": pid in ("dict-it", "dict-en"),
            "files": files, "tag": tag,
        })
        print(f"  {pid}  v{version}  {mb} MB  {len(files)} files")
    with open(DATA_PACKS, encoding="utf-8") as f:
        doc = json.load(f)
    keep = [p for p in doc.get("packs", []) if p.get("id") not in only]
    order = {pid: i for i, pid in enumerate(PACK_FILES)}             # dict-* rows stay in PACK_FILES order
    doc["packs"] = sorted(keep + rows, key=lambda p: (p["id"] in order, order.get(p["id"], 0)))
    with open(DATA_PACKS, "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, ensure_ascii=False, indent=2)
        f.write("\n")
    print(f"-> {os.path.relpath(DATA_PACKS, ROOT)}  ({len(keep)} other + {len(rows)} dictionary packs)")
    print("\nTo publish (run them yourself when ready):")
    print(f"  gh release create {tag} --repo {STORE_REPO} --title \"ANIMA dictionaries {tag}\" \\")
    print("     --notes \"Wiktionary / WikDict (CC BY-SA 4.0), Open English WordNet (CC BY 4.0): ANIMA offline "
          "dictionaries.\" \\")
    print("     " + " ".join(os.path.relpath(u, ROOT) for u in uploads))
    print("  python server/appstore/export_static.py --out <checkout of the store repo>   # signs data/<id>/pack.sig")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["fetch", "build", "check", "publish"])
    ap.add_argument("--tag", default="", help="publish: GitHub release tag (default dict-YYYY.MM)")
    ap.add_argument("--version", default="", help="publish: pack version (default YYYY.M)")
    ap.add_argument("--out", default="", help="the dictionaries' folder (default sd/data/anima)")
    ap.add_argument("--packs", default="", help="publish: only these packs, e.g. dict-it (default all)")
    a = ap.parse_args()
    if a.out:
        OUT = os.path.abspath(a.out)
    {"fetch": lambda a: fetch(), "build": build, "check": check, "publish": publish}[a.cmd](a)
