"""icon.py - Vertice Bass: the launcher / store icon (icon.z = raw-deflated 80x80 BGRA).

A painted bass leaping out of the lake with a lure in its jaw, against a sunset, in the game's
16-bit painting style; bold shapes so it still reads at 80 pixels. Rounded corners like the other
NucleoOS icons. Three seeds; the pick is written to icon.z (and art/icon.png as a preview).

    python apps/bass/art/icon.py              # previews in %TEMP%/bass_art/icon_<seed>.png
    python apps/bass/art/icon.py --pick 1
"""
import os
import sys
import zlib

from PIL import Image, ImageDraw, ImageEnhance, ImageFilter

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tools"))
import qwen_assets as q

APP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
OUT = os.environ.get("ART_OUT", os.path.join(os.environ.get("TEMP", "/tmp"), "bass_art"))
PROMPT = ("A video game app icon artwork, square, a beautiful stylised 16-bit era arcade illustration with bold "
          "clean shapes and strong outlines, very high contrast, no text, no letters, no border. A big green "
          "largemouth bass leaping out of the water toward the viewer, body arched, mouth wide open with a red "
          "and white crankbait lure in its jaw, a white splash of water around it, a huge glowing orange sun "
          "and a golden sunset sky behind, deep blue water at the bottom. The fish fills most of the square "
          "and sits in the centre.")
SEEDS = (151, 152, 153)
SIZE, RADIUS = 80, 16


def make(img):
    m = img.width * 6 // 100                                  # the painting's own rounded corners (white) cut off
    im = img.convert("RGB").crop((m, m, img.width - m, img.height - m)).resize((SIZE * 4, SIZE * 4), Image.LANCZOS)
    im = ImageEnhance.Color(im).enhance(1.15)
    im = im.resize((SIZE, SIZE), Image.LANCZOS).filter(ImageFilter.UnsharpMask(radius=1, percent=60, threshold=2))
    mask = Image.new("L", (SIZE * 4, SIZE * 4), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, SIZE * 4 - 1, SIZE * 4 - 1), RADIUS * 4, fill=255)
    mask = mask.resize((SIZE, SIZE), Image.LANCZOS)
    out = im.convert("RGBA")
    out.putalpha(mask)
    return out


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    if "--pick" in sys.argv:
        sd = SEEDS[int(sys.argv[sys.argv.index("--pick") + 1]) - 1]
        icon = make(Image.open(os.path.join(OUT, "icon_%d.png" % sd)))
        icon.save(os.path.join(APP, "art", "icon.png"))
        co = zlib.compressobj(9, zlib.DEFLATED, -15)
        with open(os.path.join(APP, "icon.z"), "wb") as f:
            f.write(co.compress(icon.tobytes("raw", "BGRA")) + co.flush())
        print("wrote icon.z from seed", sd)
        sys.exit(0)
    for sd in SEEDS:
        p = os.path.join(OUT, "icon_%d.png" % sd)
        if not os.path.exists(p):
            q.generate(PROMPT, 1024, 1024, sd).save(p)
        make(Image.open(p)).save(os.path.join(OUT, "icon80_%d.png" % sd))
        print("icon", sd, "ok", flush=True)
