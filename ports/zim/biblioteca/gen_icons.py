"""gen_icons.py — the interface icons of the Biblioteca app, drawn here (no third-party artwork):
8-bit coverage masks the app tints with the theme colours.

    python ports/zim/biblioteca/gen_icons.py OUT.h

Each icon is drawn on a 24-unit grid at 16x, then box-filtered down to its size in pixels.
"""
import math
import sys

from PIL import Image, ImageDraw

SS = 16   # supersampling


def canvas(px):
    return Image.new("L", (px * SS, px * SS), 0)


def P(px, x, y):   # grid units (0..24) -> supersampled pixels
    k = px * SS / 24.0
    return (x * k, y * k)


def W(px, w):
    return max(1, int(round(w * px * SS / 24.0)))


def line(d, px, pts, w=2.0):
    q = [P(px, x, y) for x, y in pts]
    d.line(q, fill=255, width=W(px, w), joint="curve")
    r = W(px, w) / 2.0
    for x, y in (q[0], q[-1]):   # round caps
        d.ellipse((x - r, y - r, x + r, y + r), fill=255)


def circle(d, px, cx, cy, r, w=None):
    (x0, y0), (x1, y1) = P(px, cx - r, cy - r), P(px, cx + r, cy + r)
    if w is None:
        d.ellipse((x0, y0, x1, y1), fill=255)
    else:
        d.ellipse((x0, y0, x1, y1), outline=255, width=W(px, w))


def poly(d, px, pts, fill=True, w=2.0):
    q = [P(px, x, y) for x, y in pts]
    if fill:
        d.polygon(q, fill=255)
    else:
        d.line(q + [q[0]], fill=255, width=W(px, w), joint="curve")


def star(d, px, filled):
    pts = []
    for i in range(10):
        a = -math.pi / 2 + i * math.pi / 5
        r = 9.5 if i % 2 == 0 else 4.2
        pts.append((12 + r * math.cos(a), 12.6 + r * math.sin(a)))
    poly(d, px, pts, fill=filled, w=1.8)


def rrect(d, px, x0, y0, x1, y1, r, w=None):
    (a, b), (c, e) = P(px, x0, y0), P(px, x1, y1)
    if w is None:
        d.rounded_rectangle((a, b, c, e), radius=r * px * SS / 24.0, fill=255)
    else:
        d.rounded_rectangle((a, b, c, e), radius=r * px * SS / 24.0, outline=255, width=W(px, w))


