"""gen_fonts.py — the fonts of the Biblioteca app: Noto Sans (regular, bold, italic, bold italic) and
Noto Sans Mono, cut down to the scripts the reader draws, into ports/zim/biblioteca/fonts/ (committed).

    python ports/zim/biblioteca/gen_fonts.py DIR

DIR holds the unhinted static TTFs from the Noto releases (SIL OFL 1.1), checked here by SHA-256:
  https://github.com/notofonts/latin-greek-cyrillic/releases/download/NotoSans-v2.015/NotoSans-v2.015.zip
    (sha256 0c34df072a3fa7efbb7cbf34950e1f971a4447cffe365d3a359e2d4089b958f5), NotoSans/unhinted/ttf/
  https://github.com/notofonts/latin-greek-cyrillic/releases/download/NotoSansMono-v2.014/NotoSansMono-v2.014.zip
    (sha256 090cf6c5e03f337a755630ca888b1fef463e64ae7b33ee134e9309c05f978732), NotoSansMono/unhinted/ttf/

The app rasterises them at run time (stb_truetype), so only the outlines and metrics matter: the
OpenType layout tables (stb_truetype reads none of them) and the glyph names are dropped.
"""
import hashlib
import os
import sys

from fontTools import subset
from fontTools.ttLib import TTFont

FONTS = {   # output name: (source file, sha256 of the source)
    "sans.ttf": ("NotoSans-Regular.ttf",
                  "f3961a9cde016d41a4879aecda1474d3a36d6bf54fa0e4643de029cc2248b0e8"),
    "sans-bold.ttf": ("NotoSans-Bold.ttf",
                  "87cb2d84472a7d66da659ee47b6cdb9552326e8c128245231f191b6ac72529d9"),
    "sans-italic.ttf": ("NotoSans-Italic.ttf",
                  "678288f868807d4d64a6f3b51466871d117d915780381ce9d0ed4b3bcbd06d37"),
    "sans-bolditalic.ttf": ("NotoSans-BoldItalic.ttf",
                  "3d367743f371f28671d2764e911a53d7c20ec9b6aa8791d059e7090389fc52a5"),
    "mono.ttf": ("NotoSansMono-Regular.ttf",
                  "87f8ce0522a6c99b743ee5fc75b4073cfdd575639119672828b7b9944b65b4f4"),
}

# Latin (Western, Central and Eastern European, Turkish, Romanian, Vietnamese tones as composed
# letters), Greek, Cyrillic, punctuation, super/subscripts, currency, letterlike symbols, number
# forms, arrows, the maths operators Wikipedia uses in running text, a few shapes, U+FFFD.
RANGES = [(0x20, 0x7E), (0xA0, 0x24F), (0x259, 0x259), (0x2B0, 0x2FF), (0x370, 0x3FF), (0x400, 0x52F),
          (0x1E00, 0x1EFF), (0x2000, 0x206F), (0x2070, 0x209F), (0x20A0, 0x20C0), (0x2100, 0x214F),
          (0x2150, 0x218F), (0x2190, 0x21FF), (0x2200, 0x22FF), (0x2300, 0x23FF), (0x25A0, 0x25FF),
          (0x2600, 0x26FF), (0x2700, 0x27BF), (0xFB00, 0xFB06), (0xFFFD, 0xFFFD)]


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "."
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fonts")
    os.makedirs(out, exist_ok=True)
    unicodes = [c for a, b in RANGES for c in range(a, b + 1)]
    for name, (file, want) in FONTS.items():
        path = os.path.join(src, file)
        with open(path, "rb") as f:
            digest = hashlib.sha256(f.read()).hexdigest()
        if digest != want:
            sys.exit("%s: sha256 %s, expected %s" % (file, digest, want))
        opts = subset.Options()
        opts.layout_features = []
        opts.drop_tables += ["GSUB", "GPOS", "GDEF", "BASE", "JSTF", "MATH", "DSIG", "meta", "STAT", "fvar"]
        opts.name_IDs = [0, 1, 2, 3, 4, 5, 6, 13, 14]   # keep the copyright and the OFL notice
        opts.glyph_names = False
        opts.notdef_outline = True
        opts.hinting = False
        font = TTFont(path)
        sub = subset.Subsetter(opts)
        sub.populate(unicodes=unicodes)
        sub.subset(font)
        dest = os.path.join(out, name)
        font.save(dest)
        n = len(font.getGlyphOrder())
        print("%-20s %7d bytes, %4d glyphs  (from %s, sha256 %s)" % (name, os.path.getsize(dest), n, file, digest[:16]))


if __name__ == "__main__":
    main()
