"""make_icon.py — CHIP-8 app store/launcher icon (icon.z).

    python ports/chip8/make_icon.py <out/icon.z> [--png]

80x80 rounded tile in Octo's default palette with "C8" drawn in the CHIP-8 hex font, one square per
font pixel. Same icon.z format as ports/make_icon.py (LVGL ARGB8888 byte order B,G,R,A, raw deflate),
without its terminal badge: this app has its own window.
"""
import sys
import zlib

from PIL import Image, ImageDraw

SIZE = 80
S = 4   # supersample
BG, FG, SHADOW = (153, 102, 0), (255, 204, 0), (102, 34, 0)
GLYPHS = [[0xF0, 0x80, 0x80, 0x80, 0xF0],   # C
          [0xF0, 0x90, 0xF0, 0x90, 0xF0]]   # 8


def main():
    out = sys.argv[1]
    img = Image.new("RGBA", (SIZE * S, SIZE * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((2 * S, 2 * S, (SIZE - 2) * S, (SIZE - 2) * S), radius=18 * S, fill=BG + (255,))
    px, gap = 7, 1                       # font pixel size, gap between the two glyphs (in font px)
    w, h = (4 * 2 + gap) * px, 5 * px
    x0, y0 = (SIZE - w) / 2, (SIZE - h) / 2
    for dx, dy, col in ((1.5, 1.5, SHADOW), (0, 0, FG)):
        for gi, g in enumerate(GLYPHS):
            for row, bits in enumerate(g):
                for b in range(4):
                    if bits >> (7 - b) & 1:
                        x = x0 + (gi * (4 + gap) + b) * px + dx
                        y = y0 + row * px + dy
                        d.rectangle((x * S, y * S, (x + px) * S - 1, (y + px) * S - 1), fill=col + (255,))
    img = img.resize((SIZE, SIZE), Image.LANCZOS)
    raw = img.tobytes("raw", "BGRA")
    co = zlib.compressobj(9, zlib.DEFLATED, -15)
    open(out, "wb").write(co.compress(raw) + co.flush())
    if "--png" in sys.argv:
        img.save(out[:-2] + ".png")


if __name__ == "__main__":
    main()
