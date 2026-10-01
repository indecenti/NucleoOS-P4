#!/bin/bash
# build.sh — build the openHASP app (openHASP 0.7 + its LVGL 7.11 fork, as a NucleoOS WASI reactor
# exporting run) into apps/openhasp/:
#
#   app.wasm   wasm32-wasip1 reactor (gfx canvas 1024x600 + mqtt + fs, wasi-libc + libc++)
#   app.aot    riscv32 AOT image for the ESP32-P4 (what the device runs)
#   icon.z     launcher/store icon (tools/make_mdi_icon.py)
#
#   bash ports/openhasp/build.sh             (Git Bash on Windows; compiles inside WSL)
#   bash ports/openhasp/build.sh --no-aot    (skip wamrc)
#   bash ports/openhasp/build.sh --clean     (drop the object cache first)
#
# Needs, in WSL (Ubuntu-24.04): the wasi-sdk 34 Linux toolchain in /opt/wasi-sdk-34.0-x86_64-linux
# and wamrc (/root/wamrc-build/wamrc, built from the firmware's WAMR tree); on Windows: Python 3
# with Pillow + PyMuPDF for the icon. Sources: ports/openhasp/fetch.sh (pinned, sha256-checked,
# patched with ports/openhasp/patches/*.patch). Objects are cached in WSL (~/openhasp_obj).
# PC test before the board: bash ports/openhasp/test.sh.
set -euo pipefail

DISTRO="${DISTRO:-Ubuntu-24.04}"
if [ "${1:-}" != "--in-wsl" ]; then
    # ---- Git Bash side: fetch, compile in WSL, then the icon with the Windows Python ----------
    here="$(cd "$(dirname "$0")" && pwd)"
    root="$(cd "$here/../.." && pwd)"
    bash "$here/fetch.sh" >/dev/null
    w="/mnt/$(cygpath -m "$here" | sed -E 's|^([A-Za-z]):|\L\1|')"
    MSYS_NO_PATHCONV=1 wsl.exe -d "$DISTRO" -- bash "$w/build.sh" --in-wsl "$@"
    python "$root/tools/make_mdi_icon.py" "$root/apps/openhasp" view-dashboard-variant "#2b6cb0" >/dev/null
    rm -f "$root/apps/openhasp/icon.png"
    echo "done: apps/openhasp/{app.wasm,app.aot,icon.z} (+ manifest.json, hand-written)"
    exit 0
fi

# ---- WSL side ---------------------------------------------------------------------------------
shift
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
aot=1
for a in "$@"; do
    case "$a" in
    --no-aot) aot=0 ;;
    --clean) rm -rf "$HOME/openhasp_obj" ;;
    esac
done

WASI="${WASI:-/opt/wasi-sdk-34.0-x86_64-linux}"
WAMRC="${WAMRC:-/root/wamrc-build/wamrc}"
OUT="$root/apps/openhasp"
S="$root/ports/_src/openhasp"
source "$here/sources.sh"   # OH LV PNG AJ FT + DEFS INCS + the source lists

CC="$WASI/bin/wasm32-wasip1-clang"
CXX="$WASI/bin/wasm32-wasip1-clang++"
OBJ="$HOME/openhasp_obj/wasm"
GEN="$HOME/openhasp_obj/gen"
mkdir -p "$OBJ" "$GEN" "$OUT"

# openhasp.ttf (the FreeType font openHASP embeds on the ESP32-S3: Roboto Condensed + MDI icons)
gen_ttf "$GEN/openhasp_ttf.c"

# -Os like upstream (platformio.ini). No exceptions, no RTTI (openHASP and ArduinoJson use neither).
COMMON=(-Os -g0 -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS -I"$here/shim/wasi"
        "${DEFS[@]}" "${INCS[@]}" -include "$here/shim/nv_hasp_port.h" -w)
CFLAGS=("${COMMON[@]}" -std=gnu11)
CXXFLAGS=("${COMMON[@]}" -std=gnu++17 -fno-exceptions -fno-rtti)

compile_all "$OBJ" "$CC" "$CXX" CFLAGS CXXFLAGS "$GEN/openhasp_ttf.c"

# Reactor (entry "run"), 1 MB C stack in linear memory (FreeType and the JSON parser are
# stack-hungry; wasm locals live on the native stack_kb stack); malloc heap up to ram_budget.
"$CXX" -Os -mexec-model=reactor -Wl,--export=run -Wl,-z,stack-size=1048576 -Wl,--strip-all \
    -Wl,--wrap=localtime -Wl,--wrap=localtime_r -fno-exceptions -fno-rtti \
    -o "$OUT/app.wasm" "${OBJS[@]}" -lwasi-emulated-signal -lwasi-emulated-process-clocks

n=$(stat -c %s "$OUT/app.wasm")
[ "$n" -le 2097152 ] || { echo "app.wasm: $n bytes > 2 MB device cap" >&2; exit 1; }
echo "  openhasp/app.wasm  $n bytes"

# Upstream licenses and attribution of everything linked into the app, shipped next to it.
{
    echo "openHASP for NucleoOS: the openHASP firmware (HASwitchPlate/openHASP @ $OPENHASP_SHA) built"
    echo "as a WASI app with its LVGL 7 fork, lv_lib_png (lodepng), ArduinoJson 6 and FreeType 2."
    echo "Adaptation files: ports/openhasp in the NucleoOS repository (MIT). Licenses of the parts:"
    for part in "openHASP (MIT)|$OH/LICENSE" "LVGL, HASwitchPlate fork (MIT)|$LV/LICENCE.txt" \
                "lv_lib_png (MIT)|$PNG/LICENSE" "ArduinoJson (MIT)|$AJ/LICENSE.txt" \
                "FreeType (FreeType License; portions of this software are copyright The FreeType Project, www.freetype.org)|$FT/docs/FTL.TXT" \
                "Roboto Condensed font (Apache 2.0)|$OH/src/font/roboto/LICENSE" \
                "Material Design Icons font (Pictogrammers Free License / Apache 2.0)|$OH/src/font/MaterialDesign-Webfont/LICENSE"; do
        printf '\n\n==== %s ====\n\n' "${part%%|*}"
        cat "${part#*|}"
    done
    printf '\n\n==== lodepng (zlib) ====\n\n'
    sed -n '1,/^\*\//p' "$PNG/lodepng.h"
} > "$OUT/LICENSE.txt"

if [ $aot = 1 ]; then
    "$WAMRC" --target=riscv32 --target-abi=ilp32f --cpu=generic-rv32 --cpu-features=+m,+a,+c,+f \
        --enable-multi-thread -o "$OUT/app.aot" "$OUT/app.wasm" >/dev/null
    n=$(stat -c %s "$OUT/app.aot")
    [ "$n" -le 4194304 ] || { echo "app.aot: $n bytes > 4 MB device cap" >&2; exit 1; }
    echo "  openhasp/app.aot   $n bytes"
fi
