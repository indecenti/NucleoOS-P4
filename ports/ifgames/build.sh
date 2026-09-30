#!/bin/bash
# build.sh — the interactive-fiction catalog: one Terminal app per freely licensed game,
# apps/if-<slug>/{app.wasm,app.aot,icon.z,manifest.json,GUIDE.md,GUIDE.en.md}.
#
#   bash ports/ifgames/build.sh [slug ...]     (Git Bash on Windows; default: every game)
#
# Each app.wasm = one interpreter + the story file, raw-deflated and embedded with #embed (C23),
# inflated at start by miniz's tinfl (MIT):
#   z-code (.z3/.z5/.z8)   Frotz 2.55, "dumb" (stdio) interface — GPL-2.0-or-later
#   Glulx (.ulx/.gblorb)   Glulxe + CheapGlk — MIT
# built unmodified: -Dmain=nv_upstream_main (our main in nv_if_main.c prints the credits and passes
# the fixed options) and -include nv_if.h (fopen of the story name -> fmemopen of the embedded bytes).
# The catalog (games.json: licence, source, sha256, texts) drives everything: gen.py writes the
# per-game C file, manifest, guides, GAMES.md and catalog_entries.json.
# Same toolchain as ports/build.sh (LLVM clang, wasi-sdk 34, wamrc in WSL, Python 3 + Pillow).
# Test on the PC: bash ports/ifgames/test.sh
set -euo pipefail
here="$(cygpath -m "$(cd "$(dirname "$0")" && pwd)")"
ports="$(cygpath -m "$(cd "$here/.." && pwd)")"
root="$(cygpath -m "$(cd "$ports/.." && pwd)")"
bash "$here/fetch.sh" >/dev/null

CLANG="${CLANG:-/c/Program Files/LLVM/bin/clang.exe}"
AR="${AR:-/c/Program Files/LLVM/bin/llvm-ar.exe}"
WASI="${WASI:-D:/esp/wasi-sdk-34}"
SYSROOT="$WASI/wasi-sysroot-34.0"
BUILTINS="$WASI/libclang_rt-34.0/wasm32-unknown-wasip1/libclang_rt.builtins.a"
WAMRC="${WAMRC:-/root/wamrc-build/wamrc}"
DISTRO="${DISTRO:-Ubuntu-24.04}"
S="$ports/_src"
GEN="$S/ifgames_gen"
FROTZ="$S/frotz-2.55/src"
GLULXE="$S/glulxe-56ab8743bab565de307bd892c555d8d8897ed517"
CHEAPGLK="$S/cheapglk-14d8aaf6e4150669762bd4646a5368e75c1eeee6"

CFLAGS=(--target=wasm32-wasip1 "--sysroot=$SYSROOT" -O2 -nodefaultlibs
        -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS -Wno-deprecated-declarations
        -Wno-c23-extensions -Dmain=nv_upstream_main -include "$here/nv_if.h")
LDFLAGS=(-Wl,-z,stack-size=262144 -Wl,--strip-all)
LIBS=(-lc -lm -lwasi-emulated-signal -lwasi-emulated-process-clocks)

wsl_path() { echo "/mnt/$(echo "${1:0:1}" | tr 'A-Z' 'a-z')${1:2}"; }   # D:/x -> /mnt/d/x

aot() {   # riscv32 AOT for the ESP32-P4 (same flags as ports/build.sh)
    local wasm="$1" out="${1%.wasm}.aot"
    MSYS_NO_PATHCONV=1 wsl.exe -d "$DISTRO" -- "$WAMRC" --target=riscv32 --target-abi=ilp32f --cpu=generic-rv32 \
        --cpu-features=+m,+a,+c,+f --enable-multi-thread -o "$(wsl_path "$out")" "$(wsl_path "$wasm")" \
        >/dev/null
    local n; n=$(stat -c %s "$out")
    [ "$n" -le 4194304 ] || { echo "$out: $n bytes > 4 MB device cap" >&2; exit 1; }
    echo "  app.aot  $n bytes"
}

# The two interpreters as static libraries, compiled once (objects don't depend on the game).
lib_frotz() {
    local out="$GEN/libfrotz.a" o="$GEN/obj-frotz"
    [ -f "$out" ] && return
    rm -rf "$o"; mkdir -p "$o"
    for f in "$FROTZ"/common/*.c "$FROTZ"/dumb/*.c "$FROTZ"/blorb/blorblib.c; do
        "$CLANG" "${CFLAGS[@]}" -I"$here/frotz" -I"$FROTZ/common" -c -o "$o/$(basename "$(dirname "$f")")_$(basename "${f%.c}").o" "$f"
    done
    "$AR" rcs "$out" "$o"/*.o
}
lib_glulxe() {
    local out="$GEN/libglulxe.a" o="$GEN/obj-glulxe" f
    [ -f "$out" ] && return
    rm -rf "$o"; mkdir -p "$o"
    local srcs=("$CHEAPGLK/gi_blorb.c" "$CHEAPGLK/gi_dispa.c" "$CHEAPGLK/gi_debug.c" "$CHEAPGLK/main.c")
    # cgunigen.c is #included by cgunicod.c (Unicode case tables), never compiled on its own.
    for f in "$CHEAPGLK"/cg*.c; do [ "$(basename "$f")" = cgunigen.c ] || srcs+=("$f"); done
    for f in accel exec files float funcs gestalt glkop heap main operand osdepend profile search \
             serial string unixstrt unixautosave vm debugger; do srcs+=("$GLULXE/$f.c"); done
    for f in "${srcs[@]}"; do
        local tag=g; case "$f" in "$CHEAPGLK"/*) tag=c;; esac
        "$CLANG" "${CFLAGS[@]}" -DOS_UNIX -I"$CHEAPGLK" -I"$GLULXE" \
            -c -o "$o/${tag}_$(basename "${f%.c}").o" "$f"
    done
    "$AR" rcs "$out" "$o"/*.o
}

mkdir -p "$GEN"
lib_frotz
lib_glulxe
# gen.py: per-game C file + story extraction, manifests, guides, GAMES.md, catalog_entries.json.
# Prints "slug interpreter label bg fg" for each game to build.
mapfile -t games < <(python "$here/gen.py" "$@")
for line in "${games[@]}"; do
    read -r slug interp label bg fg <<<"$line"
    id="if-$slug"; dir="$root/apps/$id"
    echo "== $id ($interp)"
    "$CLANG" "${CFLAGS[@]}" -I"$here" -I"$here/miniz" -I"$S/miniz-3.1.2" "${LDFLAGS[@]}" -o "$dir/app.wasm" "$GEN/$slug.c" \
        "$GEN/lib$interp.a" "${LIBS[@]}" "$BUILTINS"
    n=$(stat -c %s "$dir/app.wasm")
    [ "$n" -le 2097152 ] || { echo "$id: app.wasm $n bytes > 2 MB device cap" >&2; exit 1; }
    echo "  app.wasm $n bytes"
    aot "$dir/app.wasm"
    python "$ports/make_icon.py" "$dir/icon.z" "${label//_/ }" "$bg" "$fg"
done
echo "done: ${#games[@]} games. Test: bash ports/ifgames/test.sh"
