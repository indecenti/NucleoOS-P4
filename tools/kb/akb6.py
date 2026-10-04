#!/usr/bin/env python3
"""AKB6 — ANIMA's knowledge pack: one file per pack, laid out for a microcontroller reading an SD card.

    python tools/kb/akb6.py build <wiki.zim> <out.akb6> [--limit N]   ZIM (Kiwix) -> AKB6
    python tools/kb/akb6.py ask   <pack.akb6> "<question or title>"   look it up as the device would
    python tools/kb/akb6.py eval  <pack.akb6> <wiki.zim> [--n 1000]   hit / wrong / abstain + bytes read

Layout (little endian). Every lookup is: bisect KEYS (~20 short reads), read 12 bytes of ENTS, read one
BLKS block (~32 KB of text before compression), inflate it (the ESP32 ROM has tinfl: raw deflate).

    header   "AKB6" u16 version u16 flags char lang[4] u32 n_ent u32 n_keys u32 n_blk u32 n_sec u32 0 char id[32]
    sections n_sec x { char tag[4], u64 offset, u64 size }
      META   UTF-8 JSON: source, licence, attribution, date, counts
      KEYS   "key<TAB>id[,id...]\\n" sorted by key bytes; key = the ANIMA tokenizer's normal form of a title,
             redirect, or a title without its "(qualifier)"; '~' before an id = that key is ambiguous for it;
             '^' = the name is only a SECTION of that entity ("Jimbo Kern" -> Personaggi di South Park);
             '@' = the entity's title in ANOTHER language (Wikidata sitelinks: "napoleon" -> Napoleone Bonaparte)
      QIDS   "Q<n><TAB>id\\n" sorted by bytes: the device moves to the user's language pack by Wikidata ID
      ENTS   n_ent x { u32 block, u32 offset, u32 length }   (the entity's record inside the inflated block)
      BIDX   n_blk x { u64 offset in file, u32 compressed size, u32 inflated size }
      BLKS   raw-deflate blocks
    record   UTF-8 fields joined by 0x1E: title, qid, path, geo, summary, passages (joined by 0x1F),
             facts ("born=1879-03-14|birthplace=Ulm|gender=m", Wikidata via tools/kb/facts.py; may be empty)

Licence: Wikipedia text is CC BY-SA 4.0; META carries the attribution the device shows with an answer.
"""
import argparse
import bisect
import html
import json
import os
import re
import struct
import sys
import time
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "dicts"))
from gen_dicts import tokens, XFOLD  # noqa: E402  the firmware's normalization, mirrored

VERSION = 1
BLOCK = 32 * 1024            # inflated bytes per block: one device read + inflate per answer
SUMMARY_MAX = 380           # the answer itself: what fits a screen and a spoken reply
SUMMARY_SENTENCES = 2
PASSAGE_MAX = 700
PASSAGES = 6
SEP_F, SEP_P = "\x1e", "\x1f"


def key_of(s):
    """anima_lang_fold (á ñ ü ß ... -> ASCII) then the tokenizer: the device folds a question the same way."""
    t = tokens("".join(XFOLD.get(ch, ch) for ch in s).lower())
    return " ".join(t) if 0 < len(t) <= 12 else ""


# ---- text out of the mini HTML --------------------------------------------------------------------
RE_SEC0 = re.compile(r'<section data-mw-section-id="0".*?</section>', re.S)
RE_P = re.compile(r"<p\b[^>]*>(.*?)</p>", re.S)
RE_DROP = re.compile(r'<style\b.*?</style>|<sup\b.*?</sup>|<span[^>]*class="[^"]*IPA[^"]*"[^>]*>.*?</span>'
                     r'|<span[^>]*class="[^"]*(?:noprint|mw-ref|reference)[^"]*"[^>]*>.*?</span>', re.S)
