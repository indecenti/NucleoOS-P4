#!/bin/bash
# build.sh — build the Puzzles app (Simon Tatham's Portable Puzzle Collection, every puzzle in one
# WASI reactor module with a touch front end, ports/puzzles/nv_puzzles.c) into apps/puzzles/:
#
#   app.wasm   wasm32-wasip1 reactor exporting run (gfx canvas + wasi-libc)
#   app.aot    riscv32 AOT image for the ESP32-P4 (what the device runs)
#   icon.z     launcher/store icon
#
#   bash ports/puzzles/build.sh            (Git Bash on Windows)
#   bash ports/puzzles/build.sh --no-aot   (skip wamrc)
#
# Same toolchain as ports/build.sh: LLVM clang + the wasi-sdk sysroot/builtins, wamrc in WSL,
# Python 3 + Pillow (the text font and the icon are generated). Sources: ports/puzzles/fetch.sh.
# PC test before the board: bash ports/puzzles/test.sh.
set -euo pipefail
here="$(cygpath -m "$(cd "$(dirname "$0")" && pwd)")"
root="$(cygpath -m "$(cd "$here/../.." && pwd)")"
bash "$here/fetch.sh" >/dev/null

CLANG="${CLANG:-/c/Program Files/LLVM/bin/clang.exe}"
WASI="${WASI:-D:/esp/wasi-sdk-34}"
SYSROOT="$WASI/wasi-sysroot-34.0"
BUILTINS="$WASI/libclang_rt-34.0/wasm32-unknown-wasip1/libclang_rt.builtins.a"
WAMRC="${WAMRC:-/root/wamrc-build/wamrc}"
DISTRO="${DISTRO:-Ubuntu-24.04}"
P="$root/ports/_src/sgt-puzzles-20250730.a7c7826"
GEN="$root/ports/_src/puzzles_gen"
OUT="$root/apps/puzzles"

# Puzzles left out of the app: nullgame is upstream's empty template (not a game).
EXCLUDE=(nullgame)

mkdir -p "$GEN" "$OUT"
python "$here/gen_font.py" "$GEN/pz_font.h" >/dev/null
mapfile -t GAMES < <(python "$here/gen_meta.py" "$P" "$GEN" "${EXCLUDE[@]}" | tr -d '\r')
echo "== puzzles: ${#GAMES[@]} games"

# The puzzle-independent core (upstream CMakeLists core_obj + common), minus ps.c/printing.
CORE=(combi divvy draw-poly drawing dsf findloop grid latin laydomino loopgen malloc matching
      midend misc penrose penrose-legacy random sort tdq tree234 version hat spectre list)
srcs=()
for f in "${CORE[@]}"; do srcs+=("$P/$f.c"); done
for g in "${GAMES[@]}"; do srcs+=("$P/$g.c"); done
srcs+=("$here/nv_puzzles.c")

# -Os: keeps the riscv32 AOT image well under the 4 MB cap (-O2: 3.77 MB).
# Reactor with a 1 MB C stack in linear memory (solvers recurse; the frame buffer and game state
# come from malloc, capped by the manifest ram_budget). -DCOMBINED: one module, gamelist[].
"$CLANG" --target=wasm32-wasip1 "--sysroot=$SYSROOT" ${OPT:--Os} -nodefaultlibs \
    -DCOMBINED -DNDEBUG -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS \
    -Wall -Wno-unused-function -Wno-unused-but-set-variable -Wno-sign-compare \
    -I"$P" -I"$GEN" -I"$root/sdk/include" \
    -mexec-model=reactor -Wl,--export=run -Wl,-z,stack-size=1048576 -Wl,--strip-all \
    -o "$OUT/app.wasm" "${srcs[@]}" -lc -lm -lwasi-emulated-signal -lwasi-emulated-process-clocks "$BUILTINS"

n=$(stat -c %s "$OUT/app.wasm")
[ "$n" -le 2097152 ] || { echo "app.wasm: $n bytes > 2 MB device cap" >&2; exit 1; }
echo "  puzzles/app.wasm  $n bytes"

if [ "${1:-}" != "--no-aot" ]; then
    wsl_path() { echo "/mnt/$(echo "${1:0:1}" | tr 'A-Z' 'a-z')${1:2}"; }
    MSYS_NO_PATHCONV=1 wsl.exe -d "$DISTRO" -- "$WAMRC" --target=riscv32 --target-abi=ilp32f \
        --cpu=generic-rv32 --cpu-features=+m,+a,+c,+f --enable-multi-thread \
        -o "$(wsl_path "$OUT/app.aot")" "$(wsl_path "$OUT/app.wasm")" >/dev/null
    n=$(stat -c %s "$OUT/app.aot")
    [ "$n" -le 4194304 ] || { echo "app.aot: $n bytes > 4 MB device cap" >&2; exit 1; }
    echo "  puzzles/app.aot  $n bytes"
fi
python "$here/make_icon.py" "$OUT/icon.z"
echo "done: apps/puzzles/{app.wasm,app.aot,icon.z} (+ manifest.json, hand-written)"
