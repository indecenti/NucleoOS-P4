"""make_icon.py — store/launcher icon (icon.z) for a terminal program.

    python ports/make_icon.py <out/icon.z> <label> <bg #rrggbb> <fg #rrggbb>

80x80 rounded tile with a short label and a "terminal" badge (dark chip, green ">_", bottom right) that
tells at a glance the program has no window of its own: it runs inside the Terminal. Same badge on
every terminal program, in the Store and on the Home screen, in the LVGL ARGB8888 byte
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
    f = fit_font(d, label, FONT_DIR + "segoeuib.ttf", 60 * s, 30 * s)
    box = d.textbbox((0, 0), label, font=f)
    w, h = box[2] - box[0], box[3] - box[1]
    d.text(((SIZE * s - w) / 2 - box[0], 32 * s - h / 2 - box[1]), label, font=f, fill=fg + (255,))
    # terminal badge: near-black chip with a light rim so it reads on dark and light tiles alike
    bx0, by0, bx1, by1 = 40 * s, 53 * s, 72 * s, 71 * s
    d.rounded_rectangle((bx0, by0, bx1, by1), radius=6 * s, fill=(12, 14, 16, 255),
                        outline=(255, 255, 255, 110), width=int(1.2 * s))
    mono = ImageFont.truetype(FONT_DIR + "consolab.ttf", 13 * s)
    mb = d.textbbox((0, 0), ">_", font=mono)
    d.text(((bx0 + bx1) / 2 - (mb[2] - mb[0]) / 2 - mb[0], (by0 + by1) / 2 - (mb[3] - mb[1]) / 2 - mb[1]),
           ">_", font=mono, fill=(74, 222, 128, 255))
    img = img.resize((SIZE, SIZE), Image.LANCZOS)
    raw = img.tobytes("raw", "BGRA")
    co = zlib.compressobj(9, zlib.DEFLATED, -15)
    open(out, "wb").write(co.compress(raw) + co.flush())
    if len(sys.argv) > 5 and sys.argv[5] == "--png":   # preview next to the icon
        img.save(out[:-2] + ".png")


if __name__ == "__main__":
    main()