RE_TAG = re.compile(r"<[^>]+>")
RE_PRON = re.compile(r"\(\s*pronuncia\b[^;)]*;(?:\s*[^\W\d][^;(),]{0,20};)*\s*", re.I)   # "(pronuncia italiana ; tedesca ; "
RE_SECREF = re.compile(r"http-equiv=\"refresh\" content=\"0;URL='\./([^'#]+)#")   # a page that is a section of another
RE_GEO = re.compile(r'<meta name="geo.position" content="([-0-9.]+;[-0-9.]+)"')


def clean_par(h):
    t = RE_TAG.sub("", RE_DROP.sub("", h))
    t = html.unescape(t).replace(" ", " ")
    t = RE_PRON.sub("(", t)
    t = re.sub(r"\(\s*pronuncia[^()]{0,40}\)\s*", "", t, flags=re.I)   # "(pronuncia )": the IPA alone
    t = re.sub(r"\(\s*[;,]?\s*", "(", t)
    t = re.sub(r"\(\s*\)", "", t)
    t = re.sub(r"\s*\[\s*\]", "", t)                            # "Alderney []": the IPA was removed
    t = re.sub(r"\s+", " ", t).strip()
    t = re.sub(r"\s+([,.;:])", r"\1", t)
    return t


# The opening parenthesis of a lead ("(in latino …; pronuncia …; AFI: …; in greco …; Roma, 13 luglio 100 a.C. –
# Roma, 15 marzo 44 a.C.)") is mostly names in other scripts and phonetics: keep only its place/date parts.
NOISE = ("in latino", "in greco", "in tedesco", "in francese", "in inglese", "in spagnolo", "in russo", "in arabo",
         "in ebraico", "in cinese", "in giapponese", "afi", "pronuncia", "nelle epigrafi", "lett.", "latino:",
         "greco antico", "in lingua")


def tidy_lead(t):
    i = t.find("(")
    if i < 0 or i > 120:
        return t
    depth, j = 0, i
    for j in range(i, min(len(t), i + 900)):
        if t[j] == "(": depth += 1
        elif t[j] == ")":
            depth -= 1
            if depth == 0: break
    if depth != 0:
        return t
    inner = t[i + 1:j]
    parts = [x.strip() for x in inner.split(";")]
    keep = [x for x in parts if x and re.search(r"\d", x) and not x.lower().startswith(NOISE)
            and not re.search(r"[\u0370-\u03ff\u0400-\u04ff\u0590-\u06ff\u3040-\u9fff]", x)]
    if len(parts) == 1 and keep:
        return t                                         # "(Anchiano, 15 aprile 1452 – …)": already clean
    new = f" ({'; '.join(keep)})" if keep else ""
    return (t[:i].rstrip() + new + " " + t[j + 1:].lstrip()).replace("  ", " ").replace(" ,", ",")


# A full stop missing in the article itself ("... naturalizzato statunitense Ha progettato il primo
# microprocessore"). Titles and names are where such capitals are legitimate ("Kung Fu", "la Terza Era",
# "Do They Know It's Christmas?", "il film È nata una stella"), and they live in links, italics and quotes.
# So a stop is added only in the paragraph's PLAIN text (no enclosing tag), before a word that opens a
# sentence in this language (pronouns and verbs, never articles), after a lowercase content word.
STARTERS = {
    "it": ("Ha", "Hanno", "È", "Fu", "Furono", "Era", "Erano", "Sono", "Nacque", "Morì", "Venne"),
    "en": ("He was", "He is", "He has", "She was", "She is", "She has", "They were", "They are", "It is", "It was"),
    "es": ("Fue un", "Fue una", "Es un", "Es una", "Nació", "Murió", "Es considerado", "Es considerada"),
    "fr": ("Il est", "Il a", "Elle est", "Elle a", "Ils sont", "Il fut", "Elle fut"),
    "de": ("Er", "Sie ist", "Sie war", "Er ist", "Er war"),
}


GLUE = {"e", "ed", "o", "con", "come", "anche", "che", "di", "da", "per", "tra", "fra", "era", "il", "lo", "la",
        "and", "or", "of", "for", "with", "that", "what", "was", "nor", "the", "as", "by", "in", "to",
        "y", "o", "con", "que", "de", "del", "el", "et", "ou", "avec", "pour", "que", "de", "du", "le",
        "und", "oder", "mit", "wie", "von", "der", "die", "das", "den", "auch",
        "but", "who", "which", "whom", "qui", "dont", "quien", "cui", "aber"}
