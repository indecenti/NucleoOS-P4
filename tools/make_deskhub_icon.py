#!/usr/bin/env python3
"""Generate launcher icon for Pomodoro Desk Hub using flat-color alarm_clock.svg."""
import os
import zlib
import fitz
from PIL import Image

SIZE = 80
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SVG_PATH = os.path.join(ROOT, "system", "icons", "flat-color", "svg", "alarm_clock.svg")
OUT_DIR = os.path.join(ROOT, "apps", "deskhub")

def main():
    doc = fitz.open(SVG_PATH)
    page = doc[0]
    zoom = SIZE / max(page.rect.width, page.rect.height)
    pix = page.get_pixmap(matrix=fitz.Matrix(zoom, zoom), alpha=True)
    
    img = Image.frombytes("RGBA", [pix.width, pix.height], pix.samples)
    
    # Square 80x80 canvas with centering
    canvas = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    ox = (SIZE - img.width) // 2
    oy = (SIZE - img.height) // 2
    canvas.paste(img, (ox, oy), img)

    # Save PNG preview
    canvas.save(os.path.join(OUT_DIR, "icon.png"))

    # Convert to LVGL BGRA format
    bgra = canvas.tobytes("raw", "BGRA")
    
    argb_path = os.path.join(OUT_DIR, "icon.argb")
    with open(argb_path, "wb") as f:
        f.write(bgra)

    # Compress with raw-deflate for icon.z (tinfl)
    co = zlib.compressobj(9, zlib.DEFLATED, -15)
    z_data = co.compress(bgra) + co.flush()
    
    z_path = os.path.join(OUT_DIR, "icon.z")
    with open(z_path, "wb") as f:
        f.write(z_data)

    print(f"Generated from {SVG_PATH}:")
    print(f"  {argb_path} ({len(bgra)} bytes)")
    print(f"  {z_path} ({len(z_data)} bytes)")

if __name__ == "__main__":
    main()
