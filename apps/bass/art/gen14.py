"""gen14.py - Vertice Bass: the title backdrop (img/title.565) and the intro's champion (img/intro3.565)
redone sharper, in the style of win.565 / lose.565 and with the same angler (tan cap, white t-shirt,
olive fishing vest, red bass boat).

title: the angler casting from his boat at dawn on the LEFT, the right two thirds calm water and sky
(the logo, the subtitle and the menu sit there). intro3: the champion lifting a golden trophy with
fireworks, the WHOLE trophy in the picture with sky above it (the old one cut it at the top).

    python apps/bass/art/gen14.py                     # previews in %TEMP%/bass_art/g14_<name>_<seed>.png
    python apps/bass/art/gen14.py --pick title 2 intro3 1
"""
import os
import sys

from PIL import Image

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tools"))
import qwen_assets as q

IMG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "img")
OUT = os.environ.get("ART_OUT", os.path.join(os.environ.get("TEMP", "/tmp"), "bass_art"))
STYLE = ("A beautiful stylised 16-bit era arcade video game illustration, detailed crisp pixel-art painting, "
         "vivid colours, clean shapes, sharp details, no text, no letters, no logo. ")
ANGLER = ("a young angler with brown hair, a tan baseball cap, a white t-shirt and an olive green fishing vest")
JOBS = {
    "title": (STYLE + "Wide landscape. At dawn on a misty lake, " + ANGLER + " stands in his red bass boat and casts "
              "a long fishing rod, the line arcing through the air. The boat and the angler are small-to-medium "
              "sized on the LEFT third of the picture, full body, seen from the side. The right two thirds of "
              "the picture are open: calm golden water reflecting a soft orange and pink sunrise sky, a misty "
              "pine forest along the far shore, a few birds. Peaceful, inviting, high detail.", (181, 182, 183)),
    "intro3": (STYLE + "Wide landscape. Victory celebration at night: " + ANGLER + ", grinning with joy, lifts a big "
               "shining golden trophy cup high above his head with both hands, standing on the deck of his red "
               "bass boat at a lakeside tournament stage. Colourful fireworks bursting in the night sky, stage "
               "spotlights, confetti, a cheering crowd small in the background. The WHOLE trophy and his raised "
               "arms are inside the picture, with clear night sky above the trophy. The angler in the middle, "
               "from the waist up.", (191, 192, 193)),
}

if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    if "--pick" in sys.argv:
        a = sys.argv[sys.argv.index("--pick") + 1:]
        for name, k in zip(a[::2], a[1::2]):
            sd = JOBS[name][1][int(k) - 1]
            q.to565(q.fit(Image.open(os.path.join(OUT, "g14_%s_%d.png" % (name, sd))), 512, 300), os.path.join(IMG, name + ".565"))
            print("wrote", name, "from seed", sd)
        sys.exit(0)
    only = sys.argv[1:]
    for name, (prompt, seeds) in JOBS.items():
        if only and name not in only:
            continue
        for sd in seeds:
            p = os.path.join(OUT, "g14_%s_%d.png" % (name, sd))
            if not os.path.exists(p):
                q.generate(prompt, 1024, 608, sd).save(p)
            print(name, sd, "ok", flush=True)
