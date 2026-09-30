#!/bin/bash
# test.sh — PC tests of the openHASP app before it reaches the board, driven by host/*.txt scripts
# (MQTT commands, taps, back gesture); each run writes PNG screenshots and the MQTT traffic
# (mqtt.log) to ports/_out/openhasp/<mode>/<script>/:
#
#   native  the same sources as app.wasm (build.sh), built natively in WSL with -DNV_SIM, ASan and
#           UBSan, against host/nv_sim.c
#   aot     the real apps/openhasp/app.wasm compiled by wamrc to x86_64 AOT and run in WAMR by
#           host/wamr_host.c the way the device runs it (WASI "/" = data folder, 8 MB memory cap,
#           48 KB native stack); needs /root/nvhost-lib2/libiwasm.a (bash ports/host/build.sh)
#   wasm    the same in the WAMR interpreter (slow)
#
#   bash ports/openhasp/test.sh [native|aot|wasm ...] [script ...]
#        (Git Bash; default: native aot, every host/*.txt)
#
# Data folder of a run (the app's "/"): the openHASP docs "Widgets Demo" pages.jsonl (+ the logo its
# img object loads) and host/demo_1024.jsonl, which ha_push.txt pushes over MQTT the way the Home
# Assistant integration does.
set -euo pipefail
DISTRO="${DISTRO:-Ubuntu-24.04}"
if [ "${1:-}" != "--in-wsl" ]; then
    here="$(cd "$(dirname "$0")" && pwd)"
    bash "$here/fetch.sh" >/dev/null
    w="/mnt/$(cygpath -m "$here" | sed -E 's|^([A-Za-z]):|\L\1|')"
    MSYS_NO_PATHCONV=1 exec wsl.exe -d "$DISTRO" -- bash "$w/test.sh" --in-wsl "$@"
fi
shift
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
S="$root/ports/_src/openhasp"
source "$here/sources.sh"

OBJ="$HOME/openhasp_obj/native"
GEN="$HOME/openhasp_obj/gen"
mkdir -p "$OBJ" "$GEN"
gen_ttf "$GEN/openhasp_ttf.c"
DOCS_SHA=2f23d1f30111c2ea7aa32ee2e5a3d5d1605206f4
fail=0

modes=()
scripts=()
for a in "$@"; do
    case "$a" in
    native | aot | wasm) modes+=("$a") ;;
    *) scripts+=("$a") ;;
    esac
done
[ ${#modes[@]} -gt 0 ] || modes=(native aot)
if [ ${#scripts[@]} -eq 0 ]; then
    for f in "$here"/host/*.txt; do scripts+=("$(basename "$f" .txt)"); done
fi

for m in "${modes[@]}"; do
    case $m in
    native)
        SAN=(-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize=alignment)
        COMMON=("${SAN[@]}" -DNV_SIM "${DEFS[@]}" "${INCS[@]}" -include "$here/shim/nv_hasp_port.h" -w)
        CFLAGS=("${COMMON[@]}" -std=gnu11)
        CXXFLAGS=("${COMMON[@]}" -std=gnu++17 -fno-exceptions -fno-rtti)
        echo "== native build (ASan/UBSan)"
        compile_all "$OBJ" gcc g++ CFLAGS CXXFLAGS "$GEN/openhasp_ttf.c" "$here/host/nv_sim.c"
        g++ "${SAN[@]}" -o "$OBJ/openhasp_sim" "${OBJS[@]}" -Wl,--wrap=localtime -Wl,--wrap=localtime_r -lm
        ;;
    aot | wasm)
        LIB=/root/nvhost-lib2/libiwasm.a
        [ -f $LIB ] || bash "$root/ports/host/build.sh"
        gcc -O2 -DNV_SIM -I"$root/reference/wasm-micro-runtime/core/iwasm/include" -I"$root/sdk/include" \
            -I"$here/host" -o "$OBJ/openhasp_wamr" "$here/host/wamr_host.c" $LIB -lm -lpthread -ldl
        mod="$root/apps/openhasp/app.wasm"
        if [ $m = aot ]; then
            /root/wamrc-build/wamrc --target=x86_64 --bounds-checks=1 --enable-multi-thread \
                -o "$OBJ/app_x64.aot" "$mod" >/dev/null
            mod="$OBJ/app_x64.aot"
        fi
        ;;
    esac

    for s in "${scripts[@]}"; do
        echo "== $m / $s"
        D="$HOME/openhasp_test/$m/$s" # the app's data folder ("/")
        out="$root/ports/_out/openhasp/$m/$s"
        rm -rf "$D" "$out"
        mkdir -p "$D" "$out"
        # pages.jsonl = the code block of the docs' Widgets Demo
        sed -n '/^```json/,/^```$/p' "$S/../openhasp-docs-widgets-$DOCS_SHA.md" | sed '1d;$d' > "$D/pages.jsonl"
        cp "$S/../openhasp-docs-logo-medium-$DOCS_SHA.png" "$D/logo-medium.png"
        cp "$here/host/demo_1024.jsonl" "$D/"
        if [ -f "$here/host/$s.config.json" ]; then cp "$here/host/$s.config.json" "$D/config.json"; fi
        if [ $m = native ]; then
            cmd=("$OBJ/openhasp_sim" "$D" "$here/host/$s.txt" "$out")
        else
            cmd=("$OBJ/openhasp_wamr" "$mod" "$D" "$here/host/$s.txt" "$out")
        fi
        if ! ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 "${cmd[@]}" > "$out/run.log" 2>&1; then
            echo "  FAILED (see ports/_out/openhasp/$m/$s/run.log)"
            tail -5 "$out/run.log"
            fail=1
        fi
        grep -E "ERROR: AddressSanitizer|TRAP|returned" "$out/run.log" | head -5 || true
        grep -E "runtime error" "$out/run.log" | sed -E 's|.*/(lv_[a-z_]+/[a-z_]+\.c:[0-9]+).*|  ubsan (lvgl): \1|' |
            sort -u | head -10 || true
        python3 - "$out" <<'PY'
import sys, glob, os
from PIL import Image
for p in sorted(glob.glob(os.path.join(sys.argv[1], "*.ppm"))):
    Image.open(p).save(p[:-4] + ".png"); os.remove(p)
    print("  shot", os.path.basename(p[:-4]) + ".png")
PY
        echo "  published: $(grep -c '^PUB' "$out/mqtt.log" || true)"
    done
done
exit $fail
