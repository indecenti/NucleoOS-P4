"""make_icon.py — store/launcher icon (icon.z) for a terminal program.

    python ports/make_icon.py <out/icon.z> <label> <bg #rrggbb> <fg #rrggbb>

80x80 rounded tile with a short label and a small ">_" prompt mark, in the LVGL ARGB8888 byte
order the firmware expects (B,G,R,A), raw-deflate compressed (tinfl) — the same icon.z format as
tools/w4harness/store_meta.py.
"""
import sys
import zlib

from PIL import Image, ImageDraw, ImageFont

SIZE = 80
FONT_DIR = "C:/Windows/Fonts/"


def rgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def fit_font(draw, text, path, max_w, start):
    size = start
    while size > 10:
        f = ImageFont.truetype(path, size)
        box = draw.textbbox((0, 0), text, font=f)
        if box[2] - box[0] <= max_w:
            return f
        size -= 2
    return ImageFont.truetype(path, size)


def main():
    out, label, bg, fg = sys.argv[1], sys.argv[2], rgb(sys.argv[3]), rgb(sys.argv[4])
    s = 4   # draw at 4x, downsample: smooth edges
    img = Image.new("RGBA", (SIZE * s, SIZE * s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((2 * s, 2 * s, (SIZE - 2) * s, (SIZE - 2) * s), radius=18 * s, fill=bg + (255,))
    mono = FONT_DIR + "consolab.ttf"
    small = ImageFont.truetype(mono, 13 * s)
    d.text((12 * s, 10 * s), ">_", font=small, fill=fg + (200,))
    f = fit_font(d, label, FONT_DIR + "segoeuib.ttf", 60 * s, 30 * s)
    box = d.textbbox((0, 0), label, font=f)
    w, h = box[2] - box[0], box[3] - box[1]
    d.text(((SIZE * s - w) / 2 - box[0], 44 * s - h / 2 - box[1]), label, font=f, fill=fg + (255,))
    img = img.resize((SIZE, SIZE), Image.LANCZOS)
    raw = img.tobytes("raw", "BGRA")
    co = zlib.compressobj(9, zlib.DEFLATED, -15)
    open(out, "wb").write(co.compress(raw) + co.flush())
    if len(sys.argv) > 5 and sys.argv[5] == "--png":   # preview next to the icon
        img.save(out[:-2] + ".png")


if __name__ == "__main__":
    main()
