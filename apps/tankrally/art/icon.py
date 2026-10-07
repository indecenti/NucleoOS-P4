# Tank Rally icon: an olive tank on a sunset tile. Writes icon.png (preview) and icon.z (80x80 BGRA,
# raw deflate) next to the app.  python apps/tankrally/art/icon.py
import os
import zlib
from PIL import Image, ImageDraw

S = 320                       # drawn at 4x, then downsampled
APP = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def main():
    bg = Image.new('RGBA', (S, S))
    d = ImageDraw.Draw(bg)
    for y in range(S):                                   # sky: deep blue to orange, then the ground
        t = y / S
        c = lerp((40, 50, 120), (255, 150, 80), min(1, t / 0.62)) if t < 0.62 else lerp((92, 120, 60), (50, 76, 40), (t - 0.62) / 0.38)
        d.line([(0, y), (S, y)], fill=c + (255,))
    d.ellipse([200, 70, 260, 130], fill=(255, 230, 150, 255))                 # the sun
    mask = Image.new('L', (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, S - 1, S - 1], radius=72, fill=255)
    tile = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    tile.paste(bg, (0, 0), mask)
    d = ImageDraw.Draw(tile)
    olive, dark, track, wheel = (110, 128, 62), (78, 92, 44), (36, 36, 40), (90, 92, 98)
    d.ellipse([52, 232, 268, 262], fill=(20, 30, 16, 120))                    # shadow
    d.rounded_rectangle([48, 196, 272, 248], radius=24, fill=track)          # track
    for i in range(6):
        x = 74 + i * 34
        d.ellipse([x - 14, 208, x + 14, 236], fill=wheel)
        d.ellipse([x - 6, 216, x + 6, 228], fill=track)
    d.polygon([(60, 196), (80, 160), (250, 160), (270, 196)], fill=olive)    # hull
    d.rectangle([60, 186, 270, 198], fill=dark)
    d.line([(150, 112), (292, 92)], fill=dark, width=16)                     # barrel
    d.rectangle([280, 82, 300, 104], fill=track)                              # muzzle brake
    d.rounded_rectangle([104, 108, 206, 164], radius=18, fill=olive)         # turret
    d.rounded_rectangle([128, 92, 156, 112], radius=6, fill=dark)            # hatch
    cx, cy, r = 155, 136, 13                                                  # white star
    import math
    pts = [(cx + (r if k % 2 == 0 else r * 0.42) * math.sin(k * math.pi / 5), cy - (r if k % 2 == 0 else r * 0.42) * math.cos(k * math.pi / 5)) for k in range(10)]
    d.polygon(pts, fill=(240, 240, 230))
    img = tile.resize((80, 80), Image.LANCZOS)
    img.save(os.path.join(APP, 'icon.png'))
    bgra = img.tobytes('raw', 'BGRA')
    co = zlib.compressobj(9, zlib.DEFLATED, -15)
    open(os.path.join(APP, 'icon.z'), 'wb').write(co.compress(bgra) + co.flush())
    print('icon ok')


if __name__ == '__main__':
    main()
