import os
"""Vertice Bass: landscape pack from Qwen-Image 2.1 — panoramas, water tiles, building/rock textures,
tree and plant billboards. Writes .565 into apps/bass/img and previews into the scratchpad."""
import sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tools"))
import numpy as np
import qwen_assets as q
from PIL import Image, ImageDraw

OUT = os.environ.get("ART_OUT", os.path.join(os.environ.get("TEMP", "/tmp"), "bass_art")) + "/"
IMG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "img") + "/"
STYLE = ("beautiful detailed 16-bit era arcade video game art, vivid colours, clean shapes, rich detail, "
         "painterly pixel art, no text, no people")
KEYC = (255, 0, 255)


def tileable(im, border=0.4):
    a = np.asarray(im.convert("RGB")).astype(np.float32)
    h, w, _ = a.shape
    b = np.roll(np.roll(a, h // 2, 0), w // 2, 1)
    y = np.abs(np.linspace(-1, 1, h))[:, None]
    x = np.abs(np.linspace(-1, 1, w))[None, :]
    m = np.clip(np.maximum(x, y) * 1.4 - border, 0, 1)[..., None]
    return Image.fromarray((a * (1 - m) + b * m).astype(np.uint8))


def wrap_x(im, blend=96):
    """Horizontal wrap for a 360-degree panorama: cross-fade the ends."""
    a = np.asarray(im.convert("RGB")).astype(np.float32)
    w = a.shape[1]
    out = a[:, :w - blend].copy()
    t = np.linspace(0, 1, blend)[None, :, None]
    out[:, :blend] = a[:, :blend] * t + a[:, w - blend:] * (1 - t)
    return Image.fromarray(out.astype(np.uint8))


def key_from_top(im, thresh=48):
    """Sky -> magenta: flood fill from every top-row pixel (the engine shows its sky gradient there)."""
    im = im.convert("RGB")
    for x in range(0, im.width, 3):
        if im.getpixel((x, 0)) != KEYC:
            ImageDraw.floodfill(im, (x, 0), KEYC, thresh=thresh)
    return im


def key_bg(im, thresh=60):
    im = im.convert("RGB")
    w, h = im.size
    for x in range(0, w, 4):
        for y in (0, h - 1):
            if im.getpixel((x, y)) != KEYC:
                ImageDraw.floodfill(im, (x, y), KEYC, thresh=thresh)
    for y in range(0, h, 4):
        for x in (0, w - 1):
            if im.getpixel((x, y)) != KEYC:
                ImageDraw.floodfill(im, (x, y), KEYC, thresh=thresh)
    return im


def texture_cell(cell, size):
    cell = cell.crop((14, 14, cell.width - 14, cell.height - 14)).resize((256, 256), Image.LANCZOS)
    return tileable(cell).resize((size, size), Image.LANCZOS)


def billboard(cell, w, h, crop_w=0.62):
    cell = cell.convert("RGB").crop((12, 12, cell.width - 12, cell.height - 12))
    cw = int(cell.width * crop_w)
    x0 = (cell.width - cw) // 2
    cell = cell.crop((x0, 0, x0 + cw, cell.height))
    small = cell.resize((w, h), Image.LANCZOS)
    k = key_bg(small, 60)
    # a keyed edge pixel next to the tree's colour can leave a dark rim: fine at this size
    return k


GRID = ("A 2x2 grid of four separate, equally sized square panels, separated by thin white borders. ")
TEX = GRID + ("Each panel is a seamless tileable texture, flat orthographic view, even lighting, no shadows, "
              "no objects, " + STYLE + ". ")
BB = GRID + ("Each panel on a plain flat deep navy blue background, " + STYLE + ". ")

grids = [
    ("water1", TEX + "Top left: clear alpine lake water surface seen from above, blue-green with gentle ripples and sparkles. "
     "Top right: lake water at sunset seen from above, golden orange and purple reflections with ripples. "
     "Bottom left: dark lake water at night seen from above, deep navy with silver moonlit ripples. "
     "Bottom right: turquoise green canyon river water seen from above, ripples and light caustics.", 81,
     [("w0", "tex", 128), ("w1", "tex", 128), ("w2", "tex", 128), ("w3", "tex", 128)]),
    ("water2", TEX + "Top left: dark teal autumn lake water seen from above with ripples and a few floating orange leaves. "
     "Top right: deep sapphire royal blue lake water seen from above with sparkling ripples. "
     "Bottom left: layered red sandstone canyon rock wall. Bottom right: grey medieval castle stone block wall.", 82,
     [("w4", "tex", 128), ("w5", "tex", 128), ("t_sand", "tex", 128), ("t_castle", "tex", 128)]),
    ("build", TEX + "Top left: weathered grey concrete dam wall with stains and seams. Top right: weathered wooden planks "
     "of a fishing pier. Bottom left: red clay roof tiles. Bottom right: lush green lakeside grass with small flowers.", 83,
     [("t_conc", "tex", 128), ("t_wood", "tex", 128), ("t_roof", "tex", 128), ("t_grass", "tex", 128)]),
    ("trees", BB + "Each panel shows one whole tree standing, side view, centred, tall, filling the panel height. "
     "Top left: a tall dark green pine fir tree. Top right: a tall pine fir tree covered in snow. "
     "Bottom left: a tall autumn maple tree with orange and red leaves. Bottom right: a tall swamp bald cypress tree with hanging moss.", 84,
     [("b_pine", "bb", (64, 128)), ("b_snow", "bb", (64, 128)), ("b_maple", "bb", (64, 128)), ("b_cypress", "bb", (64, 128))]),
    ("plants", BB + "Each panel shows one lakeside plant, side view, centred, filling the panel. "
     "Top left: a clump of tall green reeds and brown cattails. Top right: a round red and orange desert rock butte. "
     "Bottom left: a leafy green bush. Bottom right: a bare dead grey tree with twisted branches.", 85,
     [("b_reeds", "bb", (64, 64)), ("b_butte", "bb", (64, 64)), ("b_bush", "bb", (64, 64)), ("b_dead", "bb", (64, 64))]),
]

PANO = ("A very wide seamless panoramic landscape of the far shore of a lake, " + STYLE + ". The shoreline runs straight "
        "along the very bottom edge of the image, no water visible, the land rising from the bottom edge. ")
panos = [
    ("p0", PANO + "Alpine: dense green pine forest on the shore, behind it grey rocky mountains with snowy peaks, "
     "plain flat pale blue sky with a few small clouds.", 91),
    ("p1", PANO + "Marsh at sunset: reeds, willows and low trees on the shore, distant purple hills, "
     "plain flat orange sunset sky.", 92),
    ("p2", PANO + "Night: a dark forest shore, black hills, a concrete dam with small warm lights far away, "
     "plain flat dark navy night sky.", 93),
    ("p3", PANO + "Red canyon: red and orange sandstone cliffs, mesas and buttes, a few green shrubs at the bottom, "
     "plain flat light blue sky.", 94),
    ("p4", PANO + "Autumn: forest of orange, red and yellow trees on the shore, soft hills behind, "
     "plain flat pale warm sky.", 95),
    ("p5", PANO + "Royal lake: green forest shore, rolling hills, a distant white fairytale castle with blue roofs, "
     "plain flat bright blue sky.", 96),
]

only = sys.argv[1:]
for name, prompt, seed, cells in grids:
    if only and name not in only:
        continue
    img = q.generate(prompt, 1024, 1024, seed)
    img.save(OUT + "g8_" + name + ".png")
    for (n, kind, size), cell in zip(cells, q.split_grid(img)):
        if kind == "tex":
            t = texture_cell(cell, size)
        else:
            t = billboard(cell, size[0], size[1], 0.62 if size[1] > size[0] else 0.95)
        t.save(OUT + "g8c_" + n + ".png")
        q.to565(t, IMG + n + ".565")
    print(name, "ok", flush=True)

for name, prompt, seed in panos:
    if only and name not in only and "pano" not in only:
        continue
    img = q.generate(prompt, 2048, 512, seed)
    img.save(OUT + "g8_" + name + ".png")
    wide = img.convert("RGB").resize((1024 + 96, 256), Image.LANCZOS)
    wide = key_from_top(wrap_x(wide, 96), 44)          # wrap, key the sky from the top, then crop
    band = wide.crop((0, 256 - 128, wide.width, 256))
    band.save(OUT + "g8c_" + name + ".png")
    q.to565(band, IMG + name + ".565")
    print(name, "ok", flush=True)
