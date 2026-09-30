#!/usr/bin/env python3
"""Launcher/store icon for a WASM app from a Material Design Icons glyph (system/icons/mdi/svg).

    python tools/make_mdi_icon.py apps/casa home-assistant "#18bcf2"
    python tools/make_mdi_icon.py apps/zigbee zigbee "#ffc107" --fg "#1a1a1a"

Writes <app>/icon.png (preview) and <app>/icon.z (80x80 BGRA, raw deflate — what the store and the
launcher load). Same rendering as tools/make_meteo_icon.py: rounded tile, glyph centred, 4x
supersampled. Needs Pillow + PyMuPDF (fitz).
"""
import argparse
import os
import zlib

import fitz
from PIL import Image, ImageDraw

SIZE, SCALE = 80, 4
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def hex_rgba(s):
    s = s.lstrip("#")
    return tuple(int(s[i:i + 2], 16) for i in (0, 2, 4)) + (255,)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("app_dir")
    ap.add_argument("glyph", help="MDI icon name, e.g. home-assistant")
    ap.add_argument("bg", help="tile colour, #rrggbb")
    ap.add_argument("--fg", default="#ffffff", help="glyph colour")
    ap.add_argument("--glyph-size", type=int, default=52)
    a = ap.parse_args()

    bg, fg = hex_rgba(a.bg), hex_rgba(a.fg)
    canvas = Image.new("RGBA", (SIZE * SCALE, SIZE * SCALE), (0, 0, 0, 0))
    ImageDraw.Draw(canvas).rounded_rectangle(
        (2 * SCALE, 2 * SCALE, (SIZE - 2) * SCALE, (SIZE - 2) * SCALE), radius=18 * SCALE, fill=bg)

    doc = fitz.open(os.path.join(ROOT, "system", "icons", "mdi", "svg", a.glyph + ".svg"))
    page = doc[0]
    zoom = a.glyph_size * SCALE / max(page.rect.width, page.rect.height)
    pix = page.get_pixmap(matrix=fitz.Matrix(zoom, zoom), alpha=True)
    glyph = Image.frombytes("RGBA", [pix.width, pix.height], pix.samples)
    alpha = glyph.split()[3]
    tinted = Image.merge("RGBA", [Image.new("L", glyph.size, c) for c in fg[:3]] + [alpha])
    canvas.paste(tinted, ((SIZE * SCALE - glyph.width) // 2, (SIZE * SCALE - glyph.height) // 2), tinted)

    icon = canvas.resize((SIZE, SIZE), Image.LANCZOS)
    icon.save(os.path.join(a.app_dir, "icon.png"))
    co = zlib.compressobj(9, zlib.DEFLATED, -15)
    data = co.compress(icon.tobytes("raw", "BGRA")) + co.flush()
    with open(os.path.join(a.app_dir, "icon.z"), "wb") as f:
        f.write(data)
    print("%s: icon.png + icon.z (%d bytes)" % (a.app_dir, len(data)))


if __name__ == "__main__":
    main()
