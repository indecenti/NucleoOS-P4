"""gen_fonts.py — the reading fonts of the Libri app: Noto Serif (regular, bold, italic, bold italic),
cut down to the scripts the reader draws, into ports/books/fonts/ (committed). The sans and mono
fonts are the Biblioteca app's (ports/zim/biblioteca/fonts), made by its own gen_fonts.py.

    python ports/books/gen_fonts.py DIR

DIR holds the unhinted static TTFs of the Noto release (SIL OFL 1.1), checked here by SHA-256:
  https://github.com/notofonts/latin-greek-cyrillic/releases/download/NotoSerif-v2.015/NotoSerif-v2.015.zip
    (sha256 0e9a43c8a4b94ac76f55069ed1d7385bbcaf6b99527a94deb5619e032b7e76c1), NotoSerif/unhinted/ttf/

The app rasterises them at run time (stb_truetype), so only the outlines and metrics matter: the
OpenType layout tables (stb_truetype reads none of them) and the glyph names are dropped.
"""
import hashlib
import os
import sys

from fontTools import subset
from fontTools.ttLib import TTFont

FONTS = {   # output name: (source file, sha256 of the source)
    "serif.ttf": ("NotoSerif-Regular.ttf",
                  "a15cfbbc1539d707115111d672d590a3d70d4f74b4c0a315956da20ae19a14e1"),
    "serif-bold.ttf": ("NotoSerif-Bold.ttf",
                  "24ad531e6b05ddad8c3d89572d2c93eb86a6b74e652ce7ee3c3e171de68e84c3"),
    "serif-italic.ttf": ("NotoSerif-Italic.ttf",
                  "c4b3c971741ecdb40f5a443bce754e8fe91efe761b6ea10c92be7c3597cdadc4"),
    "serif-bolditalic.ttf": ("NotoSerif-BoldItalic.ttf",
                  "1bc4f86502eaa368718f6192bee022ea9a703d5af1c18b7d291212657b63074a"),
}

# The ranges of ports/zim/biblioteca/gen_fonts.py: Latin (with Vietnamese), IPA, combining marks,
# Greek (polytonic too), Cyrillic, punctuation, super/subscripts, currency, letterlike symbols,
# number forms, arrows, maths operators, a few shapes, U+FFFD.
RANGES = [(0x20, 0x7E), (0xA0, 0x24F), (0x250, 0x2AF), (0x2B0, 0x2FF), (0x300, 0x36F), (0x370, 0x3FF),
          (0x400, 0x52F), (0x1E00, 0x1EFF), (0x1F00, 0x1FFF), (0x2000, 0x206F), (0x2070, 0x209F), (0x20A0, 0x20C0),
          (0x2100, 0x214F), (0x2150, 0x218F), (0x2190, 0x21FF), (0x2200, 0x22FF), (0x2300, 0x23FF), (0x25A0, 0x25FF),
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
        print("%-22s %7d bytes, %4d glyphs  (from %s, sha256 %s)" % (name, os.path.getsize(dest), n, file, digest[:16]))


if __name__ == "__main__":
    main()
