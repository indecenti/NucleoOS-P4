"""gen9.py - Vertice Bass 2.0 art: wide full-screen scenes (generated at the screen's aspect, so
nothing is cropped away), painted underwater plants, and high-resolution fish for the 3D skins.

    python apps/bass/art/gen9.py [job names]      # previews in %TEMP%/bass_art (or ART_OUT)

Jobs
  win_a, win_b   the angler showing off a trophy bass, the whole fish in frame (1024x576)
  weeds          four underwater plants on navy, for billboards (2x2 grid)
  fishhd1/2      the eight species, side view, nose right, large (2x2 grids, 512 px panels)
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tools"))
import qwen_assets as q

OUT = os.environ.get("ART_OUT", os.path.join(os.environ.get("TEMP", "/tmp"), "bass_art")) + "/"
os.makedirs(OUT, exist_ok=True)
STYLE = ("A beautiful stylised 16-bit era arcade video game illustration, vivid colours, clean shapes, "
         "rich detail, no text, no letters, no logo.")
NAVY = "each panel on a plain flat deep navy blue background, nothing else in the panel"

WIDE = {
    "win_a": (11,
              "Wide landscape painting. " + STYLE + " A cheerful bass angler in a cap and fishing vest standing in a red "
              "bass boat on a sunny lake, holding a huge largemouth bass horizontally in front of his chest with both "
              "hands, the WHOLE fish clearly visible from its open mouth to its tail fin, the fish in the middle of the "
              "picture, plenty of blue sky above, confetti, green forest shore behind, the angler on the left half."),
    "win_b": (23,
              "Wide landscape painting. " + STYLE + " A happy bass fishing champion on the deck of a bass boat lifting a "
              "giant largemouth bass at arm's length, the entire fish fully inside the picture with space around it, "
              "mouth open, water drops sparkling, sunny blue sky with a few clouds, pine forest shore, celebration."),
}
GRIDS = {
    "weeds": (["g_eel", "g_milfoil", "g_cabbage", "g_stems"], 64,
              "A 2x2 grid of four separate, equally sized square panels, separated by thin white borders, " + NAVY + ". " + STYLE +
              " Each panel shows ONE underwater freshwater plant growing up from the bottom edge of the panel to near "
              "its top, side view, centred. Top left: tall wavy ribbons of eelgrass, bright green. Top right: feathery "
              "water milfoil, olive green stems with fine leaves. Bottom left: broad-leaved cabbage pondweed, "
              "yellow-green. Bottom right: long thin lily stems rising to the surface, reddish brown and green."),
    "angler": (["an_idle", "an_wind", "an_cast", "an_reel"], 81,
               "A 2x2 grid of four separate, equally sized square panels, separated by thin white borders, " + NAVY + ". " + STYLE +
               " The SAME bass fisherman in all four panels, seen exactly from BEHIND (we see his back), full body from cap to "
               "boots, standing, centred, filling the panel height: blue fishing vest over a white shirt, navy cap, khaki "
               "trousers, holding a long black fishing rod with a spinning reel. Top left: standing relaxed, rod held "
               "forward and up at 45 degrees. Top right: winding up a cast, the rod swung back over his right shoulder. "
               "Bottom left: just after the cast, both arms stretched forward, the rod pointing far out ahead. "
               "Bottom right: reeling in, the rod held low to his right side, left hand on the reel handle."),
    "fishhd1": (["hd0", "hd1", "hd2", "hd3"], 71,
                "A 2x2 grid of four separate, equally sized square panels, separated by thin white borders, " + NAVY + ". " + STYLE +
                " Each panel shows ONE whole fish, exact side view, facing right, horizontal, filling the panel width, "
                "every fin and the tail inside the panel. Top left: a largemouth bass, green with a dark lateral stripe. "
                "Top right: a rainbow trout, silver with a pink band and black spots. Bottom left: a northern pike, long, "
                "olive green with pale spots. Bottom right: a brown catfish with long whiskers."),
    "fishhd2": (["hd4", "hd5", "hd6", "hd7"], 72,
                "A 2x2 grid of four separate, equally sized square panels, separated by thin white borders, " + NAVY + ". " + STYLE +
                " Each panel shows ONE whole fish, exact side view, facing right, horizontal, filling the panel width, "
                "every fin and the tail inside the panel. Top left: a golden bronze common carp with big scales. "
                "Top right: a yellow perch with dark vertical bars and orange fins. Bottom left: a zander, grey green "
                "with faint bars and a spiny dorsal fin. Bottom right: a shining golden largemouth bass, metallic gold."),
}

only = [a for a in sys.argv[1:] if a not in ("weeds_post", "angler_post")]
if sys.argv[1:] and not only:
    only = ["-"]
for name, (seed, prompt) in WIDE.items():
    if only and name not in only:
        continue
    img = q.generate(prompt, 1024, 576, seed)
    img.save(OUT + name + ".png")
    print(name, "ok", flush=True)
for name, (cells, seed, prompt) in GRIDS.items():
    if only and name not in only:
        continue
    img = q.generate(prompt, 1024, 1024, seed)
    img.save(OUT + name + "_sheet.png")
    for n, cell in zip(cells, q.split_grid(img)):
        cell.save(OUT + n + ".png")
    print(name, "ok", flush=True)


# ---- post: the plants as billboard textures (128x256, keyed, root on the bottom row, flipped) ----
def weeds_to_565():
    import numpy as np
    from PIL import Image, ImageDraw
    app = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    for n in ("g_eel", "g_milfoil", "g_cabbage", "g_stems"):
        c = Image.open(OUT + n + ".png").convert("RGB")
        c = c.crop((10, 10, c.width - 10, c.height - 10))
        a = np.asarray(c).astype(int)
        bg = a[5, 5]
        far = np.abs(a - bg).sum(axis=2) > 90                      # the plant
        ys, xs = np.where(far)
        y0, y1, x0, x1 = ys.min(), ys.max(), xs.min(), xs.max()
        c = c.crop((x0, y0, x1 + 1, y1 + 1))
        im = c.copy()
        d = ImageDraw.Draw(im)
        aa = np.asarray(c).astype(int)
        for (px, py) in [(x, 0) for x in range(0, c.width, 2)] + [(0, y) for y in range(0, c.height, 2)] + \
                        [(c.width - 1, y) for y in range(0, c.height, 2)]:
            if np.abs(aa[py, px] - bg).sum() < 90 and im.getpixel((px, py)) != (255, 0, 255):
                ImageDraw.floodfill(im, (px, py), (255, 0, 255), thresh=70)
        # pockets of background enclosed by leaves: key every pixel close to the navy, and the dark
        # blue-ish fringe around them (the plants are green, yellow and red: no blue in them)
        a2 = np.asarray(im).astype(int)
        near = (np.abs(a2 - bg).sum(axis=2) < 110) | ((a2[..., 2] > a2[..., 1] + 25) & (a2[..., 2] > a2[..., 0] + 25))
        a2[near] = (255, 0, 255)
        im = Image.fromarray(a2.astype(np.uint8))
        W, H = 128, 256
        s = min((W - 2) / im.width, (H - 1) / im.height)
        im = im.resize((max(1, int(im.width * s)), max(1, int(im.height * s))), Image.NEAREST)
        out = Image.new("RGB", (W, H), (255, 0, 255))
        out.paste(im, ((W - im.width) // 2, H - im.height))     # root on the bottom row
        out.save(OUT + n + "_tex.png")
        out = out.transpose(Image.FLIP_TOP_BOTTOM)                 # VX_BILLBOARD: row 0 is the bottom
        q.to565(out, os.path.join(app, "img", n + ".565"))
        print(n, "->", n + ".565")


if "weeds_post" in sys.argv[1:]:
    weeds_to_565()


# ---- post: the angler poses as billboard textures (256x256, keyed, feet on the bottom row, flipped)
# and where the rod tip is in each (angler.h), for the line.
def angler_to_565():
    import numpy as np
    from collections import deque
    from PIL import Image
    app = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    tips = []
    for n in ("an_idle", "an_wind", "an_cast", "an_reel"):
        c = Image.open(OUT + n + ".png").convert("RGB")
        c = c.crop((10, 10, c.width - 10, c.height - 10))
        a = np.asarray(c).astype(int)
        bg = a[4, 4]
        dist = np.abs(a - bg).sum(axis=2)
        near = dist < 34                                   # strict: the vest is blue too
        hh, ww = near.shape
        out = np.zeros_like(near)
        q = deque([(y, x) for y in range(hh) for x in (0, ww - 1) if near[y, x]] + [(y, x) for x in range(ww) for y in (0, hh - 1) if near[y, x]])
        for y, x in q:
            out[y, x] = True
        while q:
            y, x = q.popleft()
            for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                ny, nx = y + dy, x + dx
                if 0 <= ny < hh and 0 <= nx < ww and near[ny, nx] and not out[ny, nx]:
                    out[ny, nx] = True
                    q.append((ny, nx))
        # a one-pixel fringe of navy anti-aliasing next to the background
        edge = out.copy()
        edge[1:, :] |= out[:-1, :]; edge[:-1, :] |= out[1:, :]; edge[:, 1:] |= out[:, :-1]; edge[:, :-1] |= out[:, 1:]
        out |= edge & (dist < 80)
        # below the knees (the gap between the legs, the shadow ellipse): every navy-ish pixel goes
        low = np.arange(hh)[:, None] > hh * 0.62
        out |= low & (dist < 90) & (a[..., 2] > a[..., 0] + 20)
        a[out] = (255, 0, 255)
        ys, xs = np.where(~out)
        y0, y1, x0, x1 = ys.min(), ys.max(), xs.min(), xs.max()
        a = a[y0:y1 + 1, x0:x1 + 1]
        im = Image.fromarray(a.astype(np.uint8))
        S = 256
        s = min((S - 2) / im.width, (S - 1) / im.height)
        im = im.resize((max(1, int(im.width * s)), max(1, int(im.height * s))), Image.NEAREST)
        canvas = Image.new("RGB", (S, S), (255, 0, 255))
        ox, oy = (S - im.width) // 2, S - im.height
        canvas.paste(im, (ox, oy))
        # rod tip: the solid pixel furthest up and right (the rod runs to the top right)
        b = np.asarray(canvas).astype(int)
        solid = ~((b[..., 0] == 255) & (b[..., 1] == 0) & (b[..., 2] == 255))
        ys2, xs2 = np.where(solid)
        k = np.argmax(xs2 - ys2)
        legs = solid[int(S * 0.6):, :]                          # where the feet are: the figure's centre line
        lx = np.where(legs)[1]
        body = float(lx.mean()) / S if len(lx) else 0.5
        tips.append((xs2[k] / S, 1.0 - ys2[k] / S, body))     # u (0 left), v (0 bottom), body centre u
        canvas.save(OUT + n + "_tex.png")
        q2 = canvas.transpose(Image.FLIP_TOP_BOTTOM)            # VX_BILLBOARD: row 0 is the bottom
        qq.to565(q2, os.path.join(app, "img", n + ".565"))
        print(n, "tip", tips[-1])
    with open(os.path.join(app, "angler.h"), "w", newline="\n") as f:
        f.write("// angler.h - generated by art/gen9.py (angler_post): the rod tip in each pose's billboard,\n")
        f.write("// as fractions of its width (0 = left) and height (0 = bottom). Poses: idle, wind, cast, reel.\n")
        f.write("static const float k_rod_tip[4][2] = { %s };\n" % ", ".join("{ %.3ff, %.3ff }" % t[:2] for t in tips))
        f.write("static const float k_body_u[4] = { %s };   // the figure's centre line (fraction of width)\n" % ", ".join("%.3ff" % t[2] for t in tips))


if "angler_post" in sys.argv[1:]:
    qq = q
    angler_to_565()
