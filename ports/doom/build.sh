#!/bin/bash
# build.sh — the Doom engine app (apps/doom): doomgeneric + Chocolate Doom OPL music, WASI reactor.
#
#   bash ports/doom/build.sh            wasm + AOT into apps/doom, then the PC harness smoke test
#   bash ports/doom/build.sh harness    only the PC harness (WSL gcc): boots Freedoom headless,
#                                       plays the demos, writes ports/_src/doom/test/*.png + timings
#
# Needs what ports/build.sh needs: LLVM clang (wasm32), wasi-sdk 34 sysroot + builtins, WSL
# (Ubuntu-24.04) with wamrc (/root/wamrc-build/wamrc) and gcc for the harness, Python 3 + Pillow.
# Sources come pinned from ports/doom/fetch.sh and are patched by patch_sources.py.
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
S="$root/ports/_src/doom"
B="$S/build"
T="$S/test"
OUT="$root/apps/doom"

wsl_path() { echo "/mnt/$(echo "${1:0:1}" | tr 'A-Z' 'a-z')${1:2}"; }   # D:/x -> /mnt/d/x
wsl() { MSYS_NO_PATHCONV=1 wsl.exe -d "$DISTRO" -- "$@"; }

rm -rf "$B"
python "$here/patch_sources.py" "$S" "$B" >/dev/null

