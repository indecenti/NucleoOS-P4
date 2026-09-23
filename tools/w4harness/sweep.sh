#!/bin/bash
# sweep.sh — regression test of the WASM-4 layer against the official wasm4.org gallery (~150
# carts, fetched with a sparse clone of github.com/aduros/wasm4; they are other people's work and
# are only run locally, never committed). Each cart runs 10 s of game time with autoplay input in
# its own process. Summary + per-cart lines in $W4_OUT/results.txt, screens in $W4_OUT/shots/.
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
  line=$(timeout 60 "$OUT/w4run" "$c" --frames "$FRAMES" --screen "$OUT/shots/$n.ppm" --quiet)
  rc=$?
  if [ -n "$line" ]; then echo "$line"; elif [ $rc -eq 124 ]; then echo "RESULT $n.wasm SLOW (60 s timeout)"; \
  else echo "RESULT $n.wasm CRASH rc=$rc"; fi
done > "$OUT/results.txt"
echo "== $(wc -l < "$OUT/results.txt") carts"
awk '{print $3}' "$OUT/results.txt" | sort | uniq -c
echo "== not OK:"
grep -v ' OK ' "$OUT/results.txt" | cut -c1-180
echo "== heaviest update() (ms/frame on this PC's interpreter; the P4 interpreter is far slower, use -Aot):"
grep ' OK ' "$OUT/results.txt" | sed -E 's/.*RESULT ([^ ]+) OK.*avg_ms=([0-9.]+).*/\2 \1/' | sort -rn | head -8
# a CRASH is a host bug (the device would reboot): investigate before shipping
! grep -q ' CRASH ' "$OUT/results.txt"
