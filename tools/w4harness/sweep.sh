#!/bin/bash
# sweep.sh — regression test of the WASM-4 layer against the official wasm4.org gallery (~150
# carts, fetched with a sparse clone of github.com/aduros/wasm4; they are other people's work and
# are only run locally, never committed). Each cart runs 10 s of game time in its own process,
# three times: with the touch gamepad (480 px screen); with a USB keyboard plugged in halfway (live
# switch to the 600 px screen) and a USB mouse; with two USB gamepads (players 1 and 2). Every run
# checks the partial redraws against a full render. Summary + per-run lines in
# $W4_OUT/results.txt, screens in $W4_OUT/shots/.
#   bash tools/w4harness/sweep.sh [frames]      (run build.sh first)
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${W4_OUT:-$HOME/w4harness}"
FRAMES="${1:-600}"
CARTS="$OUT/wasm4-src/site/static/carts"
[ -x "$OUT/w4run" ] || { echo "run build.sh first"; exit 1; }
if [ ! -d "$CARTS" ]; then
  git clone -q --depth 1 --filter=blob:none --sparse https://github.com/aduros/wasm4.git "$OUT/wasm4-src"
  git -C "$OUT/wasm4-src" sparse-checkout set site/static/carts
fi
mkdir -p "$OUT/shots"
rm -f "$OUT/shots/"*.ppm "$OUT/results.txt"
for c in "$CARTS"/*.wasm; do
  n=$(basename "$c" .wasm)
  for mode in touch kbd pads; do
    case $mode in
      touch) extra=(--screen "$OUT/shots/$n.ppm") ;;
      kbd)   extra=(--keyboard $((FRAMES / 2)) --mouse) ;;
      pads)  extra=(--pads 2) ;;
    esac
    line=$(timeout 60 "$OUT/w4run" "$c" --frames "$FRAMES" --verify --quiet "${extra[@]}")
    rc=$?
    if [ -n "$line" ]; then echo "$line [$mode]"; elif [ $rc -eq 124 ]; then echo "RESULT $n.wasm SLOW (60 s timeout) [$mode]"; \
    else echo "RESULT $n.wasm CRASH rc=$rc [$mode]"; fi
  done
done > "$OUT/results.txt"
echo "== $(wc -l < "$OUT/results.txt") runs"
awk '{print $3, $NF}' "$OUT/results.txt" | sort | uniq -c
echo "== not OK:"
grep -v ' OK ' "$OUT/results.txt" | cut -c1-180
echo "== heaviest update() (ms/frame on this PC's interpreter; the P4 interpreter is far slower, use -Aot):"
grep ' OK .*\[touch\]' "$OUT/results.txt" | sed -E 's/.*RESULT ([^ ]+) OK.*avg_ms=([0-9.]+).*/\2 \1/' | sort -rn | head -8
# CRASH = host bug (the device would reboot); MISMATCH = partial redraw differs from a full one;
# QUIT = the host asked to quit without Esc being typed
! grep -qE " (CRASH|MISMATCH|QUIT) " "$OUT/results.txt"