# Engine sources: everything doomgeneric keeps for a generic port, minus unused back ends.
ENGINE=()
for f in "$B"/*.c; do
    case "$(basename "$f")" in
        w_file_posix.c) ;;
        *) ENGINE+=("$f") ;;
    esac
done
FRONT=("$here/nv_doom.c" "$here/nv_sound.c" "$here/opl_nv.c" "$here/nv_compat.c")
DEFS=(-DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -DCMAP256 -DFEATURE_SOUND -DNDEBUG
      -DUSE_EMU8950_OPL=1 -DEMU8950_NO_RATECONV=1 -DEMU8950_NO_TIMER=1 -DEMU8950_NO_TEST_FLAG=1
      # rp2040-doom's fast OPL set: block renderer per slot, silent slots skipped, no rhythm mode
      -DEMU8950_NO_WAVE_TABLE_MAP=1 -DEMU8950_NO_TLL=1 -DEMU8950_NO_FLOAT=1 -DEMU8950_SIMPLER_NOISE=1
      -DEMU8950_SHORT_NOISE_UPDATE_CHECK=1 -DEMU8950_LINEAR=1 -DEMU8950_LINEAR_SKIP=1
      -DEMU8950_NO_PERCUSSION_MODE=1 ${EXTRA_DEFS:-})
QUIET=(-fms-extensions -Wno-microsoft-anon-tag -Wno-unused-parameter -Wno-unused-variable
       -Wno-unused-but-set-variable -Wno-sign-compare -Wno-parentheses -Wno-dangling-else
       -Wno-pointer-sign -Wno-format -Wno-incompatible-pointer-types
-Wno-missing-field-initializers -Wno-deprecated-non-prototype
       -Wno-absolute-value -Wno-shift-negative-value -Wno-string-plus-int -Wno-tautological-compare
       -Wno-constant-conversion -Wno-return-type -Wno-unused-function -Wno-switch
       -Wno-self-assign -Wno-unknown-warning-option -Wno-single-bit-bitfield-constant-conversion
       -Wno-empty-body -Wno-invalid-source-encoding -Wno-address-of-packed-member)

build_wasm() {
    mkdir -p "$OUT"
    "$CLANG" --target=wasm32-wasip1 "--sysroot=$SYSROOT" -O2 -nodefaultlibs -mexec-model=reactor \
        -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS \
        "${DEFS[@]}" "${QUIET[@]}" -I"$B" -I"$root/sdk/include" -I"$here" \
        -Wl,--export=run -Wl,-z,stack-size=262144 -Wl,--error-limit=0 \
        -Wl,--initial-memory=9437184 -Wl,--max-memory=9437184 -Wl,--strip-all \
        -o "$OUT/app.wasm" "${FRONT[@]}" "${ENGINE[@]}" "$root/sdk/src/nucleo_sdk_wasi.c" \
        -lc -lwasi-emulated-signal -lwasi-emulated-process-clocks "$BUILTINS"
    local n; n=$(stat -c %s "$OUT/app.wasm")
    [ "$n" -le 2097152 ] || { echo "app.wasm: $n bytes > 2 MB device cap" >&2; exit 1; }
    echo "  doom/app.wasm  $n bytes"
    # WAMR's riscv32 AOT loader has no float -> 64-bit int helpers: such code gets app.aot refused
    if "/c/Program Files/LLVM/bin/llvm-objdump.exe" -d "$OUT/app.wasm" | grep -q "i64.trunc_\(sat_\)\?f32"; then
        echo "app.wasm converts f32 -> i64 somewhere: app.aot would be rejected on the device" >&2; exit 1
    fi
    wsl "$WAMRC" --target=riscv32 --target-abi=ilp32f --cpu=generic-rv32 \
        --cpu-features=+m,+a,+c,+f --enable-multi-thread \
        -o "$(wsl_path "$OUT/app.aot")" "$(wsl_path "$OUT/app.wasm")" >/dev/null
    n=$(stat -c %s "$OUT/app.aot")
    [ "$n" -le 4194304 ] || { echo "app.aot: $n bytes > 4 MB device cap" >&2; exit 1; }
    echo "  doom/app.aot   $n bytes"
}

build_harness() {
    mkdir -p "$T"
    local srcs=()
    for f in "${FRONT[@]}" "${ENGINE[@]}" "$here/host/harness.c"; do srcs+=("$(wsl_path "$f")"); done
    wsl gcc ${HARNESS_CFLAGS:--O2 -g} -DNV_SIM "${DEFS[@]}" -fms-extensions -w -Werror=implicit-function-declaration -Werror=int-conversion \
        -I"$(wsl_path "$B")" -I"$(wsl_path "$root/sdk/include")" -I"$(wsl_path "$here")" \
        -o "$(wsl_path "$T/harness")" "${srcs[@]}" -lm
    echo "  harness built"
}

run_harness() {   # iwad-name frames
    rm -rf "$T/fs"; mkdir -p "$T/fs/appdata"; [ -d "$T/fs/appdata" ] || { echo "no fs/appdata" >&2; exit 1; }
    mkdir -p "$T/fs/doom"; cp "$S/freedoom/$1" "$T/fs/doom/"
    (cd "$T" && wsl ./harness "$(wsl_path "$T/fs")" "$1" "${2:-2000}")
    for p in "$T"/fs/*.ppm; do python -c "from PIL import Image; import sys; Image.open(sys.argv[1]).save(sys.argv[2])" "$p" "$T/$(basename "${p%.ppm}").png"; done
}

# Every store game: its descriptor as the cached copy, its files linked in, a warp to two maps.
# Prints any I_Error (visplane overflow, missing lump...) and the CPU cost per frame.
run_games() {
    python "$here/games.py" data "$S/site" >/dev/null
    for g in "$S"/site/*.game; do
        local id; id=$(basename "$g" .game)
        if [ -n "${GAMES:-}" ] && ! echo " $GAMES " | grep -q " $id "; then continue; fi
        local maps="1 20"
        if grep -q "iwad freedoom1.wad" "$g"; then maps="1_1 3_5 4_1"; fi
        if grep -q "iwad doom1.wad" "$g"; then maps="1_1 1_8"; fi
        for m in $maps; do
            rm -rf "$T/fs"; mkdir -p "$T/fs/appdata" "$T/fs/doom"
            cp "$g" "$T/fs/appdata/game.txt"
            for f in $(awk '$1=="file"{print $2}' "$g"); do
                wsl ln -s "$(wsl_path "$S/site/$f")" "$(wsl_path "$T/fs/doom/$f")"
            done
            local out
            out=$(cd "$T" && MSYS_NO_PATHCONV=1 wsl.exe -d "$DISTRO" -- env NUCLEO_APP="$id"                   DOOM_EXTRA="-warp ${m/_/ } -skill 3" ./harness "$(wsl_path "$T/fs")" x 700 2>&1) || true
            echo "$id map $m: $(echo "$out" | grep -E "Error|error|fatal|presents" | tail -1 || true) [missing tex: $(echo "$out" | grep -c "not found, using" || true)]"
            cp "$T/fs/frame0400.ppm" "$T/$id-$m.ppm" 2>/dev/null || true
        done
    done
}

case "${1:-all}" in
    games) build_harness; run_games ;;
    harness) build_harness; run_harness freedoom1.wad 2500 ;;
    wasm) build_wasm ;;
    *) build_wasm; build_harness; run_harness freedoom1.wad 2500 ;;
esac
echo "done."
