#!/bin/bash
# run.sh — build an app against the Vertice PC simulator and render it to PNGs (run inside WSL).
#
#   bash tools/vertice/sim/run.sh apps/<id> [frames] [out dir]
#   VX_TOUCH="10-60:400,250" VX_DUMP="0,30,60" bash tools/vertice/sim/run.sh apps/corsa 120
#
# The engine objects are cached in /tmp/vxsim-obj (rebuilt when a source is newer). Output: PPM +
# PNG frames at panel size (1024x600, the canvas scaled exactly like the OS PPA fit) in <out dir>
# (default /tmp/vxsim-out/<id>), plus the simulator log.
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
app="$(cd "$1" && pwd)"; id="$(basename "$app")"
frames="${2:-120}"
out="${3:-/tmp/vxsim-out/$id}"
obj=/tmp/vxsim-obj
mkdir -p "$obj" "$out"
rm -f "$out"/frame_*.ppm "$out"/frame_*.png

V="$root/components/vertice"
CXXFLAGS=(-O2 -std=c++17 -pthread -w -DVX_SIM_CLOCK -include "$root/tools/vertice/sim/vx_rename.h"
          -I"$V" -I"$V/core" -I"$V/include")
objs=()
for src in "$V/vertice.cpp" "$V"/core/*.cpp; do
    o="$obj/$(basename "${src%.cpp}").o"
    if [ ! -f "$o" ] || [ "$src" -nt "$o" ] || [ "$V/JetConfig.hpp" -nt "$o" ] || [ "$V/include/vertice.h" -nt "$o" ]; then
        g++ "${CXXFLAGS[@]}" -c "$src" -o "$o" &
    fi
    objs+=("$o")
done
wait
gcc -O1 -g -std=gnu11 -DNV_SIM -Wall -Wextra -Wno-unused-parameter -I"$root/sdk/include" \
    -c "$root/tools/vertice/sim/nv_sim.c" -o "$obj/nv_sim.o"
appobjs=()
for src in "$app"/*.c; do
    o="$obj/app_${id}_$(basename "${src%.c}").o"
    gcc -O1 -g -std=gnu11 -DNV_SIM -Wall -Wextra -I"$root/sdk/include" -c "$src" -o "$o"
    appobjs+=("$o")
done
g++ -pthread -o "$obj/vxsim_$id" "$obj/nv_sim.o" "${appobjs[@]}" "${objs[@]}" -lm
"$obj/vxsim_$id" "$app" "$out" "$frames" | tee "$out/sim.log"
python3 - "$out" <<'EOF'
import glob, os, sys
from PIL import Image
out = sys.argv[1]
for p in sorted(glob.glob(os.path.join(out, 'frame_*.ppm'))):
    im = Image.open(p)
    w, h = im.size
    k = min(1024 * 16 // w, 600 * 16 // h)            # the OS fit: exact k/16 factor
    tw, th = w * k // 16, h * k // 16
    panel = Image.new('RGB', (1024, 600))
    panel.paste(im.resize((tw, th), Image.BILINEAR), ((1024 - tw) // 2, (600 - th) // 2))
    panel.save(p[:-4] + '.png')
print('png frames:', len(glob.glob(os.path.join(out, 'frame_*.png'))), '->', out)
EOF
