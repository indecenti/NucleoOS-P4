"""make_icon.py — launcher/store icon (icon.z) of the Libri app.

    python ports/books/make_icon.py <out/icon.z> [--png]

80x80 rounded tile: an open book with a ribbon, in the LVGL ARGB8888 byte order the firmware
expects (B,G,R,A), raw-deflate compressed (the icon.z format of ports/make_icon.py).
"""
import sys
import zlib

from PIL import Image, ImageDraw

SIZE, S = 80, 8
img = Image.new("RGBA", (SIZE * S, SIZE * S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
# the tile: a warm vertical gradient
tile = Image.new("RGBA", (SIZE * S, SIZE * S), (0, 0, 0, 0))
td = ImageDraw.Draw(tile)
for y in range(SIZE * S):
    t = y / (SIZE * S)
    c = (int(214 - 40 * t), int(96 - 34 * t), int(58 - 18 * t), 255)
    td.line((0, y, SIZE * S, y), fill=c)
mask = Image.new("L", (SIZE * S, SIZE * S), 0)
ImageDraw.Draw(mask).rounded_rectangle((2 * S, 2 * S, (SIZE - 2) * S, (SIZE - 2) * S), radius=18 * S, fill=255)
img.paste(tile, (0, 0), mask)
d = ImageDraw.Draw(img)
# the open book: two pages curving down to the spine
paper, shade, ink = (250, 246, 236, 255), (226, 218, 200, 255), (150, 140, 124, 255)
L = [(14, 26), (24, 22), (34, 22), (40, 26), (40, 60), (34, 56), (24, 56), (14, 60)]
R = [(66, 26), (56, 22), (46, 22), (40, 26), (40, 60), (46, 56), (56, 56), (66, 60)]
d.polygon([(x * S, y * S) for x, y in L], fill=paper)
d.polygon([(x * S, y * S) for x, y in R], fill=paper)
d.polygon([(38 * S, 26 * S), (42 * S, 26 * S), (42 * S, 60 * S), (38 * S, 60 * S)], fill=shade)
for k in range(5):   # lines of text
    y = (30 + k * 5) * S
    d.line((19 * S, y, 35 * S, y - 1 * S), fill=ink, width=int(1.4 * S))
    d.line((45 * S, y - 1 * S, 61 * S, y), fill=ink, width=int(1.4 * S))
# the ribbon
d.polygon([(52 * S, 22 * S), (58 * S, 22 * S), (58 * S, 66 * S), (55 * S, 62 * S), (52 * S, 66 * S)], fill=(242, 193, 78, 255))
img = img.resize((SIZE, SIZE), Image.LANCZOS)
co = zlib.compressobj(9, zlib.DEFLATED, -15)
open(sys.argv[1], "wb").write(co.compress(img.tobytes("raw", "BGRA")) + co.flush())
if "--png" in sys.argv:
    img.save(sys.argv[1][:-2] + ".png")
