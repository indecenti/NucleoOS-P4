#!/bin/bash
# store.sh — turn the wasm4.org gallery into an app-store folder for server/appstore: one
# <id>/{manifest.json, app.wasm, app.aot, icon.z} per cart that never failed in the last sweep.sh
# run (any mode: touch, USB keyboard + mouse, USB gamepads; SLOW is fine, it is the PC interpreter
# and the board runs app.aot). Name, author, description and icon come from the cart's page in the
# gallery archive (store_meta.py), plus the license and a link to the cart on wasm4.org.
#   bash tools/w4harness/store.sh [store dir]            (default ~/w4harness/store-apps)
#   bash tools/w4harness/store.sh --descriptions > en.json   English descriptions to translate
# Translations: $OUT/translations.json = {"<id>": {"en": "...", "it": "..."}}, merged into each
# manifest. Editorial choices (featured, non-game categories, exclusions): store_curation.json.
# Serve a copy on a Windows disk (reading \\wsl.localhost is ~100x slower: the device gives up):
#   python server/appstore/appstore_server.py --apps-dir apps --apps-dir D:\w4store
# The gallery carts are licensed CC BY-NC-SA 4.0 by their authors (see the archive's LICENSE.txt):
# credit the author, same license, no commercial use; never commit them here.
set -e
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${W4_OUT:-$HOME/w4harness}"
CARTS="$OUT/wasm4-src/site/static/carts"
WAMRC="${WAMRC:-/root/wamrc-build/wamrc}"
[ -f "$OUT/results.txt" ] || { echo "run sweep.sh first"; exit 1; }

# Carts with at least one run and no failed one.
passing() {
  awk '$1 == "RESULT" { n = $2; sub(/\.wasm$/, "", n); runs[n]++;
                        if ($3 != "OK" && $3 != "SLOW") bad[n] = 1 }
       END { for (n in runs) if (!(n in bad)) print n }' "$OUT/results.txt" | sort
}

if [ "$1" = "--descriptions" ]; then
  tmp=$(mktemp -d)
  echo "{"
  first=1
  for n in $(passing); do
    id=$(echo "$n" | tr -c 'A-Za-z0-9_\n-' '-' | cut -c1-31)
    python3 "$HERE/store_meta.py" "$CARTS/$n.md" "$CARTS/$n.png" "$id" "$tmp" 2>/dev/null
    d=$(python3 -c 'import json,sys; print(json.dumps(json.load(open(sys.argv[1]))["description"]))' "$tmp/manifest.json")
    [ $first = 1 ] && first=0 || echo ","
    printf '  "%s": %s' "$id" "$d"
  done
  echo; echo "}"
  rm -rf "$tmp"
  exit 0
fi

STORE="${1:-$OUT/store-apps}"
[ -x "$WAMRC" ] || { echo "wamrc not found at $WAMRC (set WAMRC=)"; exit 1; }
mkdir -p "$STORE"
cp "$CARTS/LICENSE.txt" "$STORE/LICENSE-carts.txt"
TR=""
[ -f "$OUT/translations.json" ] && TR="$OUT/translations.json"
CUR="$HERE/store_curation.json"
EXCLUDE=$(python3 -c 'import json,sys; print(" ".join(json.load(open(sys.argv[1])).get("exclude", [])))' "$CUR")

ok=0
total=$(ls "$CARTS"/*.wasm | wc -l)
for n in $(passing); do
  case " $EXCLUDE " in *" $n "*) continue;; esac
  id=$(echo "$n" | tr -c 'A-Za-z0-9_\n-' '-' | cut -c1-31)
  d="$STORE/$id"
  mkdir -p "$d"
  cp "$CARTS/$n.wasm" "$d/app.wasm"
  rm -f "$d/icon.argb"
  python3 "$HERE/store_meta.py" "$CARTS/$n.md" "$CARTS/$n.png" "$id" "$d" "$TR" "$CUR"
  "$OUT/w4run" "$d/app.wasm" --prep "$d/prep.wasm" > /dev/null
  if ! "$WAMRC" --target=riscv32 --target-abi=ilp32f --cpu=generic-rv32 --cpu-features=+m,+a,+c,+f \
       --enable-multi-thread -o "$d/app.aot" "$d/prep.wasm" > "$d/wamrc.log" 2>&1; then
    echo "  $id: wamrc failed, wasm only"; rm -f "$d/app.aot"
  fi
  rm -f "$d/prep.wasm" "$d/wamrc.log"
  ok=$((ok + 1))
done
skipped=$((total - ok))
echo "store: $ok carts in $STORE ($skipped skipped: failed in the sweep)"
du -sh "$STORE" | cut -f1
