#!/bin/bash
# test.sh — PC tests of the Puzzles app before it reaches the board (Git Bash; runs in WSL):
#
#   1. native: every puzzle + nv_puzzles.c built for x86_64 with ASan/UBSan against test/native_stub.c,
#      driven by the scripted touch session of test/script.c (open, tap, drag, long press, keys,
#      undo/redo, solve, new, presets, restart, back, resume). Any memory error aborts.
#   2. wamr: the real apps/puzzles/app.wasm in WAMR (interpreter, then x86_64 AOT) with the gfx
#      imports of test/host_gfx.c, same script, on a 48 KB native stack like the device's worker.
#
#   bash ports/puzzles/test.sh [native|wamr ...] [--game N]
# Screenshots (PPM -> PNG) land in ports/_out/puzzles/<mode>/.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
w="/mnt/$(cygpath -m "$here" | sed -E 's|^([A-Za-z]):|\L\1|')"   # /mnt/d/NucleoV2/ports/puzzles
root="${w%/ports/puzzles}"
export MSYS_NO_PATHCONV=1
modes=() game=""
while [ $# -gt 0 ]; do
    case "$1" in --game) game="$2"; shift ;; *) modes+=("$1") ;; esac
    shift
done
[ ${#modes[@]} -gt 0 ] || modes=(native wamr)
bash "$here/fetch.sh" >/dev/null
mkdir -p "$here/../_out/puzzles"

cat > "$here/../_out/puzzles/run.sh" <<EOF
#!/bin/bash
set -u
P=$root/ports/_src/sgt-puzzles-20250730.a7c7826
GEN=$root/ports/_src/puzzles_gen
T=$w/test
OUT=$root/ports/_out/puzzles
fail=0
games=\$(sed -E 's/GAME\((.*)\)/\1/' \$GEN/generated-games.h | tr -d '\r')
srcs=""
for f in combi divvy draw-poly drawing dsf findloop grid latin laydomino loopgen malloc matching midend \
         misc penrose penrose-legacy random sort tdq tree234 version hat spectre list \$games; do
    srcs="\$srcs \$P/\$f.c"
done
shots() {   # dir: PPM -> PNG
    python3 - "\$1" <<'PY' 2>/dev/null || true
import sys, glob, os
try:
    from PIL import Image
except ImportError:
    sys.exit(0)
for p in glob.glob(os.path.join(sys.argv[1], "*.ppm")):
    Image.open(p).save(p[:-4] + ".png"); os.remove(p)
PY
}
for mode in ${modes[*]}; do
    D=/root/pz_data/\$mode   # the app's data folder: WSL-local (the /mnt share is slow)
    rm -rf \$OUT/\$mode \$D; mkdir -p \$OUT/\$mode \$D
    case \$mode in
    native)
        echo "== native (ASan/UBSan)"
        O=/root/pz_obj; mkdir -p \$O   # upstream objects are cached (pinned sources)
        SAN="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -DCOMBINED -DNV_SIM -I\$P -I\$GEN -I$root/sdk/include -I\$T"
        # upstream quietly, in parallel (12 jobs); our files with all warnings on
        printf '%s\n' \$srcs | xargs -P 12 -I{} sh -c 'o='\$O'/\$(basename {} .c).o; [ -f \$o ] || gcc '"\$SAN"' -w -c {} -o \$o' || { echo "native build failed"; exit 1; }
        for f in $w/nv_puzzles.c \$T/native_stub.c \$T/script.c; do
            gcc \$SAN -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -c \$f -o \$O/\$(basename \$f .c).o || exit 1
        done
        objs=""; for f in \$srcs nv_puzzles native_stub script; do objs="\$objs \$O/\$(basename \$f .c).o"; done
        gcc -fsanitize=address,undefined -o \$OUT/pz_native \$objs -lm || exit 1
        PZ_DATA=\$D UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
            ASAN_OPTIONS=detect_leaks=0 \$OUT/pz_native \$OUT/\$mode $game || fail=1
        ;;
    wamr)
        echo "== wamr (app.wasm interpreted + x86_64 AOT)"
        [ -x /root/nvhost ] || bash $root/ports/host/build.sh
        WAMR=$root/reference/wasm-micro-runtime
        gcc -O2 -I\$WAMR/core/iwasm/include -I\$T -o \$OUT/pz_host \$T/host_gfx.c \$T/script.c \
            /root/nvhost-lib2/libiwasm.a -lm -lpthread -ldl || exit 1
        /root/wamrc-build/wamrc --target=x86_64 --bounds-checks=1 --enable-multi-thread \
            -o \$OUT/app_x64.aot $root/apps/puzzles/app.wasm >/dev/null || exit 1
        for m in aot wasm; do
            mod=$root/apps/puzzles/app.wasm; [ \$m = aot ] && mod=\$OUT/app_x64.aot
            mkdir -p \$OUT/\$mode/\$m \$D/\$m
            \$OUT/pz_host \$mod \$OUT/\$mode/\$m \$D/\$m $game || fail=1
            shots \$OUT/\$mode/\$m
        done
        ;;
    esac
    shots \$OUT/\$mode
done
[ \$fail = 0 ] && echo "ALL OK" || echo "FAILED"
exit \$fail
EOF
wsl.exe -d Ubuntu-24.04 -- bash "$w/../_out/puzzles/run.sh"
