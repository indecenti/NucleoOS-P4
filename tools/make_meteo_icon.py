#!/usr/bin/env python3
"""Generate launcher icon for Meteo app."""
import os
import zlib
import fitz
from PIL import Image, ImageDraw

SIZE = 80
SCALE = 4
RADIUS = 18 * SCALE
BG_COLOR = (25, 118, 210, 255)  # Material Blue 700

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SVG_PATH = os.path.join(ROOT, "system", "icons", "mdi", "svg", "weather-partly-cloudy.svg")
OUT_DIR = os.path.join(ROOT, "apps", "meteo")

def main():
    # 1. Base tile at 4x resolution
    canvas = Image.new("RGBA", (SIZE * SCALE, SIZE * SCALE), (0, 0, 0, 0))
    draw = ImageDraw.Draw(canvas)
    draw.rounded_rectangle(
        (2 * SCALE, 2 * SCALE, (SIZE - 2) * SCALE, (SIZE - 2) * SCALE),
        radius=RADIUS,
        fill=BG_COLOR
    )

    # 2. Render SVG glyph at high resolution
    glyph_size = 52 * SCALE
    doc = fitz.open(SVG_PATH)
    page = doc[0]
    zoom = glyph_size / max(page.rect.width, page.rect.height)
    pix = page.get_pixmap(matrix=fitz.Matrix(zoom, zoom), alpha=True)
    
    glyph_img = Image.frombytes("RGBA", [pix.width, pix.height], pix.samples)
    
    # Tint glyph to pure white with slight sun warmth if wanted, let's keep clean white
    r, g, b, a = glyph_img.split()
    white_glyph = Image.merge("RGBA", (
        Image.new("L", glyph_img.size, 255),
        Image.new("L", glyph_img.size, 255),
        Image.new("L", glyph_img.size, 255),
        a
    ))

    # Center glyph on tile
    gx = (SIZE * SCALE - glyph_img.width) // 2
    gy = (SIZE * SCALE - glyph_img.height) // 2
    canvas.paste(white_glyph, (gx, gy), white_glyph)

    # 3. Downsample to 80x80
    final_img = canvas.resize((SIZE, SIZE), Image.LANCZOS)
    
    # 4. Save PNG preview
    png_path = os.path.join(OUT_DIR, "icon.png")
    final_img.save(png_path)

    # 5. Extract BGRA bytes
    bgra = final_img.tobytes("raw", "BGRA")
    
    argb_path = os.path.join(OUT_DIR, "icon.argb")
    with open(argb_path, "wb") as f:
        f.write(bgra)

    # 6. Compress with raw deflate (-15 window bits for tinfl)
    co = zlib.compressobj(9, zlib.DEFLATED, -15)
    z_data = co.compress(bgra) + co.flush()
    
    z_path = os.path.join(OUT_DIR, "icon.z")
    with open(z_path, "wb") as f:
        f.write(z_data)

    print(f"Generated: {argb_path} ({len(bgra)} bytes), {z_path} ({len(z_data)} bytes)")

if __name__ == "__main__":
    main()
