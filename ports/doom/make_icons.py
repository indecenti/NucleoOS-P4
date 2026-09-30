"""make_icons.py — launcher icons (icon.z) for the Doom engine and its store games.

    python ports/doom/make_icons.py <titles-dir> [--png]

Each game's icon is its own title screen (TITLEPIC as the engine draws it, captured by the PC
harness: ports/_src/doom/test/title-<id>.ppm, 320x240): art titles are cropped to a square, text
titles are fitted whole over a blurred copy of themselves. The engine's icon is drawn here.
Output: apps/<id>/icon.z, 80x80 rounded tile, LVGL ARGB8888 byte order (B,G,R,A), raw deflate.
"""
import os
import sys
import zlib

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
SIZE, S = 80, 4           # tile size, supersampling

# id -> ("crop", x, y, side) in the 320x240 title, or ("fit",) for text-only titles
LAYOUT = {
    "freedoom1": ("crop", 70, 10, 190),
    "freedoom2": ("crop", 70, 10, 190),
    "doomsw": ("crop", 70, 0, 220),
    "dtwid": ("crop", 50, 0, 230),
    "d2reload": ("crop", 40, 0, 240),
    "sigil": ("crop", 40, 0, 240),
    "scythe": ("fit",),
    "plutonia2": ("fit",),
    "thousandlines2": ("fit",),
    "zone300": ("crop", 60, 0, 200),
}
# games whose title screen is not their own (they keep the IWAD's): a lettered tile instead
LETTERED = {"mementomori": ("MM", (96, 12, 12), (236, 196, 120))}


def tile(pic):
    img = Image.new("RGBA", (SIZE * S, SIZE * S), (0, 0, 0, 0))
    mask = Image.new("L", img.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle((2 * S, 2 * S, (SIZE - 2) * S, (SIZE - 2) * S),
                                           radius=18 * S, fill=255)
    img.paste(pic.resize(img.size, Image.LANCZOS).convert("RGBA"), (0, 0), mask)
    return img.resize((SIZE, SIZE), Image.LANCZOS)


def from_title(ppm, layout):
    src = Image.open(ppm).convert("RGB")
    if layout[0] == "crop":
        _, x, y, n = layout
        return tile(src.crop((x, y, x + n, y + n)))
    # fit: the whole title over a blurred, darkened cover of itself
    bg = src.crop((40, 0, 280, 240)).resize((240, 240)).filter(ImageFilter.GaussianBlur(10))
    bg = Image.eval(bg, lambda v: v * 45 // 100)
    fg = src.resize((240, 180), Image.LANCZOS)
    bg.paste(fg, (0, 30))
    return tile(bg)


def font(px):
    for f in ("C:/Windows/Fonts/impact.ttf", "C:/Windows/Fonts/arialbd.ttf"):
        if os.path.isfile(f):
            return ImageFont.truetype(f, px)
    return ImageFont.load_default()


def lettered(text, bg, fg):
    img = Image.new("RGB", (240, 240), bg)
    d = ImageDraw.Draw(img)
    for i in range(240):   # a little depth: darker towards the bottom
        d.line((0, i, 239, i), fill=tuple(max(0, c - i // 5) for c in bg))
    f = font(120)
    w = d.textlength(text, font=f)
    d.text(((240 - w) / 2 + 4, 58), text, font=f, fill=(0, 0, 0))
    d.text(((240 - w) / 2, 54), text, font=f, fill=fg)
    return tile(img)


def engine_icon():
    img = Image.new("RGB", (240, 240), (0, 0, 0))
    d = ImageDraw.Draw(img)
    for i in range(240):   # hellish glow rising from the bottom
        t = i / 239
        d.line((0, i, 239, i), fill=(int(20 + 170 * t ** 2), int(6 + 40 * t ** 3), 4))
    f = font(150)
    w = d.textlength("D", font=f)
    d.text(((240 - w) / 2 + 6, 22), "D", font=f, fill=(0, 0, 0))
    d.text(((240 - w) / 2, 16), "D", font=f, fill=(232, 222, 205))
    f2 = font(34)
    w2 = d.textlength("ENGINE", font=f2)
    d.text(((240 - w2) / 2, 184), "ENGINE", font=f2, fill=(255, 150, 40))
    return tile(img)


def save(img, gid, png):
    d = os.path.join(ROOT, "apps", gid)
    os.makedirs(d, exist_ok=True)
    co = zlib.compressobj(9, zlib.DEFLATED, -15)
    open(os.path.join(d, "icon.z"), "wb").write(co.compress(img.tobytes("raw", "BGRA")) + co.flush())
    if png:
        img.save(os.path.join(png, gid + ".png"))


def main():
    titles = sys.argv[1]
    png = titles if "--png" in sys.argv else None
    save(engine_icon(), "doom", png)
    for gid, layout in LAYOUT.items():
        save(from_title(os.path.join(titles, f"title-{gid}.ppm"), layout), gid, png)
    for gid, (text, bg, fg) in LETTERED.items():
        save(lettered(text, bg, fg), gid, png)
    print(f"{1 + len(LAYOUT) + len(LETTERED)} icons written")


if __name__ == "__main__":
    main()