RE_TAGTOK = re.compile(r"<(/?)([a-zA-Z][a-zA-Z0-9]*)\b[^>]*?(/?)>")
VOID = {"br", "img", "wbr", "hr", "meta", "link", "input", "source"}


def fix_stops(h, lang):
    """`h` = the inner HTML of one paragraph -> the same with the missing stops added (see above)."""
    words = STARTERS.get(lang)
    if not words:
        return h
    rx = re.compile(r" (?=(?:%s) [a-zà-ÿß])" % "|".join(re.escape(w) for w in sorted(words, key=len, reverse=True)))
    out, plain, depth, pos = [], "", 0, 0

    def seg(t):
        nonlocal plain
        res, last = [], 0
        for m in rx.finditer(t):
            before = RE_TAG.sub("", plain + t[:m.start()])
            w = re.findall(r"[^\W\d_]+", before[-40:])
            if w and before[-1:].islower() and w[-1][0].islower() and w[-1].lower() not in GLUE and len(w[-1]) > 2:
                res.append(t[last:m.start()] + ".")
                last = m.start()
        res.append(t[last:])
        return "".join(res)

    for m in RE_TAGTOK.finditer(h):
        text = h[pos:m.start()]
        out.append(seg(text) if depth == 0 else text)
        plain += text
        name = m.group(2).lower()
        if name not in VOID and not m.group(3):
            depth += -1 if m.group(1) else 1
            depth = max(depth, 0)
        out.append(m.group(0))
        pos = m.end()
    tail = h[pos:]
    out.append(seg(tail) if depth == 0 else tail)
    return "".join(out)


RE_SENT = re.compile(r"(?<=[.!?])\s+(?=[A-ZÀ-ÝÄÖÜ\"«(])")


def split_lead(t):
    """The first sentences as the summary (<= SUMMARY_MAX), the rest of the paragraph as the first passage.
    A sentence end is ". " before a capital; abbreviations ("a.C.", "Dr. ") rarely meet that test."""
    sents = RE_SENT.split(t)
    head = ""
    for i, x in enumerate(sents):
        cand = (head + " " + x).strip()
        if i >= SUMMARY_SENTENCES or (head and len(cand) > SUMMARY_MAX):
            return head, " ".join(sents[i:]).strip()
        head = cand
    return head, ""


def cut(t, n):
    if len(t) <= n:
        return t
    t = t[:n]
    s = max(t.rfind(". "), t.rfind("; "))
    return t[: s + 1] if s > n // 2 else t.rsplit(" ", 1)[0] + "…"


def is_prose(t):
    """A paragraph, not a navigation bar or a bare list: "Portal Geschichte | Portal Biografien | ...",
    "Architecture, Arts plastiques,,,, ...", "Afrique du Sud, Algérie, Angola, ..."."""
    out = t
    for _ in range(3):                                          # separators inside (...) are content:
        out = re.sub(r"\([^()]*\)", "", out)                   # "C·IVLIVS·C·F·CAESAR" in Cesare's lead
    if out.count("|") >= 2 or out.count("·") >= 2 or ",," in out or "◄" in t or "►" in t:
        return False
    if len(t) < 160 and re.search(r"[\u3040-\u9fff]", t):    # "Jahr der Holz-Ratte 甲子": a calendar box
        return False
    words = t.split()
    if len(words) < 6:
        return False
    if t.count(",") > 0.3 * len(words) and "." not in t[:-1]:
        return False                                            # a list of names, no sentence
    return bool(re.search(r"[a-zà-ÿ]{3,}", t))


