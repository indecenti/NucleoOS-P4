"""gen13.py - Vertice Bass: the two new species, the sturgeon (8) and the alligator gar (9).

For each one: the large side view that skins the 3D fish (%TEMP%/bass_art/hd<N>.png, read by
fishtex.py), the regular painting img/fish<N>.565, the trophy img/fish<N>_b.565 and the young one
img/fish<N>_s.565 - each its own landscape picture on navy, keyed like gen11.py.

    python apps/bass/art/gen13.py          # then: python apps/bass/art/fishtex.py
"""
import os
import shutil
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tools"))
import qwen_assets as q
from gen11 import FISH, IMG, OUT, STYLE, key_fish

KINDS = {
    8: "sturgeon, long shark-like body, grey-brown back with rows of pale bony plates along its sides, a long "
       "pointed snout with four whiskers underneath, a shark-like upturned tail, pale belly",
    9: "alligator gar, long torpedo body, olive brown back with dark spots, a broad crocodile-like snout full "
       "of needle teeth, thick diamond-shaped armour scales, rounded fins set far back near the tail; it is "
       "a FISH, not a crocodile: no legs, no feet, only small thin fins",
}
JOBS = [  # (species, suffix, how, seed): "" = the regular one (also the 3D skin), _b trophy, _s young
    (8, "", "One adult ", 161), (8, "_b", "One huge fat trophy-sized old monster ", 162), (8, "_s", "One small young ", 163),
    (9, "", "One adult ", 174), (9, "_b", "One huge fat trophy-sized old monster ", 172), (9, "_s", "One small young ", 173),
]

if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    only = sys.argv[1:]
    for sp, suf, how, seed in JOBS:
        name = "fish%d%s" % (sp, suf)
        if only and name not in only:
            continue
        src = os.path.join(OUT, "g13_%s_%d.png" % (name, seed))
        if not os.path.exists(src):
            q.generate(STYLE + FISH.format(what=how + KINDS[sp]), 1024, 672, seed).save(src)
        img = Image.open(src)
        q.to565(key_fish(img, size=0.62 if suf == "_s" else 0.97 if suf == "" else 1.0), os.path.join(IMG, name + ".565"))
        key_fish(img).save(os.path.join(OUT, "g13_%s_key.png" % name))
        if suf == "":
            shutil.copy(src, os.path.join(OUT, "hd%d.png" % sp))      # the 3D skin's source
        print(name, "ok", flush=True)
