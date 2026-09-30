"""make_gb_icon.py — store/launcher icon (icon.z) for the Game Boy apps.

    python ports/gameboy/make_gb_icon.py <out/icon.z> <bg #rrggbb> <canvas.ppm | "">

With a harness canvas dump (ports/gameboy/build.sh runs the game headless) the tile shows that frame
of the game in a small Game Boy screen; without one it draws a stylised handheld (generic player).
80x80 rounded tile, LVGL ARGB8888 byte order (B,G,R,A), raw-deflate compressed — the same icon.z
format as ports/make_icon.py. Add --png to also write a preview next to the icon.
"""
import sys
import zlib

from PIL import Image, ImageDraw

SIZE = 80


def rgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def game_frame(ppm):
    """The 160x144 game picture out of a 1024x600 canvas dump (4x or 3x, centred)."""
    im = Image.open(ppm).convert("RGB")
    for scale in (4, 3):
        gw, gh = 160 * scale, 144 * scale
        gx, gy = (1024 - gw) // 2, (600 - gh) // 2
        crop = im.crop((gx, gy, gx + gw, gy + gh))
        # the bezel colour must surround it: check one pixel left of the game area
        if im.getpixel((gx - 4, gy + gh // 2)) != im.getpixel((gx + 2, gy + gh // 2)):
            return crop.resize((160, 144), Image.NEAREST)
    return im.crop((192, 12, 832, 588)).resize((160, 144), Image.NEAREST)


def main():
    out, bg = sys.argv[1], rgb(sys.argv[2])
    ppm = sys.argv[3] if len(sys.argv) > 3 else ""
    s = 4
    img = Image.new("RGBA", (SIZE * s, SIZE * s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((2 * s, 2 * s, (SIZE - 2) * s, (SIZE - 2) * s), radius=18 * s, fill=bg + (255,))
    if ppm:
        # dark bezel + the game picture (160x144 -> 60x54)
        d.rounded_rectangle((7 * s, 9 * s, 73 * s, 71 * s), radius=6 * s, fill=(28, 30, 36, 255))
        pic = game_frame(ppm).resize((60 * s, 54 * s), Image.LANCZOS)
        img.paste(pic, (10 * s, 13 * s))
    else:
        # stylised DMG: body, green screen, d-pad, A/B
        body = (206, 206, 200, 255)
        d.rounded_rectangle((18 * s, 6 * s, 62 * s, 74 * s), radius=5 * s, fill=body)
        d.rounded_rectangle((18 * s, 58 * s, 62 * s, 74 * s), radius=10 * s, fill=body)
        d.rectangle((22 * s, 10 * s, 58 * s, 38 * s), fill=(80, 82, 96, 255))
        d.rectangle((26 * s, 13 * s, 54 * s, 35 * s), fill=(155, 188, 15, 255))
        d.rectangle((24 * s, 49 * s, 36 * s, 53 * s), fill=(40, 40, 44, 255))
        d.rectangle((28 * s, 45 * s, 32 * s, 57 * s), fill=(40, 40, 44, 255))
        d.ellipse((47 * s, 48 * s, 53 * s, 54 * s), fill=(150, 36, 84, 255))
        d.ellipse((53 * s, 44 * s, 59 * s, 50 * s), fill=(150, 36, 84, 255))
        d.rectangle((33 * s, 61 * s, 38 * s, 63 * s), fill=(90, 90, 96, 255))
        d.rectangle((42 * s, 61 * s, 47 * s, 63 * s), fill=(90, 90, 96, 255))
    img = img.resize((SIZE, SIZE), Image.LANCZOS)
    raw = img.tobytes("raw", "BGRA")
    co = zlib.compressobj(9, zlib.DEFLATED, -15)
    open(out, "wb").write(co.compress(raw) + co.flush())
    if "--png" in sys.argv:
        img.save(out[:-2] + ".png")


if __name__ == "__main__":
    main()