def strip_blocks(h, tag):
    """Remove every <tag>...</tag>, nested ones included: infobox/taxobox tables and figure captions hold <p>s
    that are not the article's prose ("Camelus pacos Linnaeus, 1758", "Katzenfloh unter dem Mikroskop")."""
    open_re, close_re = re.compile(rf"<{tag}\b", re.I), re.compile(rf"</{tag}\s*>", re.I)
    out, i = [], 0
    while True:
        m = open_re.search(h, i)
        if not m:
            out.append(h[i:]); break
        out.append(h[i:m.start()])
        depth, j = 1, m.end()
        while depth and j < len(h):
            o, c = open_re.search(h, j), close_re.search(h, j)
            if not c:
                j = len(h); break
            if o and o.start() < c.start():
                depth += 1; j = o.end()
            else:
                depth -= 1; j = c.end()
        i = j
    return "".join(out)


RE_HATNOTE = re.compile(r"^(pour plus de d[ée]tails|pour les articles homonymes|pour un article plus g[ée]n[ée]ral|"
                        r"voir aussi|see also|for other uses|siehe auch|v[ée]ase tambi[ée]n|vedi anche|"
                        r"disambiguazione)", re.I)


def extract(html_text, lang=""):
    m = RE_SEC0.search(html_text)
    body = m.group(0) if m else html_text
    for tag in ("table", "figure", "aside"):
        body = strip_blocks(body, tag)
    pars = [clean_par(fix_stops(p, lang)) for p in RE_P.findall(body)]
    pars = [p for p in pars if len(p) > 40 and is_prose(p) and not RE_HATNOTE.match(p)]
    g = RE_GEO.search(html_text)
    return pars, (g.group(1) if g else "")


# ---- build ---------------------------------------------------------------------------------------------
def load_wikidata(lang):
    """title -> QID for this language, and QID -> {lang: title} (tools/kb/wikidata.py caches)."""
    cache = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".cache")
    t2q, sl = {}, {}
    p = os.path.join(cache, f"qid-{lang}.tsv")
    if os.path.exists(p):
        for line in open(p, encoding="utf-8"):
            t, _, q = line.rstrip("\n").partition("\t")
            if q: t2q[t] = q
    p = os.path.join(cache, "sitelinks.tsv")
    if os.path.exists(p):
        langs = ("it", "en", "es", "fr", "de")
        for line in open(p, encoding="utf-8"):
            f = line.rstrip("\n").split("\t")
            if len(f) == 6: sl[f[0]] = {l: t for l, t in zip(langs, f[1:]) if t}
    return t2q, sl


