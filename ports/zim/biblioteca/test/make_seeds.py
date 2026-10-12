"""make_seeds.py — seeds for test/fuzz_img.c: one small picture per format the app decodes, each with
the two size bytes in front, and a formula written the way MathJax does (<defs>, <use>, currentColor).

    python ports/zim/biblioteca/test/make_seeds.py OUTDIR
"""
import io
import os
import sys

from PIL import Image, ImageDraw

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
im = Image.new("RGBA", (24, 16), (255, 255, 255, 0))
d = ImageDraw.Draw(im)
d.ellipse((2, 2, 21, 13), fill=(30, 90, 200, 255))
d.line((0, 15, 23, 0), fill=(200, 40, 40, 180), width=2)
for fmt, name, mode in (("PNG", "a.png", "RGBA"), ("GIF", "a.gif", "P"), ("JPEG", "a.jpg", "RGB"), ("BMP", "a.bmp", "RGB"),
                        ("WEBP", "a.webp", "RGBA")):
    b = io.BytesIO()
    src = im.convert(mode) if mode != "P" else im.convert("RGB").convert("P")
    kw = {"lossless": True} if fmt == "WEBP" else {}
    src.save(b, fmt, **kw)
    open(os.path.join(out, name), "wb").write(bytes([20, 12]) + b.getvalue())
b = io.BytesIO()
im.save(b, "WEBP", quality=60)
open(os.path.join(out, "lossy.webp"), "wb").write(bytes([30, 20]) + b.getvalue())
svg = (b'<svg xmlns:xlink="http://www.w3.org/1999/xlink" width="5.9ex" height="2.3ex" viewBox="0 -791 2558 1008" '
       b'xmlns="http://www.w3.org/2000/svg"><defs><path stroke-width="1" id="G1" d="M56 237T56 250T70 270H369V420L370 '
       b'570Q380 583 389 583Q402 583 409 568V270H707Q722 262 722 250T707 230H409V-68Q401 -82 391 -82H389Q375 -82 369 '
       b'-68V230H70Q56 237 56 250Z"></path><path id="G2" d="M0 0L300 0L300 600L0 600Z"></path></defs>'
       b'<g stroke="currentColor" fill="currentColor" stroke-width="0" transform="matrix(1 0 0 -1 0 0)">'
       b'<use xlink:href="#G1" x="0" y="0"></use><use transform="scale(0.707)" xlink:href="#G2" x="1021" y="-435"/>'
       b'<g transform="translate(1445,0)"><use href="#G1"/><rect x="0" y="220" width="600" height="60"/></g>'
       b'<use xlink:href="#missing" x="1" y="1"/></g></svg>')
open(os.path.join(out, "math.svg"), "wb").write(bytes([40, 16]) + svg)
print("%d seeds in %s" % (len(os.listdir(out)), out))
