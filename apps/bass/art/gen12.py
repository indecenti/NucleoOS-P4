"""gen12.py - Vertice Bass: the "not qualified" painting (img/lose.565), the twin of win.565.

The old one was a crude low-detail sprite with a warped face, in a style of its own. This is the
same angler as the win picture (tan cap, white tee, olive fishing vest, red bass boat), in the same
painterly 16-bit style, at dusk with nothing in his hands. Three seeds; the pick goes to img/.

    python apps/bass/art/gen12.py              # previews in %TEMP%/bass_art/g12_lose_<seed>.png
    python apps/bass/art/gen12.py --pick 2     # write the chosen one
"""
import os
import sys

from PIL import Image

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tools"))
import qwen_assets as q

IMG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "img")
OUT = os.environ.get("ART_OUT", os.path.join(os.environ.get("TEMP", "/tmp"), "bass_art"))
PROMPT = ("A beautiful stylised 16-bit era arcade video game illustration, detailed pixel-art painting, vivid "
          "colours, clean shapes, no text, no letters. A young angler with brown hair, a tan baseball cap, a "
          "white t-shirt and an olive green fishing vest sits in his red bass boat on a lake at dusk, "
          "disappointed: shoulders slumped, chin resting on his hand, looking down at his empty landing net, "
          "his fishing rod leaning on the side. Purple and orange sunset sky with clouds, dark pine forest on "
          "the far shore, calm water reflecting the sky. Medium shot, the angler in the middle of the picture "
          "with his face in the upper half, a well drawn natural face. Wide landscape composition.")
SEEDS = (141, 142, 143)

if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    if "--pick" in sys.argv:
        k = int(sys.argv[sys.argv.index("--pick") + 1])
        img = Image.open(os.path.join(OUT, "g12_lose_%d.png" % SEEDS[k - 1]))
        q.to565(q.fit(img, 512, 300), os.path.join(IMG, "lose.565"))
        print("wrote lose from seed", SEEDS[k - 1])
        sys.exit(0)
    for sd in SEEDS:
        p = os.path.join(OUT, "g12_lose_%d.png" % sd)
        if not os.path.exists(p):
            q.generate(PROMPT, 1024, 608, sd).save(p)
        print("lose", sd, "ok", flush=True)
