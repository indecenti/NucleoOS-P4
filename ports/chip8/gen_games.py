"""gen_games.py — turn the CHIP-8 Community Archive into the chip8 app's game table.

    python ports/chip8/gen_games.py <chip8Archive dir> <out games.h> [--md ports/chip8/GAMES.md]

Reads programs.json (title, authors, desc, howto, platform, options) and roms/<key>.ch8 and writes a
C header with every bundled ROM concatenated in one array plus a table of per-game settings:
tickrate, Octo quirk flags, palette (RGB565), screen rotation and the keys the game uses (from the
"howto" text, else a static scan of the ROM for "vX := NN; if vX key/-key" pairs). The whole
archive is CC0 (see its Readme "Licensing"); --md writes the per-game license/author record.
"""
import json
import os
import re
import sys

from PIL import ImageColor

# Not bundled: the Octojam greeting screens (not games), an empty template, and the multi-screen
# XO-CHIP audio/video demos that fill the whole 64 KB (rhythm demos, "Kesha" stories, 10000
# instructions/frame RPG): they would triple the app for things that are not really playable.
SKIP = {f"octojam{n}title" for n in range(1, 11)} | {f"jub8-{n}" for n in range(1, 7)} | {
    "keshaWasBird", "keshaWasBiird", "keshaWasNiiinja", "redOctober", "nokiatemplate"}

# Octo defaults when a program has no options.
DEF = {"tickrate": 20, "backgroundColor": "#996600", "fillColor": "#FFCC00", "fillColor2": "#FF6600",
       "blendColor": "#662200", "buzzColor": "#FFAA00", "quietColor": "#000000"}
QUIRKS = ["shiftQuirks", "loadStoreQuirks", "vfOrderQuirks", "clipQuirks", "jumpQuirks",
          "vBlankQuirks", "logicQuirks"]
PLATFORM = {"chip8": 0, "schip": 1, "xochip": 2}
FONT_OK = set(" 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-.:%/<>!+")


def truthy(v):
    if isinstance(v, str):
        return v.strip().lower() in ("1", "true", "yes")
    return bool(v)


def rgb565(c):
    c = str(c).strip()
    if re.fullmatch(r"[0-9A-Fa-f]{6}", c):
        c = "#" + c
    r, g, b = ImageColor.getrgb(c)[:3]
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def font_text(s):
    """Uppercase and keep only what the OS 5x7 font can draw."""
    s = s.upper().replace("&", "+").replace("?", ".").replace(";", ".").replace("(", "- ").replace(")", "")
    s = s.replace("’", "").replace("'", "").replace('"', "").replace(",", "")
    s = "".join(ch if ch in FONT_OK else " " for ch in s)
    return re.sub(r" +", " ", s).strip()


def author_name(key):
    if key == "your name here":
        return "ANONYMOUS"
    return re.sub(r"(?<=[a-z])(?=[A-Z])", " ", key)


def keys_from_howto(txt):
    mask = 0
    for m in re.finditer(r"\b[Kk]eys?\b((?:\s*(?:,|and|or|to)?\s*\b[0-9A-F]\b)+)", txt):
        for k in re.findall(r"\b[0-9A-F]\b", m.group(1)):
            mask |= 1 << int(k, 16)
    return mask


def keys_from_rom(rom):
    """vX := NN ... if vX key / -key within the next few instructions (either alignment)."""
    mask = 0
    for i in range(0, len(rom) - 1):
        op = rom[i] << 8 | rom[i + 1]
        if (op & 0xF0FF) not in (0xE09E, 0xE0A1):
            continue
        x = (op >> 8) & 0xF
        for back in (2, 4, 6):
            j = i - back
            if j < 0:
                break
            p = rom[j] << 8 | rom[j + 1]
            if (p & 0xFF00) == (0x6000 | x << 8):
                mask |= 1 << (p & 0xF)
                break
    return mask


