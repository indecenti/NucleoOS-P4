"""store_shots.py — the store's screenshots of Libri (512 x 300 baseline JPEG) from the PC harness's
shots (test/store.script), never from the board (its status bar shows the network's name).

    python ports/books/tools/store_shots.py SHOTS_DIR OUT_DIR  name[,name2] ...

Each argument is one store picture: a landscape shot scaled to 512 x 300, or two portrait shots
("a,b", or one "a,") turned upright and set side by side on a quiet background. Portrait shots are
the canvas as the app draws it: upright (x, y) is canvas (y, 599 - x).
"""
import os
import sys

from PIL import Image

W, H = 512, 300
BG = (226, 224, 219)


def upright(im):
    return im.transpose(Image.Transpose.ROTATE_270)   # clockwise


def portrait(paths):
    out = Image.new("RGB", (W, H), BG)
    pages = [upright(Image.open(p).convert("RGB")) for p in paths]
    ph = H - 24
    pw = round(ph * 600 / 1024)
    gap = 28
    x = (W - (len(pages) * pw + (len(pages) - 1) * gap)) // 2
    for pg in pages:
        pg = pg.resize((pw, ph), Image.Resampling.LANCZOS)
        out.paste((150, 148, 144), (x - 1, 11, x + pw + 1, 13 + ph))   # a hairline frame
        out.paste(pg, (x, 12))
        x += pw + gap
    return out


def main():
    src, dst = sys.argv[1], sys.argv[2]
    os.makedirs(dst, exist_ok=True)
    for n, arg in enumerate(sys.argv[3:], 1):
        names = [a for a in arg.split(",") if a]
        paths = [os.path.join(src, a + ".png") for a in names]
        if "," in arg:
            im = portrait(paths)
        else:
            im = Image.open(paths[0]).convert("RGB").resize((W, H), Image.Resampling.LANCZOS)
        out = os.path.join(dst, "%d.jpg" % n)
        im.save(out, "JPEG", quality=88, optimize=True, progressive=False)
        print(out, "<-", arg)


if __name__ == "__main__":
    main()
