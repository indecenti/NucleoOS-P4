#!/bin/bash
# build_engines.sh — the eight per-engine modules the game packages run (apps/scummvm-<engine>/
# app.wasm + app.aot), inside WSL. One engine per module keeps the riscv32 AOT image ~8 MB: with
# every engine it was ~16 MB and did not fit next to a game's memory in PSRAM.
#
#   bash ports/scummvm/build_engines.sh [engine ...]      (default: all eight)
#
# Memory per engine (MB, fixed: initial = max linear memory) must equal the game packages'
# ram_budget in games.json ("ram_mb").
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
declare -A MEM=([sky]=10 [queen]=10 [lure]=10 [drascula]=13 [dreamweb]=10 [cge]=10 [cge2]=10 [parallaction]=10)
engines=("$@")
[ ${#engines[@]} -gt 0 ] || engines=(sky queen lure drascula dreamweb cge cge2 parallaction)
export CCACHE_DIR="$HOME/.ccache"
for e in "${engines[@]}"; do
    MEM_MB=${MEM[$e]} bash "$here/build.sh" "$e" "scummvm-$e"
done
# AOT images: wamrc is single-threaded and slow (~20 min each); four at a time.
if [ "${AOT:-1}" = 1 ]; then
    root="$(cd "$here/../.." && pwd)"
    printf '%s\n' "${engines[@]}" | xargs -P 4 -I{} sh -c \
        "/root/wamrc-build/wamrc --target=riscv32 --target-abi=ilp32f --cpu=generic-rv32 --cpu-features=+m,+a,+c,+f --enable-multi-thread -o '$root/apps/scummvm-{}/app.aot' '$root/apps/scummvm-{}/app.wasm' >/dev/null && echo 'aot {} ok'"
    ls -la "$root"/apps/scummvm-*/app.aot
fi