def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    arch, out = sys.argv[1], sys.argv[2]
    md = sys.argv[sys.argv.index("--md") + 1] if "--md" in sys.argv else None
    progs = json.load(open(os.path.join(arch, "programs.json"), encoding="utf-8"))
    entries, blob, rows = [], bytearray(), []
    for key, p in progs.items():
        if key in SKIP:
            continue
        rom = open(os.path.join(arch, "roms", key + ".ch8"), "rb").read()
        o = dict(DEF)
        o.update({k: v for k, v in (p.get("options") or {}).items() if v is not None})
        tick = int(o.get("tickrate") or 20)
        q = 0
        for bit, name in enumerate(QUIRKS):
            if truthy(o.get(name, False)):
                q |= 1 << bit
        rot = int(o.get("screenRotation") or 0) % 360
        howto = p.get("howto") or ""
        keys = keys_from_howto(howto) or keys_from_rom(rom)
        m = re.search(r"\b[Kk]ey ([0-9A-F]) (?:to start|starts)\b", howto)
        start = int(m.group(1), 16) if m else 0xFF
        cols = [rgb565(o[c]) for c in ("backgroundColor", "fillColor", "fillColor2", "blendColor",
                                       "buzzColor", "quietColor")]
        info = font_text(howto or p.get("desc", ""))
        authors = [author_name(a) for a in p["authors"]]
        entries.append((key, font_text(p["title"]), font_text(" / ".join(authors)), info, len(blob), len(rom),
                        PLATFORM.get(p.get("platform", "chip8"), 0), rot, tick, q, keys, start, cols))
        rows.append((p["title"], ", ".join("anonymous (`your name here`)" if a == "your name here" else a for a in p["authors"]), p.get("platform", "chip8"), len(rom), key))
        blob += rom
    # menu order: alphabetical by title
    order = sorted(range(len(entries)), key=lambda i: entries[i][1])
    with open(out, "w", encoding="ascii") as f:
        f.write("// Generated by ports/chip8/gen_games.py from the CHIP-8 Community Archive (CC0).\n")
        f.write(f"#define C8_NGAMES {len(entries)}\n")
        f.write(f"static const unsigned char c8_roms[{len(blob)}] = {{")
        for i, b in enumerate(blob):
            f.write(("\n" if i % 32 == 0 else "") + str(b) + ",")
        f.write("\n};\n")
        f.write("static const c8_game_t c8_games[C8_NGAMES] = {\n")
        for i in order:
            key, title, author, info, off, n, plat, rot, tick, q, keys, start, cols = entries[i]
            f.write(f"  {{{cstr(key)}, {cstr(title)}, {cstr(author)}, {cstr(info)}, {off}, {n}, {plat}, "
                    f"{rot}, {tick}, 0x{q:02x}, 0x{keys:04x}, 0x{start:02x}, {{{', '.join('0x%04x' % c for c in cols)}}}}},\n")
        f.write("};\n")
    print(f"{len(entries)} games, {len(blob)} bytes of ROM -> {out}")
    if md:
        rows.sort(key=lambda r: r[0].lower())
        with open(md, "w", encoding="utf-8", newline="\n") as f:
            f.write("# CHIP-8 app: bundled games\n\n")
            f.write("Every ROM comes from the [CHIP-8 Community Archive](https://github.com/JohnEarnest/chip8Archive) "
                    "by John Earnest, pinned to commit `761e3ffc63f43e6a849e80714122feff2afca208` "
                    "(see `ports/chip8/fetch.sh`). The archive's Readme places everything in the repository "
                    "under **Creative Commons 0** (\"No Rights Reserved\"), and contributors agree to that "
                    "license by submitting; so each game below is **CC0**. Titles, authors (keys of the "
                    "archive's `authors.json`), platform and settings come from its `programs.json`.\n\n")
            f.write("Not bundled: the ten Octojam title screens (greetings, not games), `nokiatemplate` "
                    "(an empty template) and the 64 KB XO-CHIP audio/video demos (`jub8-1`..`6`, "
                    "`keshaWasBird`, `keshaWasBiird`, `keshaWasNiiinja`, `redOctober`).\n\n")
            f.write(f"{len(rows)} games, {len(blob)} bytes of ROM in total.\n\n")
            f.write("| Game | Author(s) | Platform | Bytes | ROM | License |\n|---|---|---|---:|---|---|\n")
            for t, a, pl, n, key in rows:
                f.write(f"| {t} | {a} | {pl} | {n} | `roms/{key}.ch8` | CC0 |\n")
        print(f"wrote {md}")


if __name__ == "__main__":
    main()
