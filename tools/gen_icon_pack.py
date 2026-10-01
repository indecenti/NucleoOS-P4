"""NucleoOS "line" icon pack for the built-in apps, from Phosphor Icons (MIT, phosphoricons.com).

Each icon is the Phosphor glyph, white on transparent, 80x80 ARGB8888 (LVGL byte order B,G,R,A):
the board recolours it with the active desktop / theme accent, so one pack fits every palette.
Output: tools/assets/icons_line/<app id>.argb (+ a preview sheet). Install on the SD card in
/sdcard/system/icons/line/.

    python tools/gen_icon_pack.py <path to Phosphor.ttf> <path to style.css>
"""
import re
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

SIZE = 80
GLYPH = 58
OUT = Path(__file__).resolve().parent / "assets" / "icons_line"

# Built-in app id -> Phosphor icon name.
MAP = {
    "files": "folder-simple", "settings": "gear-six", "apps": "storefront", "anima": "sparkle",
    "gallery": "images", "camera": "camera", "music": "music-notes", "video": "film-strip",
    "notes": "note-pencil", "calc": "calculator", "terminal": "terminal-window",
    "sysmon": "chart-line", "diag": "pulse", "recorder": "microphone", "secondscreen": "monitor",
    "tasks": "list-checks",
}


def codepoints(css: str) -> dict:
    out = {}
    for name, cp in re.findall(r'\.ph\.ph-([a-z0-9-]+):before\s*\{\s*content:\s*"\\([0-9a-f]+)"', css):
        out[name] = int(cp, 16)
    return out


def main() -> None:
    ttf, css = sys.argv[1], sys.argv[2]
    cps = codepoints(open(css, encoding="utf-8").read())
    font = ImageFont.truetype(ttf, GLYPH)
    OUT.mkdir(parents=True, exist_ok=True)
    sheet = Image.new("RGBA", (SIZE * len(MAP), SIZE), (8, 20, 12, 255))
    for i, (app, name) in enumerate(MAP.items()):
        ch = chr(cps[name])
        img = Image.new("RGBA", (SIZE, SIZE), (255, 255, 255, 0))
        d = ImageDraw.Draw(img)
        l, t, r, b = d.textbbox((0, 0), ch, font=font)
        d.text(((SIZE - (r - l)) / 2 - l, (SIZE - (b - t)) / 2 - t), ch, font=font, fill=(255, 255, 255, 255))
        raw = bytearray()
        for px in img.getdata():
            raw += bytes((255, 255, 255, px[3]))      # B, G, R, A (white; alpha = the glyph)
        (OUT / f"{app}.argb").write_bytes(bytes(raw))
        tint = Image.new("RGBA", (SIZE, SIZE), (57, 255, 106, 255))
        tint.putalpha(img.getchannel("A"))
        sheet.alpha_composite(tint, (i * SIZE, 0))
    sheet.save(OUT / "preview.png")
    (OUT / "LICENSE-phosphor.txt").write_text(
        "Icons: Phosphor Icons (https://phosphoricons.com), MIT License, (c) 2020-2021 Phosphor Icons.\n",
        encoding="utf-8")
    print("ok", len(MAP), "icons ->", OUT)


if __name__ == "__main__":
    main()
