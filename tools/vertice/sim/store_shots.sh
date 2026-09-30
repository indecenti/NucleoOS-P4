#!/bin/bash
# store_shots.sh ??? App Store screenshots of the Vertice games, rendered by the PC simulator (WSL).
#
#   bash tools/vertice/sim/store_shots.sh [vxgp|bass ...]      (default: every recipe below)
#
# Each recipe plays the real game in the simulator (scripted pad/touch, see run.sh) and keeps a few
# frames at canvas size (512x300). They land in apps/<id>/shots/<n>.jpg; export_static.py
# publishes them at <store>/shots/<id>/<n>.jpg (outside the package: never installed, not signed)
# and the catalog's "shots" count tells the device and the web page how many there are.
# No device, no status bar: nothing personal can end up in a picture.
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
run() {   # run <id> <frames> <keep frames "a b c"> [env...]
    local id="$1" frames="$2" keep="$3"; shift 3
    local out="/tmp/vxshots/$id"
    rm -rf "$out"
    env APP_CFLAGS= "$@" VX_DUMP="$(echo $keep | tr ' ' ',')" \
        bash "$root/tools/vertice/sim/run.sh" "$root/apps/$id" "$frames" "$out" > /dev/null 2>&1
    python3 - "$out" "$root/apps/$id/shots" $keep <<'EOF'
import os, sys
from PIL import Image
out, dst, keep = sys.argv[1], sys.argv[2], sys.argv[3:]
os.makedirs(dst, exist_ok=True)
for f in os.listdir(dst):
    if f.endswith(".jpg"):
        os.remove(os.path.join(dst, f))
for n, k in enumerate(keep, 1):
    im = Image.open(os.path.join(out, "frame_%05d.ppm" % int(k))).convert("RGB")
    # Baseline 4:2:0 JPEG: what the P4's hardware decoder reads.
    im.save(os.path.join(dst, "%d.jpg" % n), quality=85, optimize=True, progressive=False, subsampling=2)
print(os.path.basename(os.path.dirname(dst)), len(keep), "shots")
EOF
}

sel=("$@")
has() { [ ${#sel[@]} -eq 0 ] && return 0; for s in "${sel[@]}"; do [ "$s" = "$1" ] && return 0; done; return 1; }

# Vertice GP: title, then a race driven by the AI (VX_AUTOPILOT, simulator-only build flag).
has vxgp && run vxgp 600 "25 200 260 480 560" VX_TOUCH="30-31:256,150" APP_CFLAGS=-DVX_AUTOPILOT

# Vertice Bass: title, lake select, lure select, the lake, a fish at the lure.
# A (16) through title -> lake select -> stage card -> lure -> aim, then cast and reel.
has bass && run bass 1000 "545 600 730 790 960" \
    VX_PAD="560-565:16;620-625:16;700-705:16;760-765:16;840-870:16;1000-1003:16"
