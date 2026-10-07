"""gen16.py - Vertice Bass: painted sky and far range, one 360-degree panorama per lake (img/s0..s5.565).

The engine draws it with vx_panorama twice round the horizon (Vertice 1.4: PANO_HR | 2 << 12), so the
1024-wide texture covers 180 degrees at ~1.4 screen pixels a texel, with square texels. The source
(2048x512) is therefore scaled the SAME in both directions and cropped to the band near the horizon -
the only part the game's camera, looking down at the water, ever shows - instead of being squashed
(squashing flattened the clouds and peaks into streaks). The top rows fade into the lake's sky colour
(its vx_sky top). The parallax comes from the 3D shore (trees, reeds, rocks) sliding over this backdrop,
which, like real far mountains, stays put as the boat moves.

    python apps/bass/art/gen16.py              # previews in %TEMP%/bass_art/g16_*.png
    python apps/bass/art/gen16.py s2 s4        # only these
"""
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tools"))
import qwen_assets as q

IMG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "img")
OUT = os.environ.get("ART_OUT", os.path.join(os.environ.get("TEMP", "/tmp"), "bass_art"))
STYLE = ("a beautiful stylised 16-bit era arcade video game background painting, detailed crisp pixel-art, "
         "sharp clean shapes, vivid colours, no text, no people, no boats, no water")
PANO = ("A very wide seamless 360-degree panorama seen from the middle of a lake, " + STYLE + ". The far shore is a "
        "straight line along the very bottom edge. In the bottom third: ")
LAKES = [  # (far range, sky, seed, the lake's sky_top colour in lake.c)
    ("grey rocky alpine mountains with snowy peaks and glaciers, dark pine forest on the lower slopes",
     "a bright blue sky with big white fluffy cumulus clouds, sunny midday", 321, (40, 110, 220)),
    ("low rolling purple and violet hills with scattered dark trees in warm sunset light",
     "a glowing orange, pink and purple sunset sky with long streaks of lit clouds, the low sun on the horizon",
     322, (70, 60, 140)),
    ("dark blue night mountains with moonlit edges, a concrete dam with a row of tiny warm lights",
     "a deep navy night sky full of stars, a big bright full moon with a soft halo, a few thin silver clouds",
     323, (8, 12, 40)),
    ("red and orange sandstone mesas, buttes and cliffs with layered stripes",
     "a warm clear blue sky with a few high wispy clouds, golden afternoon light", 324, (60, 110, 200)),
    ("rolling hills covered in an autumn forest of orange, red and yellow trees, a misty blue ridge behind",
     "a soft pale blue sky with warm golden clouds, a hazy autumn afternoon", 325, (70, 120, 200)),
    ("green wooded hills and blue distant mountains, a white fairy-tale castle with blue roofs on one hilltop",
     "a bright blue sky with big white clouds and a few birds", 326, (30, 90, 200)),
]
W, H, WRAP, FADE = 1024, 128, 96, 26


def wrap_x(a, blend):
    out = a[:, :a.shape[1] - blend].copy()
    t = np.linspace(0, 1, blend)[None, :, None]
    out[:, :blend] = a[:, :blend] * t + a[:, -blend:] * (1 - t)
    return out


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    only = sys.argv[1:]
    for n, (far, sky, seed, top) in enumerate(LAKES):
        name = "s%d" % n
        if only and name not in only:
            continue
        src = os.path.join(OUT, "g16_%s_%d.png" % (name, seed))
        if not os.path.exists(src):
            q.generate(PANO + far + "; above them " + sky + ".", 2048, 512, seed).save(src)
        img = Image.open(src).convert("RGB")
        # Qwen paints a strip of lake along the bottom despite the prompt: find where the water ends
        # (rows that are blue, smooth and flat) and cut it, so the band starts at the foot of the range.
        a0 = np.asarray(img).astype(np.float32)
        y = a0.shape[0] - 1
        while y > a0.shape[0] * 0.6:
            row = a0[y]
            blue = (row[:, 2] > row[:, 0] + 25).mean() > 0.85 and (row[:, 2] > row[:, 1] - 5).mean() > 0.8
            if not blue:
                break
            y -= 1
        img = img.crop((0, 0, img.width, y + 1))
        # One scale across, a little less down (x1.5): the game's camera only shows ~50 px above the
        # treeline, and squared texels left the range above it; squashed 4x it read as streaks.
        k = (W + WRAP) / img.width
        a = np.asarray(img.resize((W + WRAP, round(img.height * k / 1.5)), Image.LANCZOS)).astype(np.float32)
        a = wrap_x(a[-H:], WRAP)                                 # the band at the horizon
        t = np.clip(np.arange(H) / FADE, 0, 1)[:, None, None]
        t = t * t * (3 - 2 * t)                                  # top rows -> the engine's sky colour
        a = a * t + np.array(top, np.float32)[None, None, :] * (1 - t)
        a[(a[..., 0] > 247) & (a[..., 1] < 8) & (a[..., 2] > 247)] -= 8      # never the magenta key
        im = Image.fromarray(np.clip(a, 0, 255).astype(np.uint8))
        im.save(os.path.join(OUT, "g16_%s.png" % name))
        q.to565(im, os.path.join(IMG, name + ".565"))
        print(name, "ok, water cut at row", y, flush=True)