def build(zim_path, out_path, limit=0):
    from libzim.reader import Archive
    z = Archive(zim_path)
    meta = {k: (z.get_metadata(k).decode("utf-8", "replace") if not k.startswith("Illustration") else None)
            for k in z.metadata_keys}
    lang = {"ita": "it", "eng": "en", "spa": "es", "fra": "fr", "deu": "de"}.get(meta.get("Language", ""), "xx")
    t2q, sitel = load_wikidata(lang)
    print(f"  Wikidata: {len(t2q)} QIDs for {lang}, {len(sitel)} sitelinks")
    import facts as wdf                                        # tools/kb/facts.py: Wikidata facts (CC0)
    have_facts = os.path.exists(wdf.RAW)
    fraw = wdf.load_raw() if have_facts else {}
    fnames = wdf.load_labels() if have_facts else {}
    nfacts = 0
    t0 = time.time()
    ents, path_id, redirects, sections = [], {}, [], []
    for i in range(z.all_entry_count):
        e = z._get_entry_by_id(i)
        if e.is_redirect:
            try:
                redirects.append((e.title, e.get_redirect_entry().path))
            except Exception:
                pass
            continue
        if e.path in ("mainPage", "index") or e.path.startswith(("_", "-")):
            continue
        it = e.get_item()
        if it.mimetype != "text/html":
            continue
        if "(disambigua)" in e.title:
            continue
        page = bytes(it.content).decode("utf-8", "replace")
        sref = RE_SECREF.search(page) if len(page) < 2048 else None
        if sref:                                                 # "Jimbo Kern" -> Personaggi_di_South_Park#...
            from urllib.parse import unquote
            sections.append((e.title, unquote(sref.group(1))))
            continue
        pars, geo = extract(page, lang)
        if not pars:
            continue
        summary, rest = split_lead(tidy_lead(pars[0]))
        passages = ([rest] if rest else []) + pars[1:]
        q = t2q.get(e.title, "")
        fl = wdf.fact_line(fraw[q], lang, fnames) if q in fraw else ""
        nfacts += bool(fl)
        rec = SEP_F.join([e.title, q, e.path, geo, cut(summary, SUMMARY_MAX + 120),
                          SEP_P.join(cut(p, PASSAGE_MAX) for p in passages[:PASSAGES]), fl])
        path_id[e.path] = len(ents)
        ents.append((e.title, rec.encode("utf-8")))
        if limit and len(ents) >= limit:
            break
    print(f"  {len(ents)} entities ({nfacts} with Wikidata facts), {len(redirects)} redirects, {len(sections)} section pages read in {time.time() - t0:.0f}s")

    # keys: title, redirects, title without "(qualifier)" (ambiguous: several may share it)
    keys = {}
    def add(k, i, amb=False, mark=None):
        if not k:
            return
        lst = keys.setdefault(k, [])
        tag = (mark or ("~" if amb else "")) + str(i)
        if str(i) not in [x.lstrip("~^@") for x in lst]:
            lst.append(tag)
    for i, (title, _r) in enumerate(ents):
        add(key_of(title), i)
    for title, target in redirects:
        if target in path_id:
            add(key_of(title), path_id[target])
    for title, target in sections:                               # '^': this name is a SECTION of that entity
        if target in path_id:
            add(key_of(title), path_id[target], mark="^")
    for i, (title, _r) in enumerate(ents):
        base = re.sub(r"\s*\([^)]*\)\s*$", "", title)
        if base != title:
            add(key_of(base), i, amb=True)
    qid_ent = {}
    nx = 0
    for i, (title, _r) in enumerate(ents):                       # the same entity's title in the other languages
        q = t2q.get(title)
        if not q:
            continue
        qid_ent.setdefault(q, i)
        for l, t in sitel.get(q, {}).items():
            if l == lang:
                continue
            k = key_of(t)
            if k and k not in keys:                               # a native key always wins
                add(k, i, mark="@"); nx += 1
            kb2 = key_of(re.sub(r"\s*\([^)]*\)\s*$", "", t))
            if kb2 and kb2 != k and kb2 not in keys:
                add(kb2, i, mark="@"); nx += 1
    print(f"  {nx} cross-language keys, {len(qid_ent)} entities with a QID")
    for k, lst in keys.items():                                  # exact ids before ambiguous ones
        lst.sort(key=lambda x: ({"@": 1, "~": 2, "^": 3}.get(x[0], 0)))
        if len(lst) > 1 and any(not x.startswith("~") for x in lst) is False:
            pass
    keys_blob = "".join(f"{k}\t{','.join(v[:8])}\n" for k, v in sorted(keys.items(), key=lambda kv: kv[0].encode()))

    # blocks
    blocks, entrecs, cur, cur_recs = [], [], bytearray(), []
    def flush():
        if cur:
            c = zlib.compressobj(9, zlib.DEFLATED, -15)
            blocks.append((c.compress(bytes(cur)) + c.flush(), len(cur)))
    for i, (_t, rec) in enumerate(ents):
        if len(cur) + len(rec) > BLOCK and cur:
            flush(); cur = bytearray()
        entrecs.append((len(blocks), len(cur), len(rec)))
        cur += rec
    flush()

    info = {"format": "AKB6", "version": VERSION, "lang": lang, "source": meta.get("Source", ""),
            "source_name": meta.get("Name", ""), "source_date": meta.get("Date", ""),
            "built": time.strftime("%Y-%m-%d"), "licence": "CC BY-SA 4.0",
            "attribution": f"Wikipedia ({meta.get('Source', '')}), CC BY-SA 4.0, via Kiwix/openZIM",
            "entities": len(ents), "keys": len(keys), "blocks": len(blocks)}
    qids_blob = "".join(f"{q}\t{i}\n" for q, i in sorted(qid_ent.items(), key=lambda kv: kv[0].encode()))
    info["qids"] = len(qid_ent)
    sec = {"META": json.dumps(info, ensure_ascii=False).encode(), "KEYS": keys_blob.encode(),
           "ENTS": b"".join(struct.pack("<III", *r) for r in entrecs), "QIDS": qids_blob.encode()}
    order = ["META", "KEYS", "ENTS", "QIDS", "BIDX", "BLKS"]
    head_len = 64 + len(order) * 20
    off = head_len
    offs = {}
    for name in ("META", "KEYS", "ENTS", "QIDS"):
        offs[name] = off; off += len(sec[name])
    bidx_off = off; off += len(blocks) * 16
    blks_off = off
    bidx, p = bytearray(), blks_off
    for data, raw in blocks:
        bidx += struct.pack("<QII", p, len(data), raw); p += len(data)
    sec["BIDX"] = bytes(bidx); offs["BIDX"] = bidx_off
    offs["BLKS"] = blks_off
    with open(out_path, "wb") as f:
        pid = f"wikipedia_{lang}_{meta.get('Name', '').split('_')[-1]}"[:31].encode()
        f.write(b"AKB6" + struct.pack("<HH4sIIIII32s", VERSION, 0, lang.encode(), len(ents), len(keys), len(blocks),
                                     len(order), 0, pid))
        for name in order:
            size = sum(len(d) for d, _ in blocks) if name == "BLKS" else len(sec[name])
            f.write(name.encode() + struct.pack("<QQ", offs[name], size))
        assert f.tell() == head_len
        for name in ("META", "KEYS", "ENTS", "QIDS", "BIDX"):
            assert f.tell() == offs[name], name
            f.write(sec[name])
        for data, _ in blocks:
            f.write(data)
    sz = os.path.getsize(out_path)
    print(f"  {out_path}: {sz / 1e6:.1f} MB ({os.path.getsize(zim_path) / 1e6:.1f} MB ZIM), {len(keys)} keys, "
          f"{len(blocks)} blocks, KEYS {len(keys_blob) / 1e6:.1f} MB")


