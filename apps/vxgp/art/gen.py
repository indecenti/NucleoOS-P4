"""Vertice GP: art and music from the local models (Qwen-Image 2.1 + ACE-Step 1.5 in ComfyUI).

Same prompt + same seed = same asset, so this script *is* the recipe. Writes apps/vxgp/img/*.565 and
apps/vxgp/snd/*.wav; previews go to ART_OUT (default %TEMP%/vxgp_art).
Run all:   python apps/vxgp/art/gen.py   (road needs ground; menu_split needs music_menu)
Run some:  python apps/vxgp/art/gen.py title pano0 music_menu
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
IMG = os.path.join(HERE, "..", "img")
SND = os.path.join(HERE, "..", "snd")
PREVIEW = os.environ.get("ART_OUT", os.path.join(os.environ.get("TEMP", "/tmp"), "vxgp_art"))
TOOL = [sys.executable, os.path.join(ROOT, "tools", "game_assets.py"), "--preview", PREVIEW]

STEPS = {
    # full-screen title painting: karts low and centre, the sky free for the logo
    "title": ["art", os.path.join(IMG, "title.565"), "--seed", "11", "--scene",
              "four small colourful cartoon go-karts (red, blue, green, orange) racing side by side toward the "
              "viewer on a sunny asphalt race circuit, low dynamic angle, red and white kerbs, a grandstand and "
              "chequered flags far behind, the karts in the lower half, a big clear blue sky with a few white "
              "clouds filling the upper half"],
    # the three circuits plus the podium, as cards for the menus
    "cards": ["grid", IMG, "--kind", "art", "--size", "256x150", "--seed", "23",
              "--names", "card0,card1,card2,podium", "--cells",
              "a kart race circuit winding through green meadows and pine woods, rolling hills, blue lakes, sunny day",
              "a kart race circuit through a red rock desert canyon, tall mesas, cactus, dusty warm afternoon light",
              "a kart race circuit in snowy alpine mountains, snow banks, snowy pine trees, crisp blue sky",
              "a winners podium with a golden trophy, confetti and fireworks at a kart race track at dusk"],
    # driver portraits (keyed sprites) for the driver select and the results
    "drivers": ["grid", IMG, "--kind", "sprite", "--size", "96x96", "--seed", "37",
                "--names", "drv0,drv1,drv2,drv3",
                "--subject", "the head and shoulders portrait of one cheerful cartoon kart racer, facing the viewer, "
                             "centred, filling the panel",
                "--cells",
                "a boy with a red racing helmet, visor up, red racing suit, big grin",
                "a girl with a blue racing helmet, visor up, blue racing suit, confident smile",
                "a bearded man with a green racing helmet, visor up, green racing suit, thumbs up",
                "a girl with an orange racing helmet, visor up, orange racing suit, winking"],
    # trackside props + the coin and the trophy
    "props": ["grid", IMG, "--kind", "sprite", "--size", "64x64", "--seed", "52",
              "--names", "coin,tyres,hay,trophy",
              "--subject", "one single object, centred, whole object visible",
              "--cells", "a shiny gold coin with an embossed star, seen from the front",
              "a stack of black rubber tyres painted with red and white stripes",
              "a round golden hay bale", "a golden winners trophy cup with two handles"],
    # ground textures (Mode-7 floor) and the base grain for the asphalt
    "ground": ["grid", IMG, "--kind", "tex", "--size", "128x128", "--seed", "61",
               "--names", "g_grass,g_sand,g_snow,g_asphalt",
               "--cells", "short mowed green grass lawn", "warm orange desert sand with small pebbles",
               "fresh white snow with soft blue shadows", "dark grey road asphalt with fine grain"],
    # music
    "music_menu": ["music", os.path.join(SND, "menu.wav"), "--seconds", "30", "--bpm", "128", "--key", "E major",
                   "--seed", "5", "--tags", "upbeat kart racing game title theme, funky slap bass, bright synth "
                                            "brass, electric guitar riffs, energetic drums, loopable"],
    "music_win": ["music", os.path.join(SND, "win.wav"), "--seconds", "7", "--bpm", "120", "--key", "C major",
                  "--seed", "21", "--tags", "short triumphant victory fanfare, brass, drum roll, cymbal crash"],
    "music_lose": ["music", os.path.join(SND, "finish.wav"), "--seconds", "6", "--bpm", "100", "--key", "G major",
                   "--seed", "22", "--tags", "short friendly race finished jingle, synth, light drums"],
}


def post_road():
    """road.565: the painted asphalt grain + the circuit markings (u across the road, v along it):
    white edge lines, two darker rubbered-in racing lines, a dashed centre line."""
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import qwen_assets as q
    from PIL import Image
    import numpy as np
    src = os.path.join(PREVIEW, "g_asphalt.png")
    a = np.asarray(Image.open(src).convert("RGB").resize((128, 128), Image.LANCZOS)).astype(np.float32)
    g = a.mean(axis=2, keepdims=True)
    a = a * 0.35 + g * 0.65                                   # desaturate: grey road
    a = (a - a.mean()) * 0.8 + 100                            # mid-grey, gentle grain
    x = np.arange(128)
    band = ((x > 24) & (x < 44)) | ((x > 84) & (x < 104))
    a[:, band] *= 0.88
    a[:, (x <= 6) | (x >= 121)] = (236, 236, 230)
    a[:56, 62:66] = (245, 232, 180)
    q.to565(Image.fromarray(np.clip(a, 0, 255).astype(np.uint8)), os.path.join(IMG, "road.565"))
    print("wrote road.565")


def post_menu():
    """menu.wav -> menu0..5.wav (5 s parts): the game queues them one by one, so the race can take the
    speaker within one part (the OS plays one stream at a time)."""
    import wave
    src = os.path.join(SND, "menu.wav")
    with wave.open(src, "rb") as w:
        rate, ch, sw = w.getframerate(), w.getnchannels(), w.getsampwidth()
        data = w.readframes(w.getnframes())
    part = rate * 5 * ch * sw
    for i in range(6):
        chunk = data[i * part:(i + 1) * part]
        if not chunk:
            break
        with wave.open(os.path.join(SND, "menu%d.wav" % i), "wb") as o:
            o.setnchannels(ch); o.setsampwidth(sw); o.setframerate(rate)
            o.writeframes(chunk)
    os.remove(src)                                            # only the parts ship
    print("wrote menu0..5.wav")


TREE_PROMPT = (
    "A 2x2 grid of four separate, equally sized square panels, separated by thin white borders. "
    "Each panel on a plain flat solid deep navy blue background, beautiful detailed 16-bit era arcade video "
    "game art, vivid colours, clean shapes, painterly pixel art, no text. Each panel shows one single whole "
    "tree standing alone, side view, centred, the crown fully inside the panel with navy sky around it, the "
    "trunk reaching the bottom of the panel, no ground, no grass, no shadow on the ground, crisp clean outline. "
    "Top left: %s. Top right: %s. Bottom left: %s. Bottom right: %s.")
TREES = [("tr_pine", "a tall dark green pine fir with layered branches"),
         ("tr_oak", "a round bushy green oak tree with a thick brown trunk"),
         ("tr_cactus", "a tall green saguaro cactus with two arms raised"),
         ("tr_snow", "a dark green pine fir with thick white snow on its branches")]


def cutout(cell, w, h, thresh=62):
    """A clean keyed billboard from one grid cell: the background is found at full resolution (flood
    from the borders + near-background pockets), the mask is eroded one pixel so no navy fringe
    survives, colours are downsampled premultiplied by the mask (edges keep the tree's own colour),
    and transparent texels are exact magenta with a clear 1-texel frame at top and sides (the engine
    samples with clamp, and a trunk row can never show above the crown)."""
    from PIL import Image, ImageDraw, ImageFilter
    import numpy as np
    c = cell.convert("RGB").crop((18, 18, cell.width - 18, cell.height - 18))
    a = np.asarray(c).astype(np.int32)
    border = np.concatenate([a[0], a[-1], a[:, 0], a[:, -1]])
    bg = np.median(border, axis=0)
    # flood fill from every border pixel on a copy marked with a sentinel colour
    fl = c.copy()
    sent = (255, 0, 255)
    seeds = [(x, y) for x in range(0, c.width, 6) for y in (0, c.height - 1)] +             [(x, y) for y in range(0, c.height, 6) for x in (0, c.width - 1)]
    for x, y in seeds:                    # only from background-coloured border pixels: a bust or a
        p = a[y, x]                       # trunk touching the edge must not be flooded
        if np.sqrt(((p - bg) ** 2).sum()) < thresh and fl.getpixel((x, y)) != sent:
            ImageDraw.floodfill(fl, (x, y), sent, thresh=thresh)
    f = np.asarray(fl).astype(np.int32)
    bgm = (f[..., 0] == 255) & (f[..., 1] == 0) & (f[..., 2] == 255)
    d = np.sqrt(((a - bg) ** 2).sum(axis=2))
    bgm |= d < thresh * 0.6                                   # enclosed sky between branches
    m = Image.fromarray(((~bgm) * 255).astype(np.uint8))
    m = m.filter(ImageFilter.MedianFilter(5)).filter(ImageFilter.MinFilter(3))   # de-speckle, erode
    ma = np.asarray(m) > 127
    ys, xs = np.nonzero(ma)
    x0, x1, y0, y1 = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1
    # fit the object into (w-2) x (h-1): 1 texel free at the sides and the top, trunk on the bottom row
    bw, bh = x1 - x0, y1 - y0
    k = min((w - 2) / bw, (h - 1) / bh)
    tw, th = max(1, int(bw * k)), max(1, int(bh * k))
    rgb = a[y0:y1, x0:x1].astype(np.float32) * ma[y0:y1, x0:x1, None]
    al = ma[y0:y1, x0:x1].astype(np.float32)
    rs = lambda arr: np.asarray(Image.fromarray(arr.astype(np.float32)).resize((tw, th), Image.BOX))
    pa = rs(al)
    pc = np.stack([rs(rgb[..., i]) for i in range(3)], axis=2) / np.maximum(pa[..., None], 1e-3)
    out = np.zeros((h, w, 3), np.uint8); out[:] = (255, 0, 255)
    ox, oy = (w - tw) // 2, h - th
    keep = pa > 0.5
    col = np.clip(pc, 0, 255).astype(np.uint8)
    col[..., 1] = np.where((col[..., 0] >= 248) & (col[..., 1] < 4) & (col[..., 2] >= 248), 4, col[..., 1])
    region = out[oy:oy + th, ox:ox + tw]
    region[keep] = col[keep]
    return Image.fromarray(out)


def post_trees(seed=84):
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import qwen_assets as q
    img = q.generate(TREE_PROMPT % tuple(t[1] for t in TREES), 1024, 1024, seed)
    os.makedirs(PREVIEW, exist_ok=True)
    img.save(os.path.join(PREVIEW, "grid_trees.png"))
    for (name, _), cell in zip(TREES, q.split_grid(img)):
        t = cutout(cell, 128, 256)
        t.save(os.path.join(PREVIEW, name + ".png"))
        q.to565(t, os.path.join(IMG, name + ".565"))
        print("wrote", name)


def post_drivers():
    """Re-cut the driver portraits from the saved grid with the careful cutout (the busts touch the
    bottom edge, the generic keying flooded into the suits)."""
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import qwen_assets as q
    from PIL import Image
    grid = Image.open(os.path.join(PREVIEW, "grid_drv0.png"))
    for i, cell in enumerate(q.split_grid(grid)):
        t = cutout(cell, 96, 96)
        t.save(os.path.join(PREVIEW, "drv%d.png" % i))
        q.to565(t, os.path.join(IMG, "drv%d.565" % i))
    print("wrote drv0..3")


PANOS = [  # (name, seed, sky colour of the circuit's vx_sky bottom, scene)
    ("pano0", 91, "green rolling hills with round trees and dark pine woods, distant blue mountains with a "
                  "little snow"),
    ("pano1", 92, "red rock desert mesas and buttes, orange canyon walls, a few saguaro cactus"),
    ("pano2", 93, "high snowy alpine peaks and glaciers, snowy pine forest at their foot"),
]
PANO_LAND = 52        # rows of land in the 1024x128 band (the engine draws ~1.5 px per row)


def post_panos(only=None):
    """360-degree horizon bands: a low landscape under a big empty sky, wrapped, the sky keyed from
    the top, the foreground strip trimmed, then scaled so the land is PANO_LAND rows tall on the
    bottom of a 1024x128 band (magenta above: the engine's sky gradient shows through)."""
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import qwen_assets as q
    import game_assets as ga
    from PIL import Image
    import numpy as np
    for name, seed, scene in PANOS:
        if only and name not in only:
            continue
        prompt = ("A very wide seamless panoramic horizon, " + ga.STYLE + ". A low distant landscape "
                  "only along the bottom third of the image, its base running straight along the very "
                  "bottom edge, no foreground, no road, no people. A huge plain empty clear pale sky, "
                  "one flat colour with no clouds, fills the upper two thirds. " + scene)
        img = q.generate(prompt, 2048, 512, seed)
        img.save(os.path.join(PREVIEW, name + "_full.png"))
        wide = img.convert("RGB").resize((1024 + 96, 280), Image.LANCZOS)
        wide = ga.key_top(ga.wrap_x(wide, 96), 40)
        a = np.asarray(wide)
        land = ~((a[..., 0] == 255) & (a[..., 1] == 0) & (a[..., 2] == 255))
        tops = np.array([np.argmax(col) if col.any() else a.shape[0] for col in land.T])
        top = int(np.percentile(tops, 3))                      # the highest peaks (ignore specks)
        bottom = int(a.shape[0] * 0.96)                        # trim the foreground strip
        band = wide.crop((0, top, wide.width, bottom))
        h = min(PANO_LAND, band.height)
        band = band.resize((1024, h), Image.NEAREST)          # nearest: the magenta key stays exact
        out = Image.new("RGB", (1024, 128), (255, 0, 255))
        out.paste(band, (0, 128 - h - 4))                      # 4 rows below the horizon (row 124)
        out.save(os.path.join(PREVIEW, name + ".png"))
        q.to565(out, os.path.join(IMG, name + ".565"))
        print("wrote", name, "land rows", h, "(from", bottom - top, ")")


def post_ground_soft():
    """The Mode-7 floor has no mipmaps: high-contrast grain shimmers in the distance. Pull the ground
    textures toward their mean colour and soften them a touch."""
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import qwen_assets as q
    from PIL import Image, ImageFilter
    import numpy as np
    for n, k in (("g_grass", 0.45), ("g_sand", 0.7), ("g_snow", 0.8)):
        im = Image.open(os.path.join(PREVIEW, n + ".png")).convert("RGB").filter(ImageFilter.GaussianBlur(0.7))
        a = np.asarray(im).astype(np.float32)
        a = a.mean(axis=(0, 1)) + (a - a.mean(axis=(0, 1))) * k
        q.to565(Image.fromarray(np.clip(a, 0, 255).astype(np.uint8)), os.path.join(IMG, n + ".565"))
    print("softened ground")


POST = {"ground_soft": post_ground_soft, "panos": post_panos, "road": post_road, "menu_split": post_menu, "trees": post_trees, "drivers_cut": post_drivers}


def main():
    os.makedirs(IMG, exist_ok=True)
    os.makedirs(SND, exist_ok=True)
    names = sys.argv[1:] or list(STEPS) + list(POST)
    failed = []
    for n in names:
        print("==", n, flush=True)
        if n in POST:
            POST[n]()
        elif subprocess.call(TOOL + STEPS[n]) != 0:
            failed.append(n)
    if failed:
        print("FAILED:", " ".join(failed))
        sys.exit(1)


if __name__ == "__main__":
    main()
