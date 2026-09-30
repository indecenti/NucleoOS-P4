#!/bin/bash
# build.sh — the Game Boy homebrew store apps (apps/gbh-*): each one is the ports/gameboy front-end
# (gb_frontend.c: Peanut-GB + MiniGB APU, touch/gamepad/keyboard, pause menu) with one freely
# licensed ROM from the Homebrew Hub database compiled in. The list, licenses and texts live in
# games.json / GAMES.md; ports/gameboy is reused as is (never edited from here).
#
#   bash ports/gbhomebrew/build.sh                 every game: test + apps
#   bash ports/gbhomebrew/build.sh test [ids...]   PC harness only: ~1500 frames headless per game
#                                                  -> ports/_src/gbhomebrew/test/<id>.png (+ <id>_a.png)
#   bash ports/gbhomebrew/build.sh apps [ids...]   apps/<id>/{app.wasm,app.aot,icon.z,manifest.json,
#                                                  GUIDE.md,GUIDE.en.md,shots/*.jpg} + catalog_entries.json
#
# Needs what ports/gameboy/build.sh needs: LLVM clang (wasm32), wasi-sdk 34, WSL (Ubuntu-24.04) with
# wamrc (/root/wamrc-build/wamrc) and gcc, Python 3 + Pillow. Inputs: ports/gameboy/fetch.sh
# (emulator) + ports/gbhomebrew/fetch.sh (ROMs + database screenshots, pinned).
set -euo pipefail
here="$(cygpath -m "$(cd "$(dirname "$0")" && pwd)")"
root="$(cygpath -m "$(cd "$here/../.." && pwd)")"
bash "$root/ports/gameboy/fetch.sh" >/dev/null
bash "$here/fetch.sh" >/dev/null

CLANG="${CLANG:-/c/Program Files/LLVM/bin/clang.exe}"
WASI="${WASI:-D:/esp/wasi-sdk-34}"
SYSROOT="$WASI/wasi-sysroot-34.0"
BUILTINS="$WASI/libclang_rt-34.0/wasm32-unknown-wasip1/libclang_rt.builtins.a"
WAMRC="${WAMRC:-/root/wamrc-build/wamrc}"
DISTRO="${DISTRO:-Ubuntu-24.04}"
GB="$root/ports/gameboy"            # front-end + harness (reused, read-only)
S="$root/ports/_src/gameboy"        # Peanut-GB, MiniGB APU
H="$root/ports/_src/gbhomebrew"     # ROMs, database entries (fetch.sh)
GEN="$H/gen"
TEST="$H/test"
mkdir -p "$GEN" "$TEST"

wsl_path() { echo "/mnt/$(echo "${1:0:1}" | tr 'A-Z' 'a-z')${1:2}"; }   # D:/x -> /mnt/d/x
wsl() { MSYS_NO_PATHCONV=1 wsl.exe -d "$DISTRO" -- "$@"; }
py() { PYTHONIOENCODING=utf-8 python "$here/gbh.py" "$@" | tr -d '\r'; }

# Same flags as ports/gameboy/build.sh: a WASI reactor exporting run (manifest "entry": "run").
COMMON=(-DMINIGB_APU_AUDIO_FORMAT_S16SYS -DAUDIO_SAMPLE_RATE=48000 -I"$S" -I"$root/sdk/include" -Wno-unused-parameter)
CFLAGS=(--target=wasm32-wasip1 "--sysroot=$SYSROOT" -O2 -nodefaultlibs -mexec-model=reactor
        -Wl,--export=run -Wl,-z,stack-size=65536 -Wl,--strip-all)
SRCS=("$GB/gb_frontend.c" "$S/minigb_apu.c" "$root/sdk/src/nucleo_sdk_wasi.c")