# ---- read (the device algorithm, with a byte counter) ---------------------------------------------------
class Pack:
    def __init__(self, path):
        self.f = open(path, "rb")
        self.reads = self.bytes = 0
        h = self.f.read(64)
        assert h[:4] == b"AKB6"
        _v, _fl, lang, self.n_ent, self.n_keys, self.n_blk, nsec, _res, pid = struct.unpack("<HH4sIIIII32s", h[4:64])
        self.lang = lang.rstrip(b"\0").decode()
        self.sec = {}
        for _ in range(nsec):
            d = self.f.read(20)
            self.sec[d[:4].decode()] = struct.unpack("<QQ", d[4:])
        self.meta = json.loads(self._read(*self.sec["META"]))
        self.reads = self.bytes = 0

    def _read(self, off, n):
        self.f.seek(off); self.reads += 1; self.bytes += n
        return self.f.read(n)

    def key(self, k):
        """Bisect the KEYS section exactly like anima_dict_get (whole lines, byte order)."""
        lo, hi = self.sec["KEYS"][0], self.sec["KEYS"][0] + self.sec["KEYS"][1]
        kb = k.encode()
        while hi - lo > 4096:
            mid = lo + (hi - lo) // 2
            chunk = self._read(mid, 1024)
            nl = chunk.find(b"\n")
            if nl < 0 or mid + nl + 1 >= hi:
                hi = mid; continue
            ls = mid + nl + 1
            line = chunk[nl + 1:].split(b"\n", 1)[0]
            if len(line) < 3:
                line = self._read(ls, 512).split(b"\n", 1)[0]
            if line.split(b"\t", 1)[0] < kb: lo = ls + len(line) + 1
            else: hi = mid
        win = self._read(lo, min(hi - lo + 1024, 16384))
        for line in win.split(b"\n"):
            kk, _, v = line.partition(b"\t")
            if kk == kb:
                return [(x[:1].decode() if x[:1] in (b"~", b"^", b"@") else "", int(x.lstrip(b"~^@"))) for x in v.split(b",")]
            if kk > kb:
                break
        return []

    def entity(self, i):
        b, o, n = struct.unpack("<III", self._read(self.sec["ENTS"][0] + 12 * i, 12))
        boff, csz, rsz = struct.unpack("<QII", self._read(self.sec["BIDX"][0] + 16 * b, 16))
        raw = zlib.decompress(self._read(boff, csz), -15)
        f = raw[o:o + n].decode().split(SEP_F)
        return {"title": f[0], "qid": f[1], "path": f[2], "geo": f[3], "summary": f[4],
                "passages": f[5].split(SEP_P) if f[5] else [], "facts": f[6] if len(f) > 6 else ""}


