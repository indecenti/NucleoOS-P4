#!/bin/bash
# store.sh — turn the wasm4.org gallery into an app-store folder for server/appstore: one
# <id>/{manifest.json, app.wasm, app.aot} per cart that passed the last sweep.sh run in ALL input
# modes (touch, USB keyboard + mouse, USB gamepads). Name, author and description come from the
# cart's page in the gallery archive. Serve it next to the repo apps:
#   bash tools/w4harness/store.sh
#   python server/appstore/appstore_server.py --apps-dir apps --apps-dir <WSL path>/store-apps
# The gallery carts are licensed CC BY-NC-SA 4.0 by their authors (see the archive's LICENSE.txt):
# fine for your own device, keep the author credit, don't sell them, never commit them here.
set -e
OUT="${W4_OUT:-$HOME/w4harness}"
CARTS="$OUT/wasm4-src/site/static/carts"
STORE="${1:-$OUT/store-apps}"
WAMRC="${WAMRC:-/root/wamrc-build/wamrc}"
[ -f "$OUT/results.txt" ] || { echo "run sweep.sh first"; exit 1; }
[ -x "$WAMRC" ] || { echo "wamrc not found at $WAMRC (set WAMRC=)"; exit 1; }
mkdir -p "$STORE"
cp "$CARTS/LICENSE.txt" "$STORE/LICENSE-carts.txt"

ok=0; skipped=0
for c in "$CARTS"/*.wasm; do
  n=$(basename "$c" .wasm)
  if [ "$(grep -cE "RESULT $n\.wasm OK .*\[(touch|kbd|pads)\]$" "$OUT/results.txt")" != 3 ]; then
    skipped=$((skipped + 1)); continue
  fi
  id=$(echo "$n" | tr -c 'A-Za-z0-9_\n-' '-' | cut -c1-31)
  d="$STORE/$id"
  mkdir -p "$d"
  cp "$c" "$d/app.wasm"
  python3 - "$CARTS/$n.md" "$id" "$d/manifest.json" <<'PY'
import json, re, sys
md_path, app_id, out = sys.argv[1:4]
name, author, desc = app_id.replace("-", " ").title(), "", ""
try:
    text = open(md_path, encoding="utf-8").read()
except OSError:
    text = ""
m = re.match(r"^---\n(.*?)\n---\n", text, re.S)
if m:
    for line in m.group(1).splitlines():
        if line.startswith("author:"):
            author = re.sub(r"\s*<[^>]*>", "", line[7:]).strip().strip("\"'")
    text = text[m.end():]
title = re.search(r"^#\s+(.+)$", text, re.M)
if title:
    name = title.group(1).strip()
    body = text[title.end():]
    for para in re.split(r"\n\s*\n", body):
        p = para.strip()
        if not p or p.startswith("#") or p.startswith("!") or p.startswith("|"):
            continue
        p = re.sub(r"!\[[^\]]*\]\([^)]*\)", "", p)           # images
        p = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", p)       # links -> their text
        p = re.sub(r"[*_`]", "", p)
        p = " ".join(p.split())
        if p:
            desc = p
            break
man = {"id": app_id, "name": name[:39], "version": "1.0", "wasm4": True,
       "author": author[:39], "description": desc}
json.dump(man, open(out, "w", encoding="utf-8"), ensure_ascii=False, indent=2)
PY
  "$OUT/w4run" "$d/app.wasm" --prep "$d/prep.wasm" > /dev/null
  if ! "$WAMRC" --target=riscv32 --target-abi=ilp32f --cpu=generic-rv32 --cpu-features=+m,+a,+c,+f \
       --enable-multi-thread -o "$d/app.aot" "$d/prep.wasm" > "$d/wamrc.log" 2>&1; then
    echo "  $id: wamrc failed, wasm only"; rm -f "$d/app.aot"
  fi
  rm -f "$d/prep.wasm" "$d/wamrc.log"
  ok=$((ok + 1))
done
echo "store: $ok carts in $STORE ($skipped skipped: not OK in every sweep mode)"
du -sh "$STORE" | cut -f1
