#!/bin/bash
# build.sh — build the full-screen (curses) programs for the NucleoOS Terminal:
#
#   sc-im        sc-im 0.8.5 spreadsheet: xlsx/ods/csv/tsv, formulas  apps/sc-im/{app.wasm,app.aot}
#   wordgrinder  WordGrinder 0.9 word processor: odt/md/html/txt      apps/wordgrinder/...
#
#   bash ports/tui/build.sh [sc-im|wordgrinder]      (Git Bash on Windows; default: both)
#
# Everything compiles in WSL with the Linux wasi-sdk 34 (/opt/wasi-sdk-34.0-x86_64-linux): first
# the shared static libraries into /root/nv_tui/prefix (kept between runs: ncurses/build_wsl.sh,
# deps/build_wsl.sh; delete a library there to rebuild it), then each program (<id>/build_wsl.sh),
# then the riscv32 AOT with wamrc as in ports/build.sh. Sources come from ports/tui/fetch.sh
# (pinned, patched). The WASI side of a tty program is ports/tui/compat (termios, in-band resize,
# the POSIX calls WASI lacks). Manifests, guides and icons in apps/<id>/ are hand-made (icons:
# tools/make_mdi_icon.py). Test on the PC first: bash ports/tui/test.sh.
set -euo pipefail
here="$(cygpath -m "$(cd "$(dirname "$0")" && pwd)")"      # D:/.../ports/tui
root="$(cygpath -m "$(cd "$here/../.." && pwd)")"
bash "$here/fetch.sh" >/dev/null

WAMRC="${WAMRC:-/root/wamrc-build/wamrc}"
DISTRO="${DISTRO:-Ubuntu-24.04}"
PREFIX=/root/nv_tui/prefix
wsl_path() { echo "/mnt/$(echo "${1:0:1}" | tr 'A-Z' 'a-z')${1:2}"; }   # D:/x -> /mnt/d/x
w="$(wsl_path "$here")"
wsl() { MSYS_NO_PATHCONV=1 wsl.exe -d "$DISTRO" -- "$@"; }

aot() {
    local wasm="$1" out="${1%.wasm}.aot"
    wsl "$WAMRC" --target=riscv32 --target-abi=ilp32f --cpu=generic-rv32 \
        --cpu-features=+m,+a,+c,+f --enable-multi-thread -o "$(wsl_path "$out")" "$(wsl_path "$wasm")" \
        >/dev/null
    local n; n=$(stat -c %s "$out")
    [ "$n" -le 20971520 ] || { echo "$out: $n bytes > 20 MB device cap" >&2; exit 1; }
    echo "  $(basename "$(dirname "$out")")/app.aot  $n bytes"
}

targets=("$@")
[ ${#targets[@]} -eq 0 ] && targets=(sc-im wordgrinder)

wsl bash "$w/ncurses/build_wsl.sh" "$w/_src" "$PREFIX"
wsl bash "$w/deps/build_wsl.sh" "$w/_src" "$PREFIX"
for id in "${targets[@]}"; do
    case "$id" in
        sc-im)       script=scim ;;
        wordgrinder) script=wordgrinder ;;
        *) echo "unknown target: $id" >&2; exit 1 ;;
    esac
    wsl bash "$w/$script/build_wsl.sh" "$w/_src" "$PREFIX" "$(wsl_path "$root/apps/$id/app.wasm")"
    n=$(stat -c %s "$root/apps/$id/app.wasm")
    [ "$n" -le 6291456 ] || { echo "$id: app.wasm $n bytes > 6 MB device cap" >&2; exit 1; }
    aot "$root/apps/$id/app.wasm"
done