LEADS = ["chi e stato", "chi era", "chi e", "che cos e", "cos e", "cosa e", "cosa sono", "cosa sai di", "parlami di",
         "dimmi di", "who is", "who was", "what is", "what are", "tell me about"]
ARTS = ["il", "lo", "la", "i", "gli", "le", "l", "un", "uno", "una", "the", "a", "an"]


def topic_of(question):
    """A question -> the entity key: drop the lead-in and a leading article (what the device's a_topic_strip does)."""
    k = key_of(question)
    for l in LEADS:
        if k.startswith(l + " "):
            k = k[len(l) + 1:]; break
    w = k.split()
    if len(w) > 1 and w[0] in ARTS:
        k = " ".join(w[1:])
    return k


def ask(pack, q):
    p = Pack(pack)
    k = topic_of(q)
    ids = p.key(k)
    print(f"key '{k}' -> {ids}")
    if not ids:
        print("  (no entry: the device abstains)"); return
    amb = [i for a, i in ids if a == "~"]
    exact = [i for a, i in ids if not a] or [i for a, i in ids if a == "@"]
    sect = [i for a, i in ids if a == "^"]
    if not exact and len(amb) > 1:
        print("  ambiguous:", "; ".join(p.entity(i)["title"] for i in amb[:5]))
        return
    if not exact and not amb and sect:
        print(f"  (section) ne parla la voce «{p.entity(sect[0])['title']}»")
    e = p.entity((exact or amb or sect)[0])
    print(f"  {e['title']}  [{e['path']}] geo={e['geo']}\n  {e['summary']}")
    for s in e["passages"][:2]:
        print("  +", s[:200])
    print(f"  reads={p.reads} bytes={p.bytes}  source: {p.meta['attribution']}")


