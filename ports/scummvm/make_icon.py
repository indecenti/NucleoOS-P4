"""make_icon.py — launcher/store icon (icon.z) for ScummVM and its game packages.

    python ports/scummvm/make_icon.py <out/icon.z> [--badge TEXT] [--png]

The ScummVM app icon (art/scummvm-icon-1024.png, from the ScummVM source tree:
dists/ios7/Images.xcassets/AppIcon.appiconset/icon4-1024.png, GPLv3 like ScummVM) scaled to the 80x80
rounded tile the launcher draws, in the LVGL ARGB8888 byte order the firmware expects (B,G,R,A),
raw-deflate compressed (same icon.z format as ports/make_icon.py). --badge adds a short label chip
at the bottom (game packages: "BASS", "FOTAQ"...).
"""
import os
import sys
import zlib

from PIL import Image, ImageDraw, ImageFont

SIZE = 80
HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    out = sys.argv[1]
    badge = sys.argv[sys.argv.index("--badge") + 1] if "--badge" in sys.argv else None
    s = 4
    src = Image.open(os.path.join(HERE, "art", "scummvm-icon-1024.png")).convert("RGBA")
    tile = src.resize(((SIZE - 4) * s, (SIZE - 4) * s), Image.LANCZOS)
    mask = Image.new("L", tile.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, tile.size[0] - 1, tile.size[1] - 1), radius=18 * s, fill=255)
    img = Image.new("RGBA", (SIZE * s, SIZE * s), (0, 0, 0, 0))
    img.paste(tile, (2 * s, 2 * s), mask)
    if badge:
        d = ImageDraw.Draw(img)
        f = ImageFont.truetype("C:/Windows/Fonts/segoeuib.ttf", 13 * s)
        box = d.textbbox((0, 0), badge, font=f)
        w = box[2] - box[0] + 10 * s
        x0 = (SIZE * s - w) // 2
        y0, y1 = 56 * s, 74 * s
        d.rounded_rectangle((x0, y0, x0 + w, y1), radius=6 * s, fill=(20, 22, 26, 240),
                            outline=(255, 255, 255, 120), width=int(1.2 * s))
        d.text((x0 + 5 * s - box[0], (y0 + y1) / 2 - (box[3] - box[1]) / 2 - box[1]), badge, font=f,
               fill=(255, 255, 255, 255))
    img = img.resize((SIZE, SIZE), Image.LANCZOS)
    raw = img.tobytes("raw", "BGRA")
    co = zlib.compressobj(9, zlib.DEFLATED, -15)
    with open(out, "wb") as fh:
        fh.write(co.compress(raw) + co.flush())
    if "--png" in sys.argv:
        img.save(out[:-2] + ".png")


if __name__ == "__main__":
    main()
