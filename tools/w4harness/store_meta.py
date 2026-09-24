#!/usr/bin/env python3
"""store_meta.py — store metadata for one wasm4.org gallery cart (called by store.sh).

    python3 store_meta.py <cart.md> <cart.png> <app id> <out dir> [translations.json [curation.json]]

Writes into <out dir>:
  manifest.json  id, name, version, "wasm4": true, author, license, source (the cart's page on
                 wasm4.org), description (English) and "descriptions" per language when the
                 translations file has this id ({"<id>": {"it": "..."}}), and the editorial
                 "featured" from the curation file. The device runtime reads only
                 id/name/version/wasm4; the rest is for the store server.
  icon.z         80x80 launcher / store icon: the cart's gallery picture halved (nearest, so the
                 pixel art stays crisp) with rounded corners, LVGL ARGB8888 byte order (B,G,R,A),
                 raw-deflate compressed like the firmware's built-in icons (~1-2 KB).
"""
import json
import re
import sys
import zlib

LICENSE = "CC BY-NC-SA 4.0"   # the whole gallery archive (site/static/carts/README.md)
DESC_MAX = 240                # the store server trims to what the device shows
ICON = 80
RADIUS = 14

# Paragraphs that aren't a description of the game.
SKIP = re.compile(r"^(source|sources|code|made (with|for|in)|written in|built with|controls?|how to|"
                  r"credits?|thanks|license|github|play|download|note|update|changelog|version)\b", re.I)


def plain(md):
    md = re.sub(r"!\[[^\]]*\]\([^)]*\)", "", md)            # images
    md = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", md)        # links -> their text
    md = re.sub(r"<[^>]+>", "", md)                         # inline html
    md = re.sub(r"[*_`~]", "", md)
    return " ".join(md.split())


def parse_md(path, fallback_name):
    name, author, paras = fallback_name, "", []
    try:
        text = open(path, encoding="utf-8").read()
    except OSError:
        return name, author, ""
    m = re.match(r"^---\n(.*?)\n---\n", text, re.S)
    if m:
        for line in m.group(1).splitlines():
            if line.startswith("author:"):
                author = re.sub(r"\s*<[^>]*>", "", line[7:]).strip().strip("\"'")
        text = text[m.end():]
    title = re.search(r"^#\s+(.+)$", text, re.M)
    if title:
        name = plain(title.group(1)) or name
        text = text[title.end():]
    for para in re.split(r"\n\s*\n", text):
        p = para.strip()
        if p.startswith("#"):
            if paras:
                break                                        # "## Controls" etc. end the blurb
            continue
        if not p or p[0] in "!|-*>" or re.match(r"^\d+\.", p) or p.startswith("```"):
            continue
        p = plain(p)
        if len(p) < 3 or SKIP.match(p) or re.fullmatch(r"https?://\S+", p):
            continue
        paras.append(p)
    desc = ""
    for p in paras:                                          # a short first line gets company
        desc = (desc + " " + p).strip() if desc else p
        if len(desc) >= 80:
            break
    if len(desc) > DESC_MAX:
        desc = desc[:DESC_MAX - 3].rsplit(" ", 1)[0].rstrip(",;:.") + "..."
    return name, author, desc


def icon_z(png_path):
    from PIL import Image
    img = Image.open(png_path).convert("RGBA")
    if img.size != (160, 160):
        img = img.resize((160, 160), Image.NEAREST)
    img = img.resize((ICON, ICON), Image.NEAREST)
    px = img.load()
    out = bytearray(ICON * ICON * 4)
    for y in range(ICON):
        for x in range(ICON):
            r, g, b, a = px[x, y]
            cx = RADIUS - x if x < RADIUS else (x - (ICON - RADIUS - 1) if x >= ICON - RADIUS else 0)
            cy = RADIUS - y if y < RADIUS else (y - (ICON - RADIUS - 1) if y >= ICON - RADIUS else 0)
            if cx * cx + cy * cy > RADIUS * RADIUS:
                a = 0
            i = (y * ICON + x) * 4
            out[i:i + 4] = bytes((b, g, r, a))
    co = zlib.compressobj(9, zlib.DEFLATED, -15)             # raw deflate, as tinfl reads it
    return co.compress(bytes(out)) + co.flush()


def main():
    md, png, app_id, out = sys.argv[1:5]
    tr, cur = {}, {}
    if len(sys.argv) > 5 and sys.argv[5]:
        try:
            tr = json.load(open(sys.argv[5], encoding="utf-8")).get(app_id, {})
        except (OSError, ValueError):
            tr = {}
    if len(sys.argv) > 6:
        try:
            cur = json.load(open(sys.argv[6], encoding="utf-8"))
        except (OSError, ValueError):
            cur = {}
    cart = re.sub(r"\.md$", "", md.replace("\\", "/").rsplit("/", 1)[-1])
    name, author, desc = parse_md(md, app_id.replace("-", " ").title())
    desc = tr.get("en") or desc                              # a curated English text wins
    man = {"id": app_id, "name": name[:39], "version": "1.0", "wasm4": True,
           "author": author[:39], "license": LICENSE, "source": f"https://wasm4.org/play/{cart}",
           "description": desc}
    if tr:
        man["descriptions"] = {"en": desc, **{k: v for k, v in tr.items() if v}}
    if app_id in cur.get("featured", []):
        man["featured"] = True
    json.dump(man, open(f"{out}/manifest.json", "w", encoding="utf-8"), ensure_ascii=False, indent=2)
    try:
        open(f"{out}/icon.z", "wb").write(icon_z(png))
    except Exception as e:                                   # no picture: the store shows the generic tile
        print(f"  {app_id}: no icon ({e})", file=sys.stderr)


if __name__ == "__main__":
    main()