def evaluate(pack, zim_path, n):
    """Titles and redirects asked as questions: right / wrong / ambiguous / abstain, and the device's cost."""
    import random
    from libzim.reader import Archive
    z = Archive(zim_path)
    p = Pack(pack)
    rnd = random.Random(7)
    arts, reds, secs = [], [], []
    tries = 0
    while (len(arts) < n or len(reds) < n or len(secs) < n // 4) and tries < 400 * n:   # some ZIMs have no sections
        tries += 1
        e = z._get_entry_by_id(rnd.randrange(z.all_entry_count))
        if e.path.startswith(("_", "-")) or e.path in ("mainPage", "index"):
            continue
        try:
            if e.is_redirect:
                if len(reds) < n: reds.append((e.title, e.get_redirect_entry().title))
            elif e.get_item().mimetype == "text/html" and "(disambigua)" not in e.title:
                it = e.get_item()
                if it.size < 2048:
                    m = RE_SECREF.search(bytes(it.content).decode("utf-8", "replace"))
                    if m:
                        if len(secs) < n // 4:
                            from urllib.parse import unquote
                            try: secs.append((e.title, z.get_entry_by_path(unquote(m.group(1))).title))
                            except Exception: pass
                        continue
                if len(arts) < n:
                    arts.append((e.title, e.title))
        except Exception:
            continue
    forms = [("{}", "titolo"), ("chi è {}", "chi è"), ("cos'è {}", "cos'è"), ("{}", "minuscolo")]
    report = {}
    for label, items in (("voci", arts), ("redirect", reds), ("sezioni", secs)):
        if not items:
            continue
        for fmt, fname in forms:
            right = wrong = amb = miss = 0; p.reads = p.bytes = 0; nq = 0
            for asked, want in items:
                q = fmt.format(asked.lower() if fname == "minuscolo" else asked)
                ids = p.key(topic_of(q)); nq += 1
                if not ids: miss += 1; continue
                exact = [i for a, i in ids if a != "~"]
                if not exact and len(ids) > 1: amb += 1; continue
                got = p.entity((exact or [ids[0][1]])[0])["title"]
                if got == want: right += 1
                else: wrong += 1
            report[f"{label}/{fname}"] = (right, wrong, amb, miss, p.reads / nq, p.bytes / nq)
    print(f"{'set':22} {'giuste':>7} {'sbagliate':>9} {'ambigue':>8} {'assenti':>8} {'letture':>8} {'KB letti':>9}")
    for k, (r, w, a, m, rd, by) in report.items():
        tot = r + w + a + m
        print(f"{k:22} {100*r/tot:6.1f}% {100*w/tot:8.1f}% {100*a/tot:7.1f}% {100*m/tot:7.1f}% {rd:8.1f} {by/1024:9.1f}")
    # cross-language: the entity's title in another language (Wikidata sitelinks) must find THIS entity
    t2q, sitel = load_wikidata(p.lang)
    if sitel:
        q2title = {q: t for t, q in t2q.items()}
        for other in ("en", "es", "fr", "de", "it"):
            if other == p.lang:
                continue
            pairs = [(v[other], q2title[q]) for q, v in sitel.items() if other in v and q in q2title]
            rnd.shuffle(pairs)
            right = wrong = miss = 0
            for asked, want in pairs[:n]:
                ids = p.key(topic_of(asked))
                if not ids: miss += 1; continue
                exact = [i for a, i in ids if a in ("", "@")]
                if not exact: miss += 1; continue
                if p.entity(exact[0])["title"] == want: right += 1
                else: wrong += 1
            tot = max(1, right + wrong + miss)
            print(f"{'titolo ' + other + ' -> ' + p.lang:22} {100*right/tot:6.1f}% {100*wrong/tot:8.1f}% "
                  f"{'':>8} {100*miss/tot:7.1f}%   ({tot} titoli; una 'sbagliata' è spesso un omonimo nativo)")
    # summary quality: a lead names its subject early ("Gaio Giulio Cesare (…) è stato…"). A summary that does not
    # mention any word of the title in its first 160 characters usually lost its first sentence or is a list.
    sample = rnd.sample(range(p.n_ent), min(n, p.n_ent))
    nolead = []
    for i in sample:
        e = p.entity(i)
        words = [w for w in key_of(re.sub(r"\s*\([^)]*\)\s*$", "", e["title"])).split() if len(w) >= 4]
        head = " ".join(tokens("".join(XFOLD.get(ch, ch) for ch in e["summary"][:160]).lower()))
        if words and not any(w[:5] in head for w in words):
            nolead.append(e["title"])
    print(f"riassunti che non nominano il soggetto: {100*len(nolead)/len(sample):.1f}%  es. {nolead[:4]}")
    fake = ["Pincopallino Gargamella", "Zorblax Venturini", "Teorema di Fusconi-Bratt", "Isola di Qwertania", "Newtron"]
    hits = [f for f in fake if p.key(topic_of("chi è " + f))]
    print(f"inesistenti con una risposta: {len(hits)}/{len(fake)} {hits}")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build"); b.add_argument("zim"); b.add_argument("out"); b.add_argument("--limit", type=int, default=0)
    a = sub.add_parser("ask"); a.add_argument("pack"); a.add_argument("q")
    e = sub.add_parser("eval"); e.add_argument("pack"); e.add_argument("zim"); e.add_argument("--n", type=int, default=1000)
    x = ap.parse_args()
    if x.cmd == "build": build(x.zim, x.out, x.limit)
    elif x.cmd == "ask": ask(x.pack, x.q)
    else: evaluate(x.pack, x.zim, x.n)
