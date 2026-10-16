#!/usr/bin/env python3
"""make_dmd_icon.py — the DMD Flipper app's launcher icon, drawn in the style of the icons8 flat-color set the
other system icons use (flat fills, no outlines, the same material palette): a dot-matrix display reading
"DMD" in orange dots, and below it a steel ball between two red flippers.

    python tools/icons/make_dmd_icon.py        -> tools/icons/dmd.png (80x80 RGBA)
    python tools/gen_icons.py --from-c         -> packs it into components/nv_ui/generated/nv_icons.c

Needs Pillow. Drawn 8x larger, then downsampled (smooth edges).
"""
import os

from PIL import Image, ImageDraw

SIZE, SS = 80, 8
S = SIZE * SS
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dmd.png")

# icons8 flat-color palette
PANEL = (55, 71, 79)        # #37474F
PANEL_IN = (38, 50, 56)     # #263238
DOT_ON = (255, 152, 0)      # #FF9800
DOT_OFF = (78, 52, 46)      # #4E342E, an unlit dot
RED = (244, 67, 54)         # #F44336
RED_DARK = (211, 47, 47)    # #D32F2F
STEEL = (144, 164, 174)     # #90A4AE
STEEL_LIGHT = (236, 239, 241)

# "DMD" in a 3x5 font, one dot between the letters: 11 x 5 dots
GLYPHS = {
    "D": ["110", "101", "101", "101", "110"],
    "M": ["101", "111", "111", "101", "101"],
}


def px(v):
    return int(round(v * SS))


def main():
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    # the display: a rounded panel with an inner screen
    d.rounded_rectangle((px(4), px(6), px(76), px(42)), radius=px(6), fill=PANEL)
    d.rounded_rectangle((px(8), px(10), px(72), px(38)), radius=px(3), fill=PANEL_IN)
    cols, rows = 13, 5
    pitch = 64 / cols                     # screen 64 x 28 px
    x0, y0 = 8 + pitch / 2, 10 + (28 - rows * pitch) / 2 + pitch / 2
    lit = set()
    cx = 1
    for ch in "DMD":
        for r, line in enumerate(GLYPHS[ch]):
            for c, bit in enumerate(line):
                if bit == "1":
                    lit.add((cx + c, r))
        cx += 4
    rad = pitch * 0.36
    for r in range(rows):
        for c in range(cols):
            x, y = x0 + c * pitch, y0 + r * pitch
            d.ellipse((px(x - rad), px(y - rad), px(x + rad), px(y + rad)), fill=DOT_ON if (c, r) in lit else DOT_OFF)

    # the flippers: two rounded bars pointing down towards the middle
    def flipper(xa, ya, xb, yb, w):
        d.line((px(xa), px(ya), px(xb), px(yb)), fill=RED, width=px(w))
        for (x, y, rr) in ((xa, ya, w / 2 + 1.2), (xb, yb, w / 2 - 0.4)):
            d.ellipse((px(x - rr), px(y - rr), px(x + rr), px(y + rr)), fill=RED)
        d.ellipse((px(xa - 1.6), px(ya - 1.6), px(xa + 1.6), px(ya + 1.6)), fill=RED_DARK)   # the pivot

    flipper(12, 60, 32, 70, 7)
    flipper(68, 60, 48, 70, 7)

    # the ball, with a highlight
    bx, by, br = 40, 54, 8
    d.ellipse((px(bx - br), px(by - br), px(bx + br), px(by + br)), fill=STEEL)
    d.ellipse((px(bx - 4.5), px(by - 5), px(bx - 0.5), px(by - 1)), fill=STEEL_LIGHT)

    img.resize((SIZE, SIZE), Image.LANCZOS).save(OUT)
    print("wrote", OUT)


if __name__ == "__main__":
    main()
