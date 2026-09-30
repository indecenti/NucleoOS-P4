#!/bin/bash
# build.sh — build the CHIP-8 app (apps/chip8/{app.wasm,app.aot,icon.z}).
#
#   bash ports/chip8/build.sh          (Git Bash on Windows)
#
# 1. fetch.sh downloads the pinned CHIP-8 Community Archive (CC0) into ports/_src.
# 2. gen_games.py turns programs.json + roms/ into ports/_src/chip8_gen/games.h (ROMs as C arrays:
#    the store ships only img/snd/models as separate files).
# 3. clang builds a freestanding wasm32 MVP module like sdk/build_app.ps1, except that linear
#    memory is sized by the linker (the ROMs need ~350 KB) instead of the fixed 64 KB.
# 4. wamrc (WSL, the firmware's WAMR tree) makes the riscv32 AOT image, same flags as
#    sdk/build_app.ps1 -Aot; make_icon.py draws the icon.
# The manifest (apps/chip8/manifest.json) is hand-written. PC test: bash ports/chip8/test/run.sh
set -euo pipefail
here="$(cygpath -m "$(cd "$(dirname "$0")" && pwd)")"
root="$(cygpath -m "$(cd "$here/../.." && pwd)")"
bash "$here/fetch.sh" >/dev/null

CLANG="${CLANG:-/c/Program Files/LLVM/bin/clang.exe}"
WAMRC="${WAMRC:-/root/wamrc-build/wamrc}"
DISTRO="${DISTRO:-Ubuntu-24.04}"
ARCH="$here/../_src/chip8Archive-761e3ffc63f43e6a849e80714122feff2afca208"
GEN="$here/../_src/chip8_gen"
OUT="$root/apps/chip8"
mkdir -p "$GEN" "$OUT"

python "$here/gen_games.py" "$ARCH" "$GEN/games.h"

"$CLANG" --target=wasm32 -mcpu=mvp -O2 -ffreestanding -nostdlib -fvisibility=hidden -Wall -Wextra -Werror \
    -I"$root/sdk/include" -I"$GEN" \
    -Wl,--no-entry -Wl,--export=run -Wl,-z,stack-size=16384 -Wl,--strip-all \
    -o "$OUT/app.wasm" "$here/main.c" "$here/chip8.c" "$root/sdk/src/nucleo_sdk.c"
n=$(stat -c %s "$OUT/app.wasm")
[ "$n" -le 2097152 ] || { echo "app.wasm $n bytes > 2 MB device cap" >&2; exit 1; }
echo "  chip8/app.wasm  $n bytes"

wsl_path() { echo "/mnt/$(echo "${1:0:1}" | tr 'A-Z' 'a-z')${1:2}"; }   # D:/x -> /mnt/d/x
MSYS_NO_PATHCONV=1 wsl.exe -d "$DISTRO" -- "$WAMRC" --target=riscv32 --target-abi=ilp32f --cpu=generic-rv32 \
    --cpu-features=+m,+a,+c,+f --enable-multi-thread --disable-bulk-memory --disable-ref-types \
    -o "$(wsl_path "$OUT/app.aot")" "$(wsl_path "$OUT/app.wasm")" >/dev/null
n=$(stat -c %s "$OUT/app.aot")
[ "$n" -le 4194304 ] || { echo "app.aot $n bytes > 4 MB device cap" >&2; exit 1; }
echo "  chip8/app.aot  $n bytes"

python "$here/make_icon.py" "$OUT/icon.z"
echo "  chip8/icon.z  $(stat -c %s "$OUT/icon.z") bytes"
