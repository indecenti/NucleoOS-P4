#!/usr/bin/env python3
# Build ABC 123 icon assets: download OpenMoji (CC BY-SA 4.0) color emoji, composite transparency to
# a magenta color-key, and write fixed-size RGB565 blobs the nv_gfx_image host import can blit.
#
# Blob format (little-endian): u16 width, u16 height, then width*height RGB565 pixels.
# Transparent pixels (alpha < 128) become 0xF81F (magenta) — the host skips them when blitting, so
# an icon composites cleanly over any background.
#
#   python tools/build_abc_assets.py
#
# Output: apps/abc123/img/<name>.565   (200x200 each)
import os, struct, urllib.request
from PIL import Image
import io

SIZE = 200
KEY  = 0xF81F  # magenta = transparent
BASE = "https://raw.githubusercontent.com/hfg-gmuend/openmoji/master/color/618x618/"
OUT  = os.path.join(os.path.dirname(__file__), "..", "apps", "abc123", "img")

# name -> OpenMoji codepoint (uppercase hex filename). Order matches the app's object enum.
ICONS = {
    "sun": "2600", "moon": "1F319", "star": "2B50", "cloud": "2601",
    "tree": "1F333", "flower": "1F338", "leaf": "1F343",
    "apple": "1F34E", "banana": "1F34C", "grapes": "1F347", "strawberry": "1F353",
    "carrot": "1F955", "cake": "1F370", "icecream": "1F366", "pizza": "1F355", "egg": "1F95A",
    "house": "1F3E0", "ball": "26BD", "car": "1F697", "boat": "26F5", "train": "1F686",
    "book": "1F4D6", "key": "1F511", "cup": "2615", "hat": "1F3A9", "shoe": "1F45F",
    "clock": "1F550", "balloon": "1F388", "gift": "1F381", "bell": "1F514",
    "umbrella": "2602", "guitar": "1F3B8",
    "cat": "1F431", "dog": "1F436", "fish": "1F41F", "bird": "1F426", "bee": "1F41D",
    "frog": "1F438", "lion": "1F981", "bear": "1F43B", "rabbit": "1F430",
    "butterfly": "1F98B", "heart": "2764", "dice": "1F3B2", "duck": "1F986",
    # wave 2
    "cow": "1F42E", "horse": "1F434", "pig": "1F437", "monkey": "1F435",
    "elephant": "1F418", "tiger": "1F42F", "penguin": "1F427", "owl": "1F989",
    "turtle": "1F422", "panda": "1F43C",
    "cherry": "1F352", "watermelon": "1F349", "orange": "1F34A", "lemon": "1F34B",
    "corn": "1F33D", "bread": "1F35E", "cookie": "1F36A", "hamburger": "1F354",
    "pencil": "270F", "bicycle": "1F6B2", "plane": "2708", "rocket": "1F680",
    "crown": "1F451", "rainbow": "1F308",
    # wave 3
    "dolphin": "1F42C", "snail": "1F40C", "ladybug": "1F41E", "snake": "1F40D",
    "spider": "1F577", "mouse": "1F42D", "fox": "1F98A", "wolf": "1F43A",
    "koala": "1F428", "chicken": "1F414", "goat": "1F410", "deer": "1F98C",
    "peach": "1F351", "pear": "1F350", "pineapple": "1F34D", "coconut": "1F965",
    "tomato": "1F345", "mushroom": "1F344", "cheese": "1F9C0", "candy": "1F36C",
    "drum": "1F941", "robot": "1F916", "phone": "1F4F1", "bus": "1F68C",
    # wave 4
    "shark": "1F988", "dragon": "1F409", "castle": "1F3F0", "cactus": "1F335",
    "unicorn": "1F984", "whale": "1F40B", "mountain": "1F3D4", "kite": "1FA81",
}

def to_rgb565(im):
    im = im.convert("RGBA").resize((SIZE, SIZE), Image.LANCZOS)
    px = im.load()
    out = bytearray()
    out += struct.pack("<HH", SIZE, SIZE)
    for y in range(SIZE):
        for x in range(SIZE):
            r, g, b, a = px[x, y]
            if a < 128:
                v = KEY
            else:
                v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            out += struct.pack("<H", v)
    return out

def main():
    os.makedirs(OUT, exist_ok=True)
    for name, cp in ICONS.items():
        if os.path.exists(os.path.join(OUT, name + ".565")):
            continue   # already built — rerun only fetches newly added words
        url = BASE + cp + ".png"
        try:
            data = urllib.request.urlopen(url, timeout=20).read()
            im = Image.open(io.BytesIO(data))
            blob = to_rgb565(im)
            path = os.path.join(OUT, name + ".565")
            with open(path, "wb") as f:
                f.write(blob)
            print(f"OK  {name:8s} {cp:6s} -> {os.path.basename(path)} ({len(blob)} bytes)")
        except Exception as e:
            print(f"ERR {name:8s} {cp:6s} : {e}")
    with open(os.path.join(OUT, "CREDITS.txt"), "w") as f:
        f.write("Icons: OpenMoji (https://openmoji.org) - CC BY-SA 4.0\n")

if __name__ == "__main__":
    main()
