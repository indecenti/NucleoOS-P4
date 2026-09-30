#!/bin/bash
# build.sh — the Game Boy apps: Peanut-GB + MiniGB APU behind one front-end (gb_frontend.c).
#
#   gbtobu    Tobu Tobu Girl      (ROM compiled in)      apps/gbtobu/{app.wasm,app.aot,icon.z}
#   gb2048    2048                (ROM compiled in)      apps/gb2048/...
#   gblibbet  Libbet              (ROM compiled in)      apps/gblibbet/...
#   gameboy   generic player: picks .gb files from /sdcard/home/roms
#   test      PC harness only: every game ~600 frames headless -> ports/_src/gameboy/test/*.png
#   wamr      the built app.wasm files under WAMR on the PC (interpreter + x86_64 AOT), same stubs
#
#   bash ports/gameboy/build.sh [gbtobu gb2048 gblibbet gameboy test ...]   (Git Bash; default: all)
#
# Needs what ports/build.sh needs: LLVM clang (wasm32), wasi-sdk 34 sysroot + builtins, WSL
# (Ubuntu-24.04) with wamrc (/root/wamrc-build/wamrc) and gcc for the harness, Python 3 + Pillow.
# Sources and ROMs come pinned from ports/gameboy/fetch.sh. The manifests are hand-written.
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
S="$root/ports/_src/gameboy"
GEN="$S/gen"
TEST="$S/test"
mkdir -p "$GEN" "$TEST"

wsl_path() { echo "/mnt/$(echo "${1:0:1}" | tr 'A-Z' 'a-z')${1:2}"; }   # D:/x -> /mnt/d/x
wsl() { MSYS_NO_PATHCONV=1 wsl.exe -d "$DISTRO" -- "$@"; }

# Shared: a WASI reactor exporting run (manifest "entry": "run"), like sdk/build_app.ps1 -Wasi.
COMMON=(-DMINIGB_APU_AUDIO_FORMAT_S16SYS -DAUDIO_SAMPLE_RATE=48000 -I"$S" -I"$root/sdk/include" -Wno-unused-parameter)
CFLAGS=(--target=wasm32-wasip1 "--sysroot=$SYSROOT" -O2 -nodefaultlibs -mexec-model=reactor
        -Wl,--export=run -Wl,-z,stack-size=65536 -Wl,--strip-all)
SRCS=("$here/gb_frontend.c" "$S/minigb_apu.c" "$root/sdk/src/nucleo_sdk_wasi.c")

# ROM -> C header: gb_rom_data[], GB_ROM_SIZE, GB_TITLE
rom_header() {   # rom out title
    python - "$1" "$2" "$3" <<'PY'
import sys
rom, out, title = sys.argv[1], sys.argv[2], sys.argv[3]
b = open(rom, "rb").read()
with open(out, "w") as f:
    f.write("#include <stdint.h>\n")
    f.write(f'#define GB_TITLE "{title}"\n#define GB_ROM_SIZE {len(b)}u\n')
    f.write(f"static const uint8_t gb_rom_data[{len(b)}] = {{")
    for i in range(0, len(b), 32):
        f.write(",".join(str(x) for x in b[i:i + 32]) + ",\n")
    f.write("};\n")
PY
}

aot() {   # riscv32 AOT for the ESP32-P4 (same flags as ports/build.sh)
    local wasm="$1" out="${1%.wasm}.aot"
    wsl "$WAMRC" --target=riscv32 --target-abi=ilp32f --cpu=generic-rv32 \
        --cpu-features=+m,+a,+c,+f --enable-multi-thread -o "$(wsl_path "$out")" "$(wsl_path "$wasm")" >/dev/null
    local n; n=$(stat -c %s "$out")
    [ "$n" -le 4194304 ] || { echo "$out: $n bytes > 4 MB device cap" >&2; exit 1; }
    echo "  $(basename "$(dirname "$out")")/app.aot  $n bytes"
}

# PC harness (gcc in WSL): same front-end, stubbed nv_*. $1 = id, rest = extra defines.
harness() {
    local id="$1"; shift
    local fs="$TEST/fs_$id"
    mkdir -p "$fs"
    wsl gcc -O2 -DNV_SIM -DMINIGB_APU_AUDIO_FORMAT_S16SYS -DAUDIO_SAMPLE_RATE=48000 -DGB_FS_ROOT="\"$(wsl_path "$fs")\"" "$@" \
        -I"$(wsl_path "$S")" -I"$(wsl_path "$root/sdk/include")" -o "$(wsl_path "$TEST/harness_$id")" \
        "$(wsl_path "$here/gb_frontend.c")" "$(wsl_path "$S/minigb_apu.c")" "$(wsl_path "$here/host/harness.c")"
}
run_harness() {   # id frames script -> $TEST/<id>.ppm/.png
    local id="$1"
    wsl "$(wsl_path "$TEST/harness_$id")" "$2" "$(wsl_path "$TEST/$id.ppm")" "${3:-}"
    python -c "from PIL import Image; Image.open('$TEST/$id.ppm').save('$TEST/$id.png')"
}

