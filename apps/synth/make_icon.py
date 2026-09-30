"""make_icon.py - Synth store/launcher icon (icon.z): 80x80 rounded tile (radius 18), purple to
magenta, a waveform above three piano keys. LVGL ARGB8888 byte order (B,G,R,A), raw deflate, same
format as ports/make_icon.py.

    python apps/synth/make_icon.py [--png]
"""
import math
import os
import sys
import zlib

from PIL import Image, ImageDraw

SIZE = 80
S = 4  # supersample
HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    big = SIZE * S
    # vertical gradient purple -> magenta, masked by a rounded tile
    grad = Image.new("RGBA", (big, big))
    gd = ImageDraw.Draw(grad)
    top, bot = (124, 58, 237), (219, 39, 119)
    for y in range(big):
        t = y / (big - 1)
        gd.line([(0, y), (big, y)], fill=tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3)) + (255,))
    mask = Image.new("L", (big, big), 0)
    ImageDraw.Draw(mask).rounded_rectangle((2 * S, 2 * S, (SIZE - 2) * S, (SIZE - 2) * S), radius=18 * S, fill=255)
    img = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    img.paste(grad, (0, 0), mask)
    d = ImageDraw.Draw(img)

    # waveform (a sine that grows into a saw-ish shape), white, thick
    pts = []
    x0, x1, yc, amp = 14, 66, 27, 10
    for i in range(200):
        t = i / 199
        x = x0 + (x1 - x0) * t
        ph = t * 2 * math.pi * 2.0
        y = yc - amp * (math.sin(ph) * (1 - t * 0.5) + 0.35 * t * math.sin(2 * ph))
        pts.append((x * S, y * S))
    d.line(pts, fill=(255, 255, 255, 255), width=int(3.2 * S), joint="curve")

    # three white keys with two black keys, on a soft shadow
    kx0, ky0, kw, kh = 16, 44, 16, 24
    d.rounded_rectangle(((kx0 - 1) * S, (ky0 + 1) * S, (kx0 + 3 * kw + 1) * S, (ky0 + kh + 2) * S),
                        radius=4 * S, fill=(60, 10, 60, 90))
    for i in range(3):
        x = kx0 + i * kw
        d.rounded_rectangle(((x + 0.7) * S, ky0 * S, (x + kw - 0.7) * S, (ky0 + kh) * S),
                            radius=3 * S, fill=(250, 248, 255, 255))
    for i in (1, 2):
        cx = kx0 + i * kw
        d.rounded_rectangle(((cx - 4.5) * S, ky0 * S, (cx + 4.5) * S, (ky0 + 14) * S),
                            radius=2 * S, fill=(40, 24, 64, 255))
    img = img.resize((SIZE, SIZE), Image.LANCZOS)
    raw = img.tobytes("raw", "BGRA")
    co = zlib.compressobj(9, zlib.DEFLATED, -15)
    open(os.path.join(HERE, "icon.z"), "wb").write(co.compress(raw) + co.flush())
    if "--png" in sys.argv:
        img.resize((SIZE * 4, SIZE * 4), Image.NEAREST).save(os.path.join(HERE, "icon_preview.png"))
    print("icon.z written")


if __name__ == "__main__":
    main()
