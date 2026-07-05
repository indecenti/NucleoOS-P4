#!/usr/bin/env python3
# Merge math operators (+ any missing number words) into the DEVICE's existing NTI1 voice pack
# WITHOUT dropping any clip it already has (the device pack was built from the full corpus and has
# object nouns that the general G: dict lacks). Downloads the device pack, appends the operator clips
# taken from the big G: dict, re-sorts the index, uploads it back. Idempotent (skips slugs present).
#
#   python tools/merge_tts_ops.py
import struct, os, urllib.request

DEV  = "http://192.168.0.128"
BIG  = r"G:\Nucleo\deploy\sd-master\data\tts"
NUMS_IT = "zero uno due tre quattro cinque sei sette otto nove dieci undici dodici tredici quattordici quindici sedici diciassette diciotto diciannove venti".split()
NUMS_EN = "zero one two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen sixteen seventeen eighteen nineteen twenty".split()
PRAISE_IT = "bravo bravissimo perfetto super evviva fantastico ottimo magnifico grande".split()
PRAISE_EN = "super wow great perfect bravo awesome".split()
NEG_IT = "riprova quasi coraggio".split()
NEG_EN = "almost again oops".split()
WANT = { "it": ["piu", "meno"] + NUMS_IT + PRAISE_IT + NEG_IT,
         "en": ["plus", "minus"] + NUMS_EN + PRAISE_EN + NEG_EN }

def http_get(path):
    with urllib.request.urlopen(f"{DEV}/api/fs/read?path={path}", timeout=60) as r: return r.read()

OUTDIR = os.path.join(os.path.dirname(__file__), "..", "sd", "data", "tts")

def parse_index(b):
    assert b[:4] == b"NTI1", "bad magic"
    rate, count = struct.unpack("<II", b[4:12]); recs = []
    for i in range(count):
        rec = b[12 + i*56 : 12 + i*56 + 56]
        slug = rec[:48].split(b"\x00")[0].decode("latin1")
        off, ln = struct.unpack("<II", rec[48:56]); recs.append([slug, off, ln])
    return rate, recs

def load_big(lang):
    with open(os.path.join(BIG, lang, "index.bin"), "rb") as f:
        f.read(4); rate, count = struct.unpack("<II", f.read(8)); idx = {}
        for _ in range(count):
            rec = f.read(56); slug = rec[:48].split(b"\x00")[0].decode("latin1")
            off, ln = struct.unpack("<II", rec[48:56]); idx[slug] = (off, ln)
    return idx

def write_index(rate, recs):
    out = bytearray(b"NTI1"); out += struct.pack("<II", rate, len(recs))
    for slug, off, ln in sorted(recs, key=lambda r: r[0].encode("latin1")):   # NTI1 = strcmp-sorted
        sb = slug.encode("latin1")[:47]; sb += b"\x00" * (48 - len(sb))
        out += sb + struct.pack("<II", off, ln)
    return bytes(out)

for lang, wants in WANT.items():
    rate, recs = parse_index(http_get(f"/data/tts/{lang}/index.bin"))
    blob = bytearray(http_get(f"/data/tts/{lang}/clips.pcm"))
    have = {r[0] for r in recs}
    big_idx = load_big(lang)
    added = []
    with open(os.path.join(BIG, lang, "clips.pcm"), "rb") as bf:
        for s in wants:
            if s in have or s not in big_idx: continue
            off, ln = big_idx[s]; bf.seek(off); data = bf.read(ln)
            recs.append([s, len(blob), ln]); blob += data; added.append(s)
    if not added:
        print(f"{lang}: nothing to add (already complete)"); continue
    od = os.path.join(OUTDIR, lang + "_merged"); os.makedirs(od, exist_ok=True)
    with open(os.path.join(od, "clips.pcm"), "wb") as f: f.write(bytes(blob))
    with open(os.path.join(od, "index.bin"), "wb") as f: f.write(write_index(rate, recs))
    print(f"{lang}: +{len(added)} clips {added} -> {od} ({len(recs)} clips, {len(blob)//1024} KB)")
