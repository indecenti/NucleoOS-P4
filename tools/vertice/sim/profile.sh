#!/bin/bash
# profile.sh — gprof flat profile of an app running on the Vertice PC simulator (run inside WSL).
#
#   bash tools/vertice/sim/profile.sh apps/<id> [frames] [top N]
#   VX_TOUCH="5-6:256,150;100-600:470,250" bash tools/vertice/sim/profile.sh apps/vxgp 600 30
#
# x86 timings are not P4 timings (no PSRAM latency, different caches), but the algorithmic hot
# spots — per-triangle setup, transforms, sorting, per-pixel work — rank the same way.
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
app="$(cd "$1" && pwd)"
frames="${2:-600}"
top="${3:-30}"
O=/tmp/vxprof
rm -rf "$O" && mkdir -p "$O"
V="$root/components/vertice"
F=(-O2 -pg -std=c++17 -pthread -w -DVX_SIM_CLOCK -include "$root/tools/vertice/sim/vx_rename.h"
   -I"$V" -I"$V/core" -I"$V/include")
for s in "$V/vertice.cpp" "$V"/core/*.cpp; do
    g++ "${F[@]}" -c "$s" -o "$O/$(basename "${s%.cpp}").o" &
done
wait
gcc -O2 -pg -std=gnu11 -DNV_SIM -I"$root/sdk/include" -c "$root/tools/vertice/sim/nv_sim.c" -o "$O/nv_sim.o"
for s in "$app"/*.c; do
    gcc -O2 -pg -std=gnu11 -DNV_SIM -I"$root/sdk/include" -c "$s" -o "$O/app_$(basename "${s%.c}").o"
done
g++ -pg -pthread -o "$O/vxsim" "$O"/*.o -lm
mkdir -p "$O/out"
(cd "$O" && VX_DUMP=none ./vxsim "$app" "$O/out" "$frames" > /dev/null)
gprof -b -p "$O/vxsim" "$O/gmon.out" | head -n "$((top + 6))"
