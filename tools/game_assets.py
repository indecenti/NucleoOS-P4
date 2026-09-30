#!/usr/bin/env python3
"""game_assets.py — game assets from the local models, ready for the device.

Built on tools/qwen_assets.py (Qwen-Image 2.1 in ComfyUI, 127.0.0.1:8188) and tools/ace_music.py
(ACE-Step 1.5 turbo in the same ComfyUI). Everything lands as the device formats: img/*.565
(uint16 w,h little-endian + RGB565, magenta 0xF81F = transparent) and snd/*.wav (48 kHz mono 16-bit).

  # four assets from one 2x2 grid; kind: tex (seamless texture) | sprite (keyed, on navy) | art
  python tools/game_assets.py grid apps/bass/img --kind sprite --size 150x100 \
      --names fish0,fish1,fish2,fish3 --seed 21 \
      --cells "a largemouth bass, green with a dark stripe" "a rainbow trout" "a pike" "a catfish" \
      --subject "one freshwater fish in side view facing right, whole body visible, centred"

  # a 360-degree panorama band (1024x128, shore on the bottom row, sky keyed to magenta)
  python tools/game_assets.py pano apps/bass/img/p0.565 --seed 91 \
      --scene "dense pine forest, grey rocky mountains with snowy peaks, pale blue sky"

  # a full-screen painting (512x300)
  python tools/game_assets.py art apps/bass/img/title.565 --seed 7 --scene "an angler at dawn on a misty lake"

  # music (ACE-Step): instrumental, always with bpm and key
  python tools/game_assets.py music apps/bass/snd/lake0.wav --seconds 16 --bpm 110 --key "G major" \
      --seed 91 --tags "bright alpine morning, acoustic guitar, flute, cheerful"

Previews (.png) go next to a --preview directory (default: %TEMP%/game_assets). Same prompt + seed
= same image, so keep the commands in a script next to the game to regenerate.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

STYLE = ("beautiful detailed 16-bit era arcade video game art, vivid colours, clean shapes, rich detail, "
         "painterly pixel art, no text")
GRID = "A 2x2 grid of four separate, equally sized square panels, separated by thin white borders. "
POS = ["Top left", "Top right", "Bottom left", "Bottom right"]
KEYC = (255, 0, 255)


def _img():
    from PIL import Image, ImageDraw
    return Image, ImageDraw


def tileable(im, border=0.4):
    """Seamless: cross-fade with a copy shifted by half a tile, weighted toward the copy at the edges."""
    import numpy as np
    Image, _ = _img()
    a = np.asarray(im.convert("RGB")).astype(np.float32)
    h, w, _c = a.shape
    b = np.roll(np.roll(a, h // 2, 0), w // 2, 1)
    y = np.abs(np.linspace(-1, 1, h))[:, None]
    x = np.abs(np.linspace(-1, 1, w))[None, :]
    m = np.clip(np.maximum(x, y) * 1.4 - border, 0, 1)[..., None]
    return Image.fromarray((a * (1 - m) + b * m).astype(np.uint8))


def key_border(im, thresh=60):
    """Background -> magenta: flood fill from every border pixel (asked for a flat navy background)."""
    _, ImageDraw = _img()
    im = im.convert("RGB")
    w, h = im.size
    pts = [(x, y) for x in range(0, w, 3) for y in (0, h - 1)] + [(x, y) for y in range(0, h, 3) for x in (0, w - 1)]
    for p in pts:
        if im.getpixel(p) != KEYC:
            ImageDraw.floodfill(im, p, KEYC, thresh=thresh)
    return im


def key_top(im, thresh=44):
    """Sky -> magenta: flood fill from the top row (the engine shows its sky gradient there)."""
    _, ImageDraw = _img()
    im = im.convert("RGB")
    for x in range(0, im.width, 3):
        if im.getpixel((x, 0)) != KEYC:
            ImageDraw.floodfill(im, (x, 0), KEYC, thresh=thresh)
    return im


def wrap_x(im, blend=96):
    """Horizontal wrap for a 360-degree panorama: the right end fades into the left."""
    import numpy as np
    Image, _ = _img()
    a = np.asarray(im.convert("RGB")).astype(np.float32)
    w = a.shape[1]
    out = a[:, :w - blend].copy()
    t = np.linspace(0, 1, blend)[None, :, None]
    out[:, :blend] = a[:, :blend] * t + a[:, w - blend:] * (1 - t)
    return Image.fromarray(out.astype(np.uint8))


def sprite_fit(cell, w, h, thresh=60):
    """Crop the grid border, fit into w x h on the cell's own background, then key it."""
    Image, _ = _img()
    c = cell.convert("RGB").crop((16, 16, cell.width - 16, cell.height - 16))
    if h > w * 1.3:                                   # tall targets (trees): keep the middle strip
        cw = int(c.width * w / h * 1.25)
        x0 = (c.width - cw) // 2
        c = c.crop((x0, 0, x0 + cw, c.height))
    bg = c.getpixel((4, 4))
    c.thumbnail((w, h), Image.LANCZOS)
    out = Image.new("RGB", (w, h), bg)
    out.paste(c, ((w - c.width) // 2, (h - c.height) // 2))
    return key_border(out, thresh)


def preview_dir(args):
    d = args.preview or os.path.join(os.environ.get("TEMP", "/tmp"), "game_assets")
    os.makedirs(d, exist_ok=True)
    return d


def cmd_grid(args):
    import qwen_assets as q
    Image, _ = _img()
    names = args.names.split(",")
    assert len(names) == 4 and len(args.cells) == 4, "four --names and four --cells"
    w, h = (int(v) for v in args.size.lower().split("x"))
    if args.kind == "tex":
        head = GRID + ("Each panel is a seamless tileable texture, flat orthographic view, even lighting, no shadows, "
                       "no objects, " + STYLE + ". ")
    elif args.kind == "sprite":
        head = GRID + "Each panel on a plain flat deep navy blue background, " + STYLE + ". "
        if args.subject:
            head += "Each panel shows " + args.subject + ". "
    else:
        head = GRID + "Each panel is a painting, " + STYLE + ". "
    prompt = head + " ".join("%s: %s." % (p, c.rstrip(".")) for p, c in zip(POS, args.cells))
    img = q.generate(prompt, 1024, 1024, args.seed)
    pv = preview_dir(args)
    img.save(os.path.join(pv, "grid_%s.png" % names[0]))
    for n, cell in zip(names, q.split_grid(img)):
        if args.kind == "tex":
            c = cell.crop((14, 14, cell.width - 14, cell.height - 14)).resize((256, 256), Image.LANCZOS)
            t = tileable(c).resize((w, h), Image.LANCZOS)
        elif args.kind == "sprite":
            t = sprite_fit(cell, w, h)
            if args.billboard:
                t = t.transpose(Image.FLIP_TOP_BOTTOM)     # VX_BILLBOARD: row 0 is the bottom
        else:
            t = q.fit(cell.crop((10, 10, cell.width - 10, cell.height - 10)), w, h)
        t.save(os.path.join(pv, n + ".png"))
        q.to565(t, os.path.join(args.outdir, n + ".565"))
        print("wrote", os.path.join(args.outdir, n + ".565"))


def cmd_pano(args):
    import qwen_assets as q
    Image, _ = _img()
    prompt = ("A very wide seamless panoramic landscape, " + STYLE + ". The ground line runs straight along the "
              "very bottom edge of the image, the land rising from the bottom edge. " + args.scene)
    img = q.generate(prompt, 2048, 512, args.seed)
    import numpy as np
    wide = img.convert("RGB").resize((1024 + 96, 256), Image.LANCZOS)
    wide = key_top(wrap_x(wide, 96), args.thresh)      # wrap, key the sky from the top, then crop
    # The engine spreads 1024 texels over 360 degrees (~3 screen px each): keep the landscape low —
    # the rows above the shore, shrunk to 256 wide, repeated 4x at the bottom, the rest keyed sky.
    crop = wide.crop((0, args.shore - args.height, 1024, args.shore))
    h = round(args.height * 256 / 1024)
    q4 = np.asarray(crop.resize((256, h), Image.LANCZOS)).astype(np.int32)
    q4[(q4[..., 0] > 190) & (q4[..., 2] > 190) & (q4[..., 1] < 130)] = KEYC   # re-key blended sky
    q4 = Image.fromarray(q4.astype(np.uint8))
    band = Image.new("RGB", (1024, 128), KEYC)
    for c in range(4):
        band.paste(q4, (c * 256, 128 - h))
    band.save(os.path.join(preview_dir(args), os.path.basename(args.out) + ".png"))
    q.to565(band, args.out)
    print("wrote", args.out)


def cmd_art(args):
    import qwen_assets as q
    img = q.generate("A painting, " + STYLE + ". " + args.scene, 1024, 640, args.seed)
    t = q.fit(img, 512, 300)
    t.save(os.path.join(preview_dir(args), os.path.basename(args.out) + ".png"))
    q.to565(t, args.out)
    print("wrote", args.out)


def cmd_music(args):
    import ace_music as a
    tags = "instrumental, 1990s arcade video game music, 16-bit era, catchy melody, " + args.tags
    data, rate = a.generate(tags, args.seconds, args.bpm, args.key, args.seed)
    a.to_device_wav(data, rate, args.out)
    print("wrote", args.out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--preview", help="where to put PNG previews")
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("grid"); g.add_argument("outdir"); g.add_argument("--kind", choices=["tex", "sprite", "art"], required=True)
    g.add_argument("--size", default="128x128"); g.add_argument("--names", required=True)
    g.add_argument("--cells", nargs=4, required=True); g.add_argument("--subject", default="")
    g.add_argument("--billboard", action="store_true", help="sprite for a VX_BILLBOARD (stored bottom-up)")
    g.add_argument("--seed", type=int, default=1)
    p = sub.add_parser("pano"); p.add_argument("out"); p.add_argument("--scene", required=True)
    p.add_argument("--seed", type=int, default=1); p.add_argument("--thresh", type=int, default=44)
    p.add_argument("--shore", type=int, default=250, help="shore row at 256 scale (check the preview: Qwen often paints water below it)")
    p.add_argument("--height", type=int, default=120, help="rows above the shore to keep (256 scale)")
    r = sub.add_parser("art"); r.add_argument("out"); r.add_argument("--scene", required=True); r.add_argument("--seed", type=int, default=1)
    m = sub.add_parser("music"); m.add_argument("out"); m.add_argument("--tags", required=True)
    m.add_argument("--seconds", type=int, default=16); m.add_argument("--bpm", type=int, default=110)
    m.add_argument("--key", default="C major"); m.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()
    {"grid": cmd_grid, "pano": cmd_pano, "art": cmd_art, "music": cmd_music}[args.cmd](args)


if __name__ == "__main__":
    main()
