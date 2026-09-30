"""make_icon.py — launcher/store icon (icon.z) of the Puzzles app.

    python ports/puzzles/make_icon.py <out/icon.z> [--png]

80x80 rounded tile: a small 4x4 puzzle grid (a few filled cells, one digit, one dot) in the LVGL
ARGB8888 byte order the firmware expects (B,G,R,A), raw-deflate compressed — the same icon.z format
as ports/make_icon.py, without the terminal badge (this is a full-screen app).
"""
import sys
import zlib

from PIL import Image, ImageDraw, ImageFont

SIZE, S = 80, 4
img = Image.new("RGBA", (SIZE * S, SIZE * S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
d.rounded_rectangle((2 * S, 2 * S, (SIZE - 2) * S, (SIZE - 2) * S), radius=18 * S, fill=(38, 70, 140, 255))
x0, y0, cell = 14 * S, 14 * S, 13 * S
board = (231, 236, 244, 255)
d.rectangle((x0, y0, x0 + 4 * cell, y0 + 4 * cell), fill=board)
filled = {(0, 0), (2, 1), (1, 2), (3, 3), (3, 0)}
for (cx, cy) in filled:
    d.rectangle((x0 + cx * cell, y0 + cy * cell, x0 + (cx + 1) * cell, y0 + (cy + 1) * cell), fill=(30, 36, 48, 255))
for i in range(5):
    w = 2 * S if i in (0, 4) else S
    d.line((x0 + i * cell, y0, x0 + i * cell, y0 + 4 * cell), fill=(30, 36, 48, 255), width=w)
    d.line((x0, y0 + i * cell, x0 + 4 * cell, y0 + i * cell), fill=(30, 36, 48, 255), width=w)
f = ImageFont.load_default(11 * S)
d.text((x0 + 1.5 * cell, y0 + 0.5 * cell), "3", font=f, fill=(210, 60, 50, 255), anchor="mm")
cx, cy = x0 + 2.5 * cell, y0 + 3.5 * cell
d.ellipse((cx - 4 * S, cy - 4 * S, cx + 4 * S, cy + 4 * S), fill=(40, 150, 90, 255))
img = img.resize((SIZE, SIZE), Image.LANCZOS)
co = zlib.compressobj(9, zlib.DEFLATED, -15)
open(sys.argv[1], "wb").write(co.compress(img.tobytes("raw", "BGRA")) + co.flush())
if "--png" in sys.argv:
    img.save(sys.argv[1][:-2] + ".png")
