#!/bin/bash
# run.sh — PC test of the chip8 app: builds test/harness.c natively in WSL (gcc, -DNV_SIM), runs
# every bundled game for 300 frames and turns the screenshots into PNGs in ports/_out/chip8
# (menu.png, full_*.png and sheet.png, a contact sheet of every game's display).
#
#   bash ports/chip8/test/run.sh          (Git Bash; run ports/chip8/build.sh first for games.h)
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="$root/ports/_out/chip8"
mkdir -p "$out"
rm -f "$out"/*.ppm
w() { echo "/mnt/$(cygpath -m "$1" | sed -E 's|^([A-Za-z]):|\L\1|')"; }
export MSYS_NO_PATHCONV=1
wsl.exe -d "${DISTRO:-Ubuntu-24.04}" -- bash -c "set -e; cd $(w "$root") && \
    gcc -O2 -Wall -Wextra -DNV_SIM -Isdk/include -Iports/chip8 -Iports/_src/chip8_gen \
        ports/chip8/test/harness.c ports/chip8/chip8.c -o /tmp/c8harness && /tmp/c8harness $(w "$out")"
python - "$(cygpath -m "$out")" <<'PY'
import glob, os, sys
from PIL import Image
out = sys.argv[1]
shots = sorted(p for p in glob.glob(os.path.join(out, "*.ppm")))
tiles = []
for p in shots:
    name = os.path.basename(p)[:-4]
    im = Image.open(p)
    if name == "menu" or name.startswith("full_"):
        im.save(os.path.join(out, name + ".png"))
    else:
        tiles.append((name, im.resize((256, 128))))
    os.remove(p)
cols = 8
rows = (len(tiles) + cols - 1) // cols
sheet = Image.new("RGB", (cols * 260, rows * 132), (20, 22, 28))
for i, (name, im) in enumerate(tiles):
    sheet.paste(im, ((i % cols) * 260 + 2, (i // cols) * 132 + 2))
sheet.save(os.path.join(out, "sheet.png"))
print(f"screenshots: {out}/menu.png, full_*.png, sheet.png ({len(tiles)} games)")
PY
