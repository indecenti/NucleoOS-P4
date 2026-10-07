"""gen15.py - Vertice Bass: the "qualified" painting, one per lake (img/win0..5.565), and the tournament
champion (img/champ.565), with the SAME angler as title / lose / intro3 (gen14.py, gen12.py).

The old win.565 and champ.565 showed two other men (curly hair; a beard). Every lake now gets its own
scene in its own light: the alpine lake by day, the marsh at sunset, the dam at night, the red canyon,
the autumn lake, the king's lake with its castle. Composition matches the weigh-in screen: the angler
holds the bass across his chest, his face in the upper left third (clear of the title plate in the
middle and the rank medal at the top right); the bottom quarter is covered by the score plate.

    python apps/bass/art/gen15.py                       # previews in %TEMP%/bass_art/g15_<name>_<seed>.png
    python apps/bass/art/gen15.py --pick win0 1 win1 2 ...
"""
import os
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tools"))
import qwen_assets as q
from gen14 import ANGLER, IMG, OUT, STYLE

HOLD = (", grinning proudly, holds a big largemouth bass horizontally across his chest with both hands, the "
        "WHOLE fish visible from its open mouth to its tail, standing in his red bass boat. The angler on the "
        "left half of the picture, from the waist up, his face in the upper left third. ")
SCENES = {
    "win0": ("Behind him: a clear turquoise alpine lake, snowy mountain peaks, dark green pine forest, bright blue "
             "sky with white clouds, sunny midday.", (211, 212)),
    "win1": ("Behind him: a marsh at sunset, tall cattails and reeds, a moss-hung cypress, the sky glowing orange "
             "and purple, warm golden light on his face.", (221, 222)),
    "win2": ("At night: behind him a huge concrete dam wall with a row of glowing lamps on top, a full moon, "
             "dark blue water with moonlight reflections, a lantern on the boat lighting his face.", (231, 232)),
    "win3": ("Behind him: towering red sandstone canyon walls and mesas, deep green-blue water, warm late "
             "afternoon sun, a clear sky.", (241, 242)),
    "win4": ("Behind him: an autumn lake shore of red, orange and yellow maple trees, falling leaves in the air, "
             "soft golden light.", (251, 252)),
    "win5": ("Behind him: a fairy-tale castle on the far shore of a big blue lake, tournament flags and buoys, "
             "confetti in the air, a festive sunny day.", (261, 262)),
}
CHAMP = (STYLE + "Wide landscape. " + ANGLER[0].upper() + ANGLER[1:] + ", the tournament champion, kneels on the "
         "deck of his red bass boat lifting a HUGE trophy largemouth bass with both hands, water drops "
         "flying, laughing with joy, a shining golden trophy cup beside him on the deck, a sunny lake with pine "
         "forest behind, confetti. The angler and the fish in the middle, his face clearly visible.", (271, 272))
JOBS = {n: (STYLE + "Wide landscape. " + ANGLER[0].upper() + ANGLER[1:] + HOLD + sc, seeds) for n, (sc, seeds) in SCENES.items()}
JOBS["champ"] = CHAMP

if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    if "--pick" in sys.argv:
        a = sys.argv[sys.argv.index("--pick") + 1:]
        for name, k in zip(a[::2], a[1::2]):
            sd = JOBS[name][1][int(k) - 1]
            q.to565(q.fit(Image.open(os.path.join(OUT, "g15_%s_%d.png" % (name, sd))), 512, 300), os.path.join(IMG, name + ".565"))
            print("wrote", name, "from seed", sd)
        sys.exit(0)
    only = sys.argv[1:]
    for name, (prompt, seeds) in JOBS.items():
        if only and name not in only:
            continue
        for sd in seeds:
            p = os.path.join(OUT, "g15_%s_%d.png" % (name, sd))
            if not os.path.exists(p):
                q.generate(prompt, 1024, 608, sd).save(p)
            print(name, sd, "ok", flush=True)
