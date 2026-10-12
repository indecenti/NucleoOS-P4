"""make_icon.py — launcher/store icon (icon.z) of the Biblioteca app.

    python ports/zim/biblioteca/make_icon.py <out/icon.z> [--png]

80x80 rounded tile: three books on a shelf, the middle one open-faced, in the LVGL ARGB8888 byte
order the firmware expects (B,G,R,A), raw-deflate compressed (the icon.z format of ports/make_icon.py).
"""
import sys
import zlib

from PIL import Image, ImageDraw

SIZE, S = 80, 8
img = Image.new("RGBA", (SIZE * S, SIZE * S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
d.rounded_rectangle((2 * S, 2 * S, (SIZE - 2) * S, (SIZE - 2) * S), radius=18 * S, fill=(26, 92, 200, 255))


def book(x0, y0, x1, y1, color, band):
    d.rounded_rectangle((x0 * S, y0 * S, x1 * S, y1 * S), radius=2 * S, fill=color)
    d.rectangle((x0 * S, (y0 + 6) * S, x1 * S, (y0 + 9) * S), fill=band)
    d.rectangle((x0 * S, (y1 - 9) * S, x1 * S, (y1 - 6) * S), fill=band)


book(17, 22, 29, 60, (245, 247, 250, 255), (242, 193, 78, 255))
book(31, 16, 45, 60, (242, 193, 78, 255), (26, 92, 200, 255))
# a leaning book
lean = Image.new("RGBA", (SIZE * S, SIZE * S), (0, 0, 0, 0))
ld = ImageDraw.Draw(lean)
ld.rounded_rectangle((48 * S, 24 * S, 60 * S, 62 * S), radius=2 * S, fill=(214, 228, 250, 255))
ld.rectangle((48 * S, 30 * S, 60 * S, 33 * S), fill=(26, 92, 200, 255))
ld.rectangle((48 * S, 53 * S, 60 * S, 56 * S), fill=(26, 92, 200, 255))
lean = lean.rotate(-14, resample=Image.BICUBIC, center=(48 * S, 62 * S))
img.alpha_composite(lean)
d = ImageDraw.Draw(img)
d.rounded_rectangle((13 * S, 60 * S, 67 * S, 64 * S), radius=2 * S, fill=(245, 247, 250, 255))
img = img.resize((SIZE, SIZE), Image.LANCZOS)
co = zlib.compressobj(9, zlib.DEFLATED, -15)
open(sys.argv[1], "wb").write(co.compress(img.tobytes("raw", "BGRA")) + co.flush())
if "--png" in sys.argv:
    img.save(sys.argv[1][:-2] + ".png")
