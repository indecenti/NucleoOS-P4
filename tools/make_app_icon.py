#!/usr/bin/env python3
# make_app_icon.py — render an SVG to the raw 80x80 ARGB8888 launcher icon a WASM app ships on SD.
# Output: apps/<id>/icon.argb (25600 bytes, LVGL byte order B,G,R,A) — the launcher loads it from
# /sdcard/apps/<id>/icon.argb (see wasm_tile_icon in components/nv_apps/apps_app.cpp); apps without
# one keep the generic puzzle tile. Same renderer as tools/gen_icons.py (fitz / PyMuPDF).
#
# Usage:
#   python tools/make_app_icon.py <svg-name> <app-id> [--mdi] [--tint R,G,B]
#   python tools/make_app_icon.py alphabetical-variant abc123 --mdi --tint 255,193,7
#   python tools/make_app_icon.py music pianino
import argparse
import os
import fitz

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SVG_DIR = os.path.join(ROOT, 'system', 'icons', 'flat-color', 'svg')
MDI_DIR = os.path.join(ROOT, 'system', 'icons', 'mdi', 'svg')
SIZE = 80


def render_bgra(svg_path, size, tint=None):
    doc = fitz.open(svg_path)
    page = doc[0]
    zoom = size / max(page.rect.width, page.rect.height)
    pix = page.get_pixmap(matrix=fitz.Matrix(zoom, zoom), alpha=True)
    src = pix.samples
    sw, sh, n = pix.width, pix.height, pix.n
    ox, oy = (size - sw) // 2, (size - sh) // 2
    out = bytearray(size * size * 4)
    for y in range(sh):
        for x in range(sw):
            si = (y * sw + x) * n
            r, g, b, a = src[si], src[si + 1], src[si + 2], (src[si + 3] if n == 4 else 255)
            if tint and a > 0:
                r, g, b = tint
            di = ((oy + y) * size + (ox + x)) * 4
            out[di + 0] = b
            out[di + 1] = g
            out[di + 2] = r
            out[di + 3] = a
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('svg', help='SVG basename (no .svg)')
    ap.add_argument('app', help='app id (apps/<id>/)')
    ap.add_argument('--mdi', action='store_true', help='use the MDI mono set (needs --tint)')
    ap.add_argument('--tint', help='R,G,B tint for mono glyphs')
    args = ap.parse_args()

    tint = tuple(int(x) for x in args.tint.split(',')) if args.tint else None
    base = MDI_DIR if args.mdi else SVG_DIR
    svg = os.path.join(base, args.svg + '.svg')
    data = render_bgra(svg, SIZE, tint)
    out = os.path.join(ROOT, 'apps', args.app, 'icon.argb')
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, 'wb') as f:
        f.write(data)
    print(f'{svg} -> {out} ({len(data)} bytes)')


if __name__ == '__main__':
    main()