def draw(name, px):
    im = canvas(px)
    d = ImageDraw.Draw(im)
    if name == "back":
        line(d, px, [(19, 12), (5, 12)])
        line(d, px, [(11, 6), (5, 12), (11, 18)])
    elif name == "forward":
        line(d, px, [(5, 12), (19, 12)])
        line(d, px, [(13, 6), (19, 12), (13, 18)])
    elif name == "search":
        circle(d, px, 10, 10, 6.2, w=2.0)
        line(d, px, [(14.6, 14.6), (20, 20)], w=2.4)
    elif name == "toc":
        for y in (6, 12, 18):
            circle(d, px, 4.5, y, 1.4)
            line(d, px, [(9, y), (20, y)])
    elif name == "star":
        star(d, px, False)
    elif name == "star_on":
        star(d, px, True)
    elif name == "more":
        for y in (5, 12, 19):
            circle(d, px, 12, y, 1.9)
    elif name == "library":
        rrect(d, px, 3.5, 5, 7.5, 20, 0.8, w=1.8)
        rrect(d, px, 9, 3, 13, 20, 0.8, w=1.8)
        poly(d, px, [(15, 6.2), (18.6, 5.2), (22, 19.2), (18.4, 20.2)], fill=False, w=1.8)
        line(d, px, [(2, 21.5), (22, 21.5)], w=1.6)
    elif name == "random":
        rrect(d, px, 3.5, 3.5, 20.5, 20.5, 3.5, w=2.0)
        for x, y in ((8, 8), (16, 8), (12, 12), (8, 16), (16, 16)):
            circle(d, px, x, y, 1.6)
    elif name == "info":
        circle(d, px, 12, 12, 9.5, w=2.0)
        circle(d, px, 12, 7.6, 1.4)
        line(d, px, [(12, 11), (12, 17)], w=2.2)
    elif name == "close":
        line(d, px, [(6, 6), (18, 18)])
        line(d, px, [(18, 6), (6, 18)])
    elif name == "history":
        d.arc((*P(px, 3, 3), *P(px, 21, 21)), start=-200, end=150, fill=255, width=W(px, 2.0))
        poly(d, px, [(1.2, 9.6), (6.6, 9.6), (3.4, 14.2)])
        line(d, px, [(12, 7.5), (12, 12), (15.5, 14.5)])
    elif name == "textsize":
        line(d, px, [(2.5, 19), (7, 7.5), (11.5, 19)], w=1.8)
        line(d, px, [(4.3, 15), (9.7, 15)], w=1.6)
        line(d, px, [(11.5, 19), (17, 4), (22.5, 19)], w=2.2)
        line(d, px, [(13.6, 14), (20.4, 14)], w=2.0)
    elif name == "theme":
        circle(d, px, 12, 12, 9, w=2.0)
        d.pieslice((*P(px, 3, 3), *P(px, 21, 21)), start=90, end=270, fill=255)
    elif name == "check":
        line(d, px, [(5, 12.5), (10, 17.5), (19.5, 7)], w=2.4)
    elif name == "backspace":
        poly(d, px, [(8, 5), (21, 5), (21, 19), (8, 19), (2.5, 12)], fill=False, w=1.8)
        line(d, px, [(11.5, 9), (17.5, 15)], w=1.8)
        line(d, px, [(17.5, 9), (11.5, 15)], w=1.8)
    elif name == "enter":
        line(d, px, [(19, 5), (19, 14), (5, 14)])
        line(d, px, [(9.5, 9.5), (5, 14), (9.5, 18.5)])
    elif name == "shift":
        poly(d, px, [(12, 3.5), (21, 12.5), (16, 12.5), (16, 20), (8, 20), (8, 12.5), (3, 12.5)], fill=False, w=1.8)
    elif name == "keyboard":
        rrect(d, px, 2, 6, 22, 19, 2, w=1.8)
        for y in (9.5, 12.5):
            for x in (5.5, 9, 12.5, 16, 19):
                circle(d, px, x, y, 0.9)
        line(d, px, [(7.5, 16), (16.5, 16)], w=1.6)
    elif name == "book":   # the cover of a collection without its own picture
        rrect(d, px, 5, 2.5, 20, 21.5, 1.5, w=1.6)
        line(d, px, [(8, 2.8), (8, 21.2)], w=1.4)
        line(d, px, [(10.5, 7.5), (17, 7.5)], w=1.3)
        line(d, px, [(10.5, 10.5), (15.5, 10.5)], w=1.3)
    elif name == "download":   # an arrow into a tray
        line(d, px, [(12, 3), (12, 15)], w=2.0)
        line(d, px, [(7, 10.5), (12, 15.5), (17, 10.5)], w=2.0)
        line(d, px, [(4, 15), (4, 20), (20, 20), (20, 15)], w=2.0)
    elif name == "pause":
        rrect(d, px, 6.5, 5, 10, 19, 1)
        rrect(d, px, 14, 5, 17.5, 19, 1)
    elif name == "play":
        poly(d, px, [(8, 4.5), (19.5, 12), (8, 19.5)])
    else:
        raise SystemExit("unknown icon " + name)
    return im.resize((px, px), Image.BOX)


ICONS = [("back", 24), ("forward", 24), ("search", 24), ("toc", 24), ("star", 24), ("star_on", 24),
         ("more", 24), ("library", 24), ("random", 24), ("info", 24), ("close", 24), ("history", 24),
         ("textsize", 24), ("theme", 24), ("check", 24), ("backspace", 24), ("enter", 24), ("shift", 24),
         ("keyboard", 24), ("book", 48), ("download", 24), ("pause", 24), ("play", 24)]


def main():
    out = sys.argv[1]
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("// Generated by ports/zim/biblioteca/gen_icons.py. Do not edit.\n#pragma once\n#include <stdint.h>\n\n")
        for name, px in ICONS:
            data = draw(name, px).tobytes()
            f.write("static const uint8_t ICON_%s[%d * %d] = {\n" % (name.upper(), px, px))
            for i in range(0, len(data), 24):
                f.write("    " + ",".join(str(b) for b in data[i:i + 24]) + ",\n")
            f.write("};\n#define ICON_%s_PX %d\n\n" % (name.upper(), px))
    if len(sys.argv) > 2:   # a preview sheet for checking the drawings
        sheet = Image.new("L", (len(ICONS) * 56, 56), 255)
        for i, (name, px) in enumerate(ICONS):
            im = draw(name, px)
            sheet.paste(Image.eval(im, lambda v: 255 - v), (i * 56 + (56 - px) // 2, (56 - px) // 2))
        sheet.resize((sheet.width * 2, sheet.height * 2), Image.NEAREST).save(sys.argv[2])
    print("%s: %d icons" % (out, len(ICONS)))


if __name__ == "__main__":
    main()
