#!/usr/bin/env python3
"""logo.py - Vertice Bass: the title logo (img/logo.565, magenta-keyed) for the title screen.

"VERTICE" in white over a huge "BASS" in a sunset gradient (yellow to orange), both with a thick dark
outline, a deep extruded 3D shadow and a thin light rim along the top of the letters — the 1990s
arcade marquee look. Montserrat Bold (SIL OFL) from LVGL's test fonts. Rendered 4x and reduced, edges
blended into the outline so the magenta key never fringes.

    python apps/bass/art/logo.py
"""
import os
import struct

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.dirname(HERE)
FONT = os.path.join(HERE, "..", "..", "..", "managed_components", "lvgl__lvgl", "tests", "src", "test_files",
                    "fonts", "Montserrat-Bold.ttf")
S = 4
W, H = 330, 150                                   # final size on the 512x300 canvas


def word_layer(text, size, top, track):
    """Coverage mask of one word, centred, at y = top (supersampled)."""
    f = ImageFont.truetype(FONT, size * S)
    widths = [f.getlength(c) for c in text]
    total = sum(widths) + track * S * (len(text) - 1)
    m = Image.new("L", (W * S, H * S), 0)
    d = ImageDraw.Draw(m)
    x = (W * S - total) / 2
    for c, w in zip(text, widths):
        d.text((x, top * S), c, font=f, fill=255)
        x += w + track * S
    return m


def main():
    v = word_layer("VERTICE", 30, 2, 3)
    b = word_layer("BASS", 84, 30, 2)
    canvas = np.zeros((H * S, W * S, 3), np.float32)
    alpha = np.zeros((H * S, W * S), np.float32)

    def comp(mask, rgb):
        nonlocal canvas, alpha
        a = np.asarray(mask, np.float32)[..., None] / 255.0
        canvas = canvas * (1 - a) + np.asarray(rgb, np.float32) * a
        alpha = np.maximum(alpha, a[..., 0])

    for word, gy0, gy1, top_c, bot_c, out_w, depth in (
            (b, 30, 120, (255, 236, 90), (255, 120, 20), 6, 9),
            (v, 2, 36, (255, 255, 255), (190, 220, 255), 4, 5)):
        outline = word.filter(ImageFilter.MaxFilter(2 * out_w * S + 1))
        # extrusion: the outline shifted down-right step by step, dark red to near black
        for k in range(depth * S, 0, -S):
            sh = Image.new("L", outline.size, 0)
            sh.paste(outline, (k // 2, k))
            t = k / (depth * S)
            comp(sh, (int(70 * (1 - t) + 20 * t), int(16 * (1 - t) + 6 * t), int(24 * (1 - t) + 10 * t)))
        comp(outline, (12, 14, 26))
        # the face: vertical gradient between the word's top and bottom rows
        ys = np.linspace(0, 1, H * S)[:, None]
        g = np.clip((ys * H - gy0) / max(1, gy1 - gy0), 0, 1)
        face = np.zeros((H * S, W * S, 3), np.float32)
        for ch in range(3):
            face[..., ch] = top_c[ch] * (1 - g[..., 0])[:, None] + bot_c[ch] * g[..., 0][:, None]
        a = np.asarray(word, np.float32)[..., None] / 255.0
        canvas = canvas * (1 - a) + face * a
        # a light rim on the upper edges of the letters (the mask minus itself moved down)
        down = Image.new("L", word.size, 0)
        down.paste(word, (0, 2 * S))
        rim = np.clip(np.asarray(word, np.float32) - np.asarray(down, np.float32), 0, 255)[..., None] / 255.0 * 0.7
        canvas = canvas * (1 - rim) + 255 * rim
    # reduce
    def down4(x):
        h, w = x.shape[0] // S, x.shape[1] // S
        return x[:h * S, :w * S].reshape(h, S, w, S, *x.shape[2:]).mean(axis=(1, 3))
    c = down4(canvas)
    a = down4(alpha)
    out = np.zeros((H, W, 3), np.uint8)
    out[:] = (255, 0, 255)
    on = a >= 0.5
    out[on] = np.clip(c[on] / np.maximum(a[on], 1e-3)[:, None], 0, 255).astype(np.uint8)
    im = Image.fromarray(out)
    im.save(os.path.join(os.environ.get("TEMP", "/tmp"), "bass_logo.png"))
    arr = np.asarray(im).astype(np.uint16)
    v565 = ((arr[..., 0] & 0xF8) << 8) | ((arr[..., 1] & 0xFC) << 3) | (arr[..., 2] >> 3)
    with open(os.path.join(APP, "img", "logo.565"), "wb") as f:
        f.write(struct.pack("<HH", W, H))
        f.write(v565.astype("<u2").tobytes())
    print("logo.565", W, "x", H)


if __name__ == "__main__":
    main()
