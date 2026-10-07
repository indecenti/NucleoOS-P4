"""gen18.py - Vertice Bass: the junk lying on the lake bed (img/jb0..3.565), drawn for it.

The first bed sprites were cut from the small catch paintings (junk0..3, the object ~70 px of a 150x100
picture, with water dripping off it): dark and soft on the bed. These are painted one object per picture
at full resolution, lying the way things lie on a lake bed, keyed and fitted to fill a 128x128 billboard
texture (row 0 at the BOTTOM: VX_BILLBOARD), the object standing on the bottom row, a clear frame.

    python apps/bass/art/gen18.py              # previews in %TEMP%/bass_art/g18_*.png
"""
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tools"))
import qwen_assets as q
from gen11 import IMG, OUT, STYLE, key_fish

ITEM = ("{what}, seen from the side and slightly above, whole object inside the picture and filling most of it, "
        "clearly lit, sharp details, on a plain flat deep navy blue background, nothing else, no water, no drops.")
JOBS = [  # (texture, what, seed)
    ("jb0", "One old rusty tin can lying on its side, faded red and silver label, a dented rim", 351),
    ("jb1", "One old worn brown leather work boot lying on its side, laces undone, a little green algae", 352),
    ("jb2", "One old black car tyre lying flat, seen at an angle so its round shape and tread show, some algae", 353),
    ("jb3", "One small wooden pirate treasure chest, lid half open, gold coins and jewels glinting inside", 354),
]

if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    for name, what, seed in JOBS:
        src = os.path.join(OUT, "g18_%s_%d.png" % (name, seed))
        if not os.path.exists(src):
            q.generate(STYLE + ITEM.format(what=what), 1024, 1024, seed).save(src)
        sp = np.asarray(key_fish(Image.open(src), w=128, h=128, margin=1)).copy()   # centred, fitted, keyed
        key = (sp[..., 0] == 255) & (sp[..., 1] == 0) & (sp[..., 2] == 255)
        ys = np.nonzero(~key.all(axis=1))[0]
        sp = np.roll(sp, 127 - ys.max(), axis=0)                    # stand it on the bottom row
        sp[0] = (255, 0, 255)                                        # a clear top row (clamped sampling)
        Image.fromarray(sp).save(os.path.join(OUT, "g18_%s.png" % name))
        q.to565(Image.fromarray(sp[::-1].copy()), os.path.join(IMG, name + ".565"))   # billboard: row 0 at the bottom
        print(name, "ok", flush=True)