ids() { if [ $# -gt 0 ]; then printf '%s\n' "$@"; else py ids; fi; }

rom_header() {   # id -> $GEN/<id>_rom.h (gb_rom_data[], GB_ROM_SIZE, GB_TITLE)
    py header "$1" "$H/roms/$1.gb" "$GEN/$1_rom.h"
}

aot() {   # riscv32 AOT for the ESP32-P4 (same flags as ports/build.sh and ports/gameboy/build.sh)
    local wasm="$1" out="${1%.wasm}.aot"
    wsl "$WAMRC" --target=riscv32 --target-abi=ilp32f --cpu=generic-rv32 \
        --cpu-features=+m,+a,+c,+f --enable-multi-thread -o "$(wsl_path "$out")" "$(wsl_path "$wasm")" >/dev/null
    local n; n=$(stat -c %s "$out")
    [ "$n" -le 4194304 ] || { echo "$out: $n bytes > 4 MB device cap" >&2; exit 1; }
    echo "  app.aot  $n bytes"
}

# Native harness (gcc in WSL, stubbed nv_*): run the ROM <frames> presents with <script>, dump PNG.
harness() {   # id
    local id="$1" fs="$TEST/fs_$1"
    mkdir -p "$fs"
    wsl gcc -O2 -DNV_SIM -DMINIGB_APU_AUDIO_FORMAT_S16SYS -DAUDIO_SAMPLE_RATE=48000 \
        -DGB_FS_ROOT="\"$(wsl_path "$fs")\"" -include "$(wsl_path "$GEN/${id}_rom.h")" \
        -I"$(wsl_path "$S")" -I"$(wsl_path "$root/sdk/include")" -o "$(wsl_path "$TEST/harness_$id")" \
        "$(wsl_path "$GB/gb_frontend.c")" "$(wsl_path "$S/minigb_apu.c")" "$(wsl_path "$GB/host/harness.c")"
}
run_harness() {   # id frames script out-name
    local id="$1"
    rm -f "$TEST/fs_$id"/*.sav "$TEST/fs_$id"/gb.cfg
    wsl timeout 120 "$(wsl_path "$TEST/harness_$id")" "$2" "$(wsl_path "$TEST/$4.ppm")" "$3" > "$TEST/$4.log" 2>&1 || {
        echo "  HARNESS FAILED (exit $?)"; tail -3 "$TEST/$4.log"; return 1; }
    python -c "from PIL import Image; Image.open('$TEST/$4.ppm').save('$TEST/$4.png')"
}

# Headless check: frame 1500 after the scripted inputs (<id>.png) and an early frame (<id>_a.png).
# gbh.py check then rejects crashes, emulator errors, blank screens and CGB-only ROMs.
build_test() {
    local id
    for id in $(ids "$@"); do
        echo "== $id"
        rom_header "$id"
        harness "$id"
        local script; script="$(py script "$id")"
        run_harness "$id" 1500 "$script" "$id" || continue
        run_harness "$id" "$(py early "$id")" "$script" "${id}_a" || continue
        py check "$id" "$TEST"
    done
}

build_app() {   # id
    local id="$1" dir="$root/apps/$1"
    mkdir -p "$dir"
    echo "== $id"
    rom_header "$id"
    "$CLANG" "${CFLAGS[@]}" "${COMMON[@]}" -include "$GEN/${id}_rom.h" -o "$dir/app.wasm" "${SRCS[@]}" \
        -lc "$BUILTINS"
    local n; n=$(stat -c %s "$dir/app.wasm")
    [ "$n" -le 2097152 ] || { echo "$dir/app.wasm: $n bytes > 2 MB" >&2; exit 1; }
    echo "  app.wasm  $n bytes"
    aot "$dir/app.wasm"
    [ -f "$TEST/$id.ppm" ] || build_test "$id" >/dev/null
    py icon "$id" "$dir/icon.z" "$TEST"
    py shots "$id" "$dir/shots" "$TEST"
    py meta "$id" "$dir"
}

mode="${1:-all}"; [ $# -gt 0 ] && shift
case "$mode" in
    test) build_test "$@" ;;
    apps) for id in $(ids "$@"); do build_app "$id"; done; py catalog "$here/catalog_entries.json" ;;
    all)  build_test; for id in $(ids); do build_app "$id"; done; py catalog "$here/catalog_entries.json" ;;
    *)    echo "usage: build.sh [test|apps|all] [ids...]" >&2; exit 2 ;;
esac
echo "done."
