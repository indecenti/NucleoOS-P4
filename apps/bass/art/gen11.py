"""gen11.py - Vertice Bass: the trophy fish of the catch screen redone one by one, and the weigh-in.

The monster paintings (img/fish<N>_b.565, gen6.py) came from square 2x2 grid cells: shrunk into the
150x100 frame they ended up SMALLER than the regular fish, and some were off (a black catfish, a
perch for a zander). Here every fish is its own landscape picture (the frame's 3:2 shape) filling
the width, keyed at full resolution (eroded mask, premultiplied downsample: no navy fringe).
The weigh-in backdrop (img/weigh.565) had its scale cut at the top and a crowd of smudged faces:
now a dockside weigh station with the crowd far and small.

    python apps/bass/art/gen11.py                  # all; previews in %TEMP%/bass_art/g11_*.png
    python apps/bass/art/gen11.py fish3_b weigh    # only these
"""
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tools"))
import qwen_assets as q

IMG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "img")
OUT = os.environ.get("ART_OUT", os.path.join(os.environ.get("TEMP", "/tmp"), "bass_art"))
STYLE = ("A beautiful stylised 16-bit era arcade video game illustration, vivid colours, clean shapes, "
         "rich detail, no text, no letters. ")
FISH = ("{what}, in exact side view facing right, the whole fish from the tip of its nose to the end of its tail "
        "fin inside the picture and filling its width, on a plain flat deep navy blue background, nothing else, "
        "no water, no hook, no lure.")
BIG = "One huge fat trophy-sized old monster "
JOBS = {
    "fish0_b": (BIG + "largemouth bass, deep belly, dark olive green with a black side stripe, big mouth slightly open", 111),
    "fish1_b": (BIG + "rainbow trout, thick body, silver with a vivid pink-red side band and black spots, hooked jaw", 112),
    "fish2_b": (BIG + "northern pike, very long body, green with pale yellow spots, long duck-bill jaws with teeth", 113),
    "fish3_b": (BIG + "brown catfish, massive wide flat head, long whiskers, mottled brown and olive back, pale cream belly", 114),
    "fish4_b": (BIG + "common carp, deep body, golden bronze with large shiny scales, short barbels by the mouth", 115),
    "fish5_b": (BIG + "yellow perch, humped back, golden yellow with bold dark vertical bars, bright orange fins", 116),
    "fish6_b": (BIG + "zander, long slim body, silver-grey with faint dark bars, spiny dorsal fin, big glassy eyes and fangs", 117),
    "fish7_b": (BIG + "legendary golden bass, deep body, radiant shining gold scales, spiny dorsal fin, a few sparkles", 118),
    "fish4_s": ("One small young common carp, bronze, deep body with large scales, short barbels by the mouth", 121),
    "fish7_s": ("One small young golden bass, shiny gold scales, spiny dorsal fin", 122),
}
WEIGH = (STYLE + "A fishing tournament weigh-in station on a wooden lakeside dock on a sunny day: a big round dial "
         "hanging scale on a wooden gantry in the middle, with a large bass hanging from its hook, colourful pennant "
         "bunting, flags, a cheering crowd far away and small at the bottom, blue sky with white clouds, a lake and "
         "pine trees behind. The whole scale and the fish are inside the picture with sky above. Wide landscape view.", 131)


def key_fish(img, w=150, h=100, thresh=62, margin=3, size=1.0):
    """A keyed sprite from a picture on a flat background, the object centred and as big as fits."""
    c = img.convert("RGB").crop((8, 8, img.width - 8, img.height - 8))
    a = np.asarray(c).astype(np.int32)
    border = np.concatenate([a[0], a[-1], a[:, 0], a[:, -1]])
    bg = np.median(border, axis=0)
    fl = c.copy()
    sent = (255, 0, 255)
    seeds = [(x, y) for x in range(0, c.width, 6) for y in (0, c.height - 1)] + \
            [(x, y) for y in range(0, c.height, 6) for x in (0, c.width - 1)]
    for x, y in seeds:
        if np.sqrt(((a[y, x] - bg) ** 2).sum()) < thresh and fl.getpixel((x, y)) != sent:
            ImageDraw.floodfill(fl, (x, y), sent, thresh=thresh)
    f = np.asarray(fl).astype(np.int32)
    bgm = (f[..., 0] == 255) & (f[..., 1] == 0) & (f[..., 2] == 255)
    m = Image.fromarray(((~bgm) * 255).astype(np.uint8)).filter(ImageFilter.MedianFilter(5)).filter(ImageFilter.MinFilter(3))
    ma = np.asarray(m) > 127
    ys, xs = np.nonzero(ma)
    x0, x1, y0, y1 = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1
    bw, bh = x1 - x0, y1 - y0
    k = min((w - 2 * margin) / bw, (h - 2 * margin) / bh) * size   # size < 1: a small fish, drawn small
    tw, th = max(1, int(bw * k)), max(1, int(bh * k))
    rgb = a[y0:y1, x0:x1].astype(np.float32) * ma[y0:y1, x0:x1, None]
    al = ma[y0:y1, x0:x1].astype(np.float32)
    rs = lambda arr: np.asarray(Image.fromarray(arr.astype(np.float32)).resize((tw, th), Image.BOX))
    pa = rs(al)
    pc = np.stack([rs(rgb[..., i]) for i in range(3)], axis=2) / np.maximum(pa[..., None], 1e-3)
    out = np.zeros((h, w, 3), np.uint8)
    out[:] = (255, 0, 255)
    col = np.clip(pc, 0, 255).astype(np.uint8)
    col[..., 1] = np.where((col[..., 0] >= 248) & (col[..., 1] < 4) & (col[..., 2] >= 248), 4, col[..., 1])
    ox, oy = (w - tw) // 2, (h - th) // 2
    keep = pa > 0.5
    out[oy:oy + th, ox:ox + tw][keep] = col[keep]
    return Image.fromarray(out)


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    only = sys.argv[1:]
    for name, (what, seed) in JOBS.items():
        if only and name not in only:
            continue
        src = os.path.join(OUT, "g11_%s_%d.png" % (name, seed))
        if not os.path.exists(src):
            q.generate(STYLE + FISH.format(what=what), 1024, 672, seed).save(src)
        sp = key_fish(Image.open(src), size=0.62 if name.endswith("_s") else 1.0)   # like gen6's small ones
        sp.save(os.path.join(OUT, "g11_%s_key.png" % name))
        q.to565(sp, os.path.join(IMG, name + ".565"))
        print(name, "ok", flush=True)
    if not only or "weigh" in only:
        prompt, seed = WEIGH
        src = os.path.join(OUT, "g11_weigh_%d.png" % seed)
        if not os.path.exists(src):
            q.generate(prompt, 1024, 608, seed).save(src)
        q.to565(q.fit(Image.open(src), 512, 300), os.path.join(IMG, "weigh.565"))
        print("weigh ok", flush=True)