# id rom title icon-bg frames script: the harness frame after <frames> becomes the icon picture
build_game() {
    local id="$1" rom="$2" title="$3" bg="$4" frames="$5" script="${6:-}"
    local dir="$root/apps/$id"
    rom_header "$S/$rom" "$GEN/${id}_rom.h" "$title"
    "$CLANG" "${CFLAGS[@]}" "${COMMON[@]}" -include "$GEN/${id}_rom.h" -o "$dir/app.wasm" "${SRCS[@]}" \
        -lc "$BUILTINS"
    echo "  $id/app.wasm  $(stat -c %s "$dir/app.wasm") bytes"
    aot "$dir/app.wasm"
    harness "$id" -include "$(wsl_path "$GEN/${id}_rom.h")"
    rm -f "$TEST/fs_$id"/*.sav "$TEST/fs_$id"/gb.cfg
    run_harness "$id" "$frames" "$script" | tail -2
    python "$here/make_gb_icon.py" "$dir/icon.z" "$bg" "$TEST/$id.ppm"
}

build_gbtobu()   { build_game gbtobu tobu.gb "TOBU TOBU GIRL" "#f4a6c0" 1500; }
build_gb2048()   { build_game gb2048 2048.gb "2048" "#edc22e" 600 "120-126:start,300-305:left,330-335:up"; }
build_gblibbet() { build_game gblibbet libbet.gb "LIBBET" "#5b3f8c" 1000 "200-206:start,450-456:start"; }

build_gameboy() {
    local dir="$root/apps/gameboy"
    "$CLANG" "${CFLAGS[@]}" "${COMMON[@]}" -DGB_GENERIC -o "$dir/app.wasm" "${SRCS[@]}" -lc "$BUILTINS"
    echo "  gameboy/app.wasm  $(stat -c %s "$dir/app.wasm") bytes"
    aot "$dir/app.wasm"
    python "$here/make_gb_icon.py" "$dir/icon.z" "#8b8f98" ""
}

# Headless boot test of every game + the generic picker (ROMs copied into its fake /roms).
build_test() {
    for g in "gbtobu tobu.gb TOBU:200-206:start" "gb2048 2048.gb 2048:120-126:start,300-305:left,330-335:up" \
             "gblibbet libbet.gb LIBBET:200-206:start,400-460:right"; do
        set -- $g
        local id="$1" rom="$2" rest="$3" title="${3%%:*}" script="${3#*:}"
        rom_header "$S/$rom" "$GEN/${id}_rom.h" "$title"
        harness "$id" -include "$(wsl_path "$GEN/${id}_rom.h")"
        rm -f "$TEST/fs_$id"/*.sav "$TEST/fs_$id"/gb.cfg
        echo "== $id"
        run_harness "$id" 600 "$script" | grep -v '^\[log2\] save' | tail -4
    done
    harness gameboy -DGB_GENERIC
    mkdir -p "$TEST/fs_gameboy/roms"
    cp "$S/tobu.gb" "$S/2048.gb" "$S/libbet.gb" "$TEST/fs_gameboy/roms/"
    echo "== gameboy (picker, then tap the 2nd game, zoom 3x via the menu)"
    rm -f "$TEST/fs_gameboy/gb.cfg"
    # 10: tap row 2 of the picker (libbet); 400: MENU; 420: ZOOM (-> 3x); 450: RESUME; 600: start
    run_harness gameboy 700 "10-12:t=300/185,400-402:t=96/56,420-422:t=512/330,450-452:t=512/200,600-606:start" | tail -3
}

# The real app.wasm (WAMR fast-interp) and an x86_64 AOT of it under the firmware's WAMR feature set
# (ports/host/build.sh's libiwasm.a), nv imports stubbed like the harness. Run after building the apps.
build_wamr() {
    local lib=/root/nvhost-lib2 wamr host
    wamr="$(wsl_path "$root/reference/wasm-micro-runtime")"
    host="$(wsl_path "$TEST/gbhost")"
    wsl test -f "$lib/libiwasm.a" || wsl bash "$(wsl_path "$root/ports/host/build.sh")"
    for id in gbtobu gb2048 gblibbet gameboy; do
        local fs="$TEST/wamr_$id" script=""
        rm -rf "$fs"; mkdir -p "$fs"
        if [ "$id" = gameboy ]; then
            mkdir -p "$fs/roms"; cp "$S/2048.gb" "$S/libbet.gb" "$S/tobu.gb" "$fs/roms/"
            script="10-12:t=300/135"                        # picker: first game (2048)
        fi
        [ "$id" = gb2048 ] && script="120-126:start,300-305:left,330-335:up"
        wsl gcc -O2 -DNV_SIM -DGB_FS_ROOT="\"$(wsl_path "$fs")\"" -I"$(wsl_path "$root/sdk/include")" \
            -I"$wamr/core/iwasm/include" -I"$(wsl_path "$here/host")" -o "$host" \
            "$(wsl_path "$here/host/gbhost.c")" "$lib/libiwasm.a" -lm -lpthread -ldl
        echo "== $id interp"
        wsl "$host" "$(wsl_path "$root/apps/$id/app.wasm")" "$(wsl_path "$fs")" 600 \
            "$(wsl_path "$TEST/wamr_$id.ppm")" "$script" | tail -2
        python -c "from PIL import Image; Image.open('$TEST/wamr_$id.ppm').save('$TEST/wamr_$id.png')"
        wsl /root/wamrc-build/wamrc --target=x86_64 --bounds-checks=1 --enable-multi-thread \
            -o "$(wsl_path "$TEST/$id.x64.aot")" "$(wsl_path "$root/apps/$id/app.wasm")" >/dev/null
        rm -f "$fs"/*.sav "$fs"/roms/*.sav "$fs"/gb.cfg
        echo "== $id aot (x86_64)"
        wsl "$host" "$(wsl_path "$TEST/$id.x64.aot")" "$(wsl_path "$fs")" 600 \
            "$(wsl_path "$TEST/wamr_${id}_aot.ppm")" "$script" | tail -1
    done
}

targets=("$@")
[ ${#targets[@]} -gt 0 ] || targets=(gbtobu gb2048 gblibbet gameboy)
for t in "${targets[@]}"; do
    echo "== $t"
    [ "$t" = test ] || [ "$t" = wamr ] || mkdir -p "$root/apps/$t"
    "build_$t"
done
echo "done."
