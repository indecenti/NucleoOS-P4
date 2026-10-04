"""Expand tools/anima_phrases.txt into the ANIMA L0 paraphrase table and its host tests.

  python tools/gen_anima_phrases.py           regenerate both outputs
  python tools/gen_anima_phrases.py --check   fail if they are stale (CI / before a commit)

Outputs (generated, committed):
  components/nv_anima/anima_phrases.c          sorted "lang:key" -> canonical table + the filler words
  tests/host/unit/anima_phrases_cases.h        every paraphrase, for tests/host/unit/test_anima_nl.cpp

The normalization mirrors a_tokenize() in nucleo_anima.c: lowercase, Italian accents folded to the base
vowel, every other non [a-z0-9] character a separator; then the filler words are dropped. The filler
list is emitted into the C file, so the device and this script can never disagree on it.
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "tools", "anima_phrases.txt")
SRC_XL = os.path.join(ROOT, "tools", "anima_phrases_xl.txt")   # Spanish, French, German -> English canonicals
SRC_HA = os.path.join(ROOT, "tools", "anima_phrases_ha.txt")   # imported (tools/import_ha_intents.py): ours win
OUT_C = os.path.join(ROOT, "components", "nv_anima", "anima_phrases.c")
OUT_T = os.path.join(ROOT, "tests", "host", "unit", "anima_phrases_cases.h")
MAX_PER_TEMPLATE = 400

FOLD = str.maketrans("àáâèéêìíîòóôùúû", "aaaeeeiiiooouuu")
LANGS = ("it", "en", "es", "fr", "de")
XLANGS = ("es", "fr", "de")      # understood through English: their groups' canonical is an ENGLISH phrase

# anima_lang_fold() in components/nv_anima/anima_lang.c: the Latin letters of Spanish, French and German
# fold to ASCII before anything else reads the text. Mirrored char for char (not unicodedata: the device
# has no such table, and the two must agree).
XFOLD = {}
for src, dst in (("àáâãäå", "a"), ("ç", "c"), ("èéêë", "e"), ("ìíîï", "i"), ("ñ", "n"), ("òóôõöø", "o"),
                 ("ùúûü", "u"), ("ýÿ", "y")):
    for ch in src:
        XFOLD[ch] = dst
        XFOLD[ch.upper()] = dst
XFOLD.update({"ß": "ss", "æ": "ae", "Æ": "ae", "œ": "oe", "Œ": "oe"})


def xfold(s):
    return "".join(XFOLD.get(ch, ch) for ch in s)


def tokens(s, lang="it"):
    if lang in XLANGS:
        s = xfold(s)
    return re.findall(r"[a-z0-9]+", s.lower().translate(FOLD))


# ── template expansion: (a|b) one of, [a|b] optional, nesting allowed ──
def parse_seq(s, i, stop):
    """Sequence until one of `stop` chars; returns (list of alternatives-strings, index)."""
    outs = [""]
    while i < len(s) and s[i] not in stop:
        c = s[i]
        if c in "([":
            alts, i = parse_alts(s, i + 1, ")" if c == "(" else "]")
            if c == "[":
                alts = alts + [""]
            outs = [o + a for o in outs for a in alts]
        else:
            outs = [o + c for o in outs]
            i += 1
        if len(outs) > MAX_PER_TEMPLATE:
            raise ValueError("template expands to more than %d phrases: %s" % (MAX_PER_TEMPLATE, s))
    return outs, i


def parse_alts(s, i, close):
    alts = []
    while True:
        seq, i = parse_seq(s, i, "|" + close)
        alts += seq
        if i >= len(s):
            raise ValueError("unclosed group in: " + s)
        if s[i] == close:
            return alts, i + 1
        i += 1  # '|'


def expand(t):
    outs, i = parse_seq(t, 0, "")
    return [" ".join(o.split()) for o in outs if o.strip()]


def load(path, fillers):
    """Groups of one phrase file: [(lang, canonical, [(line, template)])]. `fillers` is filled in place."""
    groups = []
    for n, raw in enumerate(open(path, encoding="utf-8"), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        m = re.match(r"fillers (it|en|es|fr|de):\s*(.*)$", line)
        if m:
            fillers[m.group(1)] = m.group(2).split()
            continue
        m = re.match(r"=\s*(it|en|es|fr|de)\s+(.+)$", line)
        if m:
            groups.append((m.group(1), m.group(2).strip(), []))
            continue
        if not groups:
            sys.exit("%s:%d: template before the first '= lang canonical' line" % (path, n))
        groups[-1][2].append((n, line))
    return groups


def build():
    fillers = {l: [] for l in LANGS}
    sources = [(SRC, load(SRC, fillers), True)]
    if os.path.exists(SRC_XL):
        sources.append((SRC_XL, load(SRC_XL, fillers), True))
    if os.path.exists(SRC_HA):
        sources.append((SRC_HA, load(SRC_HA, {l: [] for l in LANGS}), False))
    fset = {l: set(tokens(" ".join(v), l)) for l, v in fillers.items()}
    key = lambda lang, s: " ".join(t for t in tokens(s, lang) if t not in fset[lang])
    # Every canonical is a phrase L0 already owns: no template may point it at another canonical.
    owned = {}
    for path, groups, _ in sources:
        for lang, canon, _t in groups:
            if lang in XLANGS:
                continue                # an English canonical: owned by the English groups, not this language
            ck = key(lang, canon)
            if not ck:
                sys.exit("%s: canonical '%s' is only filler words" % (path, canon))
            owned.setdefault(lang + ":" + ck, canon)
    table = {}    # "it:key" -> canonical
    cases = []    # (lang, surface, canonical)
    skipped = 0   # imported phrases dropped because ours already say otherwise
    for path, groups, strict in sources:
        for lang, canon, temps in groups:
            ck = None if lang in XLANGS else key(lang, canon)
            for n, t in temps:
                try:
                    phrases = expand(t) if strict else [t]   # imported sentences are literal
                except ValueError as e:
                    sys.exit("%s:%d: %s" % (path, n, e))
                for p in phrases:
                    k = key(lang, p)
                    if not k or k == ck:
                        continue  # all filler, or the canonical itself: nothing to rewrite
                    full = lang + ":" + k
                    clash = table.get(full, owned.get(full, canon))
                    if clash != canon:
                        if strict:
                            sys.exit("%s:%d: '%s' (%s) already means '%s', not '%s'" % (path, n, p, full, clash, canon))
                        skipped += 1
                        continue
                    if full not in table:
                        table[full] = canon
                        cases.append((lang, p, canon))
    return fillers, table, cases, skipped


def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def render(fillers, table, cases):
    head = ("// GENERATED by tools/gen_anima_phrases.py from tools/anima_phrases.txt (+ anima_phrases_xl.txt, anima_phrases_ha.txt) - do not edit.\n")
    c = [head,
         "// ANIMA L0 paraphrases: a normalized phrase (\"lang:tokens without fillers\") -> the canonical\n"
         "// phrase L0 already understands. Sorted for bsearch; read by l0_query() in nucleo_anima.c.\n",
         '#include "anima_phrases.h"\n#include <stdlib.h>\n#include <string.h>\n\n',
         "typedef struct { const char *key; const char *canon; } anima_phrase_t;\n\n"]
    for lang in LANGS:
        words = sorted(set(tokens(" ".join(fillers[lang]), lang)))
        c.append("static const char *const k_fill_%s[] = { %s };\n" % (lang, ", ".join([cstr(w) for w in words] + ["NULL"])))
    c.append("\nstatic const anima_phrase_t k_phrases[] = {\n")
    for k in sorted(table):
        c.append("    { %s, %s },\n" % (cstr(k), cstr(table[k])))
    c.append("};\n\n")
    c.append(
        "bool anima_phrase_is_filler_lang(const char *lang, const char *w)\n{\n"
        "    const char *const *f = k_fill_it;\n"
        "    if (lang && !strcmp(lang, \"en\")) f = k_fill_en;\n"
        "    else if (lang && !strcmp(lang, \"es\")) f = k_fill_es;\n"
        "    else if (lang && !strcmp(lang, \"fr\")) f = k_fill_fr;\n"
        "    else if (lang && !strcmp(lang, \"de\")) f = k_fill_de;\n"
        "    for (; *f; f++) if (!strcmp(*f, w)) return true;\n"
        "    return false;\n}\n\n"
        "bool anima_phrase_is_filler(bool en, const char *w) { return anima_phrase_is_filler_lang(en ? \"en\" : \"it\", w); }\n\n"
        "static int cmp(const void *k, const void *e) { return strcmp((const char *)k, ((const anima_phrase_t *)e)->key); }\n\n"
        "const char *anima_phrase_lookup(const char *key)\n{\n"
        "    const anima_phrase_t *p = (const anima_phrase_t *)bsearch(key, k_phrases, sizeof k_phrases / sizeof k_phrases[0],\n"
        "                                                           sizeof k_phrases[0], cmp);\n"
        "    return p ? p->canon : NULL;\n}\n\n"
        "int anima_phrase_count(void) { return (int)(sizeof k_phrases / sizeof k_phrases[0]); }\n")
    t = [head, "// Every paraphrase with its canonical: test_anima_nl.cpp checks each answers like the canonical.\n",
         "#pragma once\n\n// lang: the phrase's language; its canonical is in the same language, or English for es/fr/de.\n"
         "struct PhraseCase { const char *lang; const char *phrase; const char *canon; };\n\n",
         "static const PhraseCase kPhraseCases[] = {\n"]
    for lang, p, canon in cases:
        t.append("    { %s, %s, %s },\n" % (cstr(lang), cstr(p), cstr(canon)))
    t.append("};\n")
    return "".join(c), "".join(t)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="fail if the generated files are stale")
    a = ap.parse_args()
    fillers, table, cases, skipped = build()
    out_c, out_t = render(fillers, table, cases)
    stale = []
    for path, body in ((OUT_C, out_c), (OUT_T, out_t)):
        old = open(path, encoding="utf-8").read() if os.path.exists(path) else None
        if old != body:
            stale.append(path)
            if not a.check:
                with open(path, "w", encoding="utf-8", newline="\n") as f:
                    f.write(body)
    if a.check and stale:
        sys.exit("stale: " + ", ".join(os.path.relpath(p, ROOT) for p in stale) + " (run tools/gen_anima_phrases.py)")
    print("%d paraphrases (%s), %d imported ones overridden by ours%s" % (len(table),
          ", ".join("%d %s" % (sum(k.startswith(l + ":") for k in table), l) for l in LANGS), skipped,
                                               "" if a.check else "; wrote " + ", ".join(os.path.relpath(p, ROOT) for p in stale) if stale else "; up to date"))


if __name__ == "__main__":
    main()
