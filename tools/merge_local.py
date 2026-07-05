#!/usr/bin/env python3
# Build the final device packs from LOCAL copies (no flaky download): add phrases + math ops (plain
# slugs) AND letter clips (alias the corpus `lett_<x>` under the single-char slug "<x>" so abc123 can
# speak a letter by passing that char). Vowels a/e/i/o keep their existing word clip (same sound);
# consonants + u are aliased/overwritten from lett_<x>. Extra clips come from the big G: dict.
import struct, os

BIG = r"G:\Nucleo\deploy\sd-master\data\tts"
ROOT = os.path.join(os.path.dirname(__file__), "..", "sd", "data", "tts")

PRAISE_IT = "bravo bravissimo perfetto super evviva fantastico ottimo magnifico grande".split()
PRAISE_EN = "super wow great perfect bravo awesome".split()
NEG_IT = "riprova quasi coraggio".split()
NEG_EN = "almost again oops".split()
LETTERS = "a b c d e f g h i j k l m n o p q r s t u v w x y z".split()   # all 26 aliased from lett_<x> (vowel words aren't in the subset pack)
# "wave 4" object words (2026-07-04) that DO exist in the master corpus — the rest (cactus/unicorn/
# kite it/en, squalo/balena/aquilone/unicorno IT) have no clip anywhere, so they stay gated silent by
# OBJ_VOICE_IT/EN in main.c instead of being requested here.
NEW_OBJ_IT = "drago castello montagna".split()
NEW_OBJ_EN = "shark dragon castle whale mountain".split()

JOBS = [   # (lang, source_dir_rel, plain_extra, out_dir_rel)
    ("it", "it",        PRAISE_IT + NEG_IT + NEW_OBJ_IT, "it_final"),
    ("en", "en_merged", PRAISE_EN + NEG_EN + NEW_OBJ_EN, "en_final"),
]

def parse(path):
    with open(path, "rb") as f: b = f.read()
    assert b[:4] == b"NTI1", path + " bad magic"
    rate, count = struct.unpack("<II", b[4:12]); d = {}
    for i in range(count):
        rec = b[12 + i*56 : 12 + i*56 + 56]
        slug = rec[:48].split(b"\x00")[0].decode("latin1")
        off, ln = struct.unpack("<II", rec[48:56]); d[slug] = (off, ln)
    return rate, d

def load_big(lang):
    with open(os.path.join(BIG, lang, "index.bin"), "rb") as f:
        f.read(4); rate, count = struct.unpack("<II", f.read(8)); idx = {}
        for _ in range(count):
            rec = f.read(56); slug = rec[:48].split(b"\x00")[0].decode("latin1")
            off, ln = struct.unpack("<II", rec[48:56]); idx[slug] = (off, ln)
    return idx

def write_index(rate, d):
    out = bytearray(b"NTI1"); out += struct.pack("<II", rate, len(d))
    for slug in sorted(d, key=lambda s: s.encode("latin1")):   # NTI1 = strcmp-sorted
        off, ln = d[slug]
        sb = slug.encode("latin1")[:47]; sb += b"\x00" * (48 - len(sb))
        out += sb + struct.pack("<II", off, ln)
    return bytes(out)

for lang, src, plain, outrel in JOBS:
    srcdir = os.path.join(ROOT, src)
    rate, recs = parse(os.path.join(srcdir, "index.bin"))
    blob = bytearray(open(os.path.join(srcdir, "clips.pcm"), "rb").read())
    assert len(blob) > 100_000, f"{src} clips.pcm too small ({len(blob)}) - refusing"
    big = load_big(lang)
    bf = open(os.path.join(BIG, lang, "clips.pcm"), "rb")
    def add(out_slug, src_slug, overwrite):
        if src_slug not in big: return None
        if out_slug in recs and not overwrite: return None
        off, ln = big[src_slug]; bf.seek(off); data = bf.read(ln)
        recs[out_slug] = (len(blob), ln); blob.extend(data); return out_slug
    added_p = [s for s in plain if add(s, s, False)]
    added_l = [c for c in LETTERS if add(c, "lett_" + c, True)]
    bf.close()
    od = os.path.join(ROOT, outrel); os.makedirs(od, exist_ok=True)
    open(os.path.join(od, "clips.pcm"), "wb").write(bytes(blob))
    open(os.path.join(od, "index.bin"), "wb").write(write_index(rate, recs))
    print(f"{lang}: +{len(added_p)} phrases +{len(added_l)} letters -> {outrel} ({len(recs)} clips, {len(blob)//1024} KB)")
