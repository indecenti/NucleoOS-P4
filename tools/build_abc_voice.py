#!/usr/bin/env python3
# Build a SMALL voice pack for abc123 (and the OS) by extracting only the slugs we need from the big
# G:\Nucleo concatenative-TTS dictionary (it/en, ~12.5k clips, ~400MB each). Output is the same NTI1
# format (index.bin + clips.pcm, mono 16-bit @24kHz) so nv_tts plays it unchanged — just tiny.
#
# Slugs taken: every abc123 object word (it + en columns of OBJ_W, parsed from main.c), the cardinal
# number words 0..20, and "bravo". Missing slugs are simply skipped (best effort).
#
#   python tools/build_abc_voice.py        (use Windows 'python' so it can read G:\)
# Output: sd/data/tts/<lang>/{index.bin,clips.pcm}  -> push to /sdcard/data/tts/<lang>/
import re, struct, os

ROOT = r"D:\NucleoV2"
MAIN = os.path.join(ROOT, "apps", "abc123", "main.c")
BIG  = r"G:\Nucleo\deploy\sd-master\data\tts"
OUT  = os.path.join(ROOT, "sd", "data", "tts")

def slugify(w):
    return w.strip().lower().replace(" ", "_")

# parse OBJ_W rows: {"SOLE","SUN","SOL","SOLEIL","SONNE"},
txt = open(MAIN, encoding="utf-8").read()
i = txt.index("OBJ_W[O_COUNT][L_COUNT]")
block = txt[i:txt.index("\n};", i)]
rows = re.findall(r'\{\s*"([^"]*)","([^"]*)","([^"]*)","([^"]*)","([^"]*)"\s*\}', block)
it_words = [slugify(r[0]) for r in rows]
en_words = [slugify(r[1]) for r in rows]
print(f"parsed {len(rows)} objects from main.c")

# say_letter() speaks a bare 1-char slug per tapped letter (INIZIALE/IN ORDINE/COMPONI). Pull each
# language's alphabet from T_ALPHA so whatever single-letter clips the master corpus has come along
# (the corpus is a general word dictionary, not a phonetic alphabet — coverage is partial; main.c
# gates playback with LETTER_VOICE_IT/EN so missing ones are skipped, not silently "spoken").
j = txt.index("T_ALPHA[L_COUNT]")
block2 = txt[j:txt.index("};", j)]
alpha_rows = re.findall(r'"([A-Z]+)"', block2)
alpha_it = [c.lower() for c in alpha_rows[0]]
alpha_en = [c.lower() for c in alpha_rows[1]]

num_it = "zero uno due tre quattro cinque sei sette otto nove dieci undici dodici tredici quattordici quindici sedici diciassette diciotto diciannove venti".split()
num_en = "zero one two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen sixteen seventeen eighteen nineteen twenty".split()

# operator words for the CONTI (math) game — nv_tts folds accents, so "piu" matches "più"
op_it = ["piu", "meno"]
op_en = ["plus", "minus"]

sets = {
    "it": sorted(set(it_words + num_it + op_it + alpha_it + ["bravo"])),
    "en": sorted(set(en_words + num_en + op_en + alpha_en + ["bravo"])),
}

def load_index(lang):
    with open(os.path.join(BIG, lang, "index.bin"), "rb") as f:
        f.read(4); rate, count = struct.unpack("<II", f.read(8))
        idx = {}
        for _ in range(count):
            rec = f.read(56)
            slug = rec[:48].split(b"\x00")[0].decode("latin1")
            off, ln = struct.unpack("<II", rec[48:56])
            idx[slug] = (off, ln)
    return rate, idx

def build(lang, slugs):
    rate, idx = load_index(lang)
    recs, blob = [], bytearray()
    miss = []
    with open(os.path.join(BIG, lang, "clips.pcm"), "rb") as f:
        for s in slugs:
            if s in idx:
                off, ln = idx[s]; f.seek(off); data = f.read(ln)
                recs.append((s, len(blob), len(data))); blob += data
            else:
                miss.append(s)
    recs.sort(key=lambda r: r[0].encode("latin1"))   # NTI1 needs slugs sorted by strcmp for binary search
    od = os.path.join(OUT, lang); os.makedirs(od, exist_ok=True)
    with open(os.path.join(od, "clips.pcm"), "wb") as f: f.write(blob)
    with open(os.path.join(od, "index.bin"), "wb") as f:
        f.write(b"NTI1"); f.write(struct.pack("<II", rate, len(recs)))
        for (s, o, l) in recs:
            sb = s.encode("latin1")[:47]; sb = sb + b"\x00" * (48 - len(sb))
            f.write(sb); f.write(struct.pack("<II", o, l))
    print(f"{lang}: {len(recs)}/{len(slugs)} clips  {len(blob)/1024:.0f} KB  (missing {len(miss)}: {miss[:12]})")

for lang, slugs in sets.items():
    build(lang, slugs)
