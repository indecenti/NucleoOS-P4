#!/bin/bash
# build.sh — build the NucleoOS terminal programs (WASI ports of existing software) into apps/:
#
#   lua      Lua 5.4          apps/lua/{app.wasm,app.aot,icon.z}
#   js       QuickJS-ng       apps/js/...
#   sqlite3  SQLite shell     apps/sqlite3/...
#
#   bash ports/build.sh [lua|js|sqlite3 ...]     (Git Bash on Windows; default: all)
#
# Needs: LLVM clang (wasm32 backend), the wasi-sdk sysroot + builtins (see sdk/build_app.ps1),
# WSL with wamrc built from the firmware's WAMR tree (/root/wamrc-build/wamrc) for the riscv32 AOT
# images, and Python 3 + Pillow for the icons. Sources come from ports/fetch.sh (pinned).
# The manifests (apps/<id>/manifest.json) are hand-written and committed; this script only
# regenerates the binaries. Test on the PC first: bash ports/test.sh (WSL host, same WAMR).
set -euo pipefail
# Windows-style paths (D:/...): clang.exe is a native program and gets them verbatim.
here="$(cygpath -m "$(cd "$(dirname "$0")" && pwd)")"
root="$(cygpath -m "$(cd "$here/.." && pwd)")"
bash "$here/fetch.sh" >/dev/null

CLANG="${CLANG:-/c/Program Files/LLVM/bin/clang.exe}"
WASI="${WASI:-D:/esp/wasi-sdk-34}"
SYSROOT="$WASI/wasi-sysroot-34.0"
BUILTINS="$WASI/libclang_rt-34.0/wasm32-unknown-wasip1/libclang_rt.builtins.a"
WAMRC="${WAMRC:-/root/wamrc-build/wamrc}"
DISTRO="${DISTRO:-Ubuntu-24.04}"
S="$here/_src"

# Shared flags: wasip1 command, 256 KB C stack in linear memory, emulated signal/clock shims.
CFLAGS=(--target=wasm32-wasip1 "--sysroot=$SYSROOT" -O2 -nodefaultlibs
        -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS -Wno-deprecated-declarations)
LDFLAGS=(-Wl,-z,stack-size=262144 -Wl,--strip-all)
LIBS=(-lc -lwasi-emulated-signal -lwasi-emulated-process-clocks)

wsl_path() { echo "/mnt/$(echo "${1:0:1}" | tr 'A-Z' 'a-z')${1:2}"; }   # D:/x -> /mnt/d/x

# riscv32 AOT for the ESP32-P4 (same flags as sdk/build_app.ps1 -Wasi -Aot).
aot() {
    local wasm="$1" out="${1%.wasm}.aot"
    MSYS_NO_PATHCONV=1 wsl.exe -d "$DISTRO" -- "$WAMRC" --target=riscv32 --target-abi=ilp32f --cpu=generic-rv32 \
        --cpu-features=+m,+a,+c,+f --enable-multi-thread -o "$(wsl_path "$out")" "$(wsl_path "$wasm")" \
        >/dev/null
    local n; n=$(stat -c %s "$out")
    [ "$n" -le 4194304 ] || { echo "$out: $n bytes > 4 MB device cap" >&2; exit 1; }
    echo "  $(basename "$(dirname "$out")")/app.aot  $n bytes"
}

finish() {   # id label bg fg
    local dir="$root/apps/$1" n
    n=$(stat -c %s "$dir/app.wasm")
    [ "$n" -le 2097152 ] || { echo "$1: app.wasm $n bytes > 2 MB device cap" >&2; exit 1; }
    echo "  $1/app.wasm  $n bytes"
    aot "$dir/app.wasm"
    python "$here/make_icon.py" "$dir/icon.z" "$2" "$3" "$4"
}

build_lua() {
    local L="$S/lua-5.4.9/src" srcs=()
    for f in "$L"/*.c; do [ "$(basename "$f")" = luac.c ] || srcs+=("$f"); done
    "$CLANG" "${CFLAGS[@]}" -I"$here/common" -I"$here/lua/shim" -I"$here/lua" -include nv_lua_port.h \
        -DLUA_COMPAT_5_3 "${LDFLAGS[@]}" -o "$root/apps/lua/app.wasm" "${srcs[@]}" \
        "$here/lua/nv_lua_glue.c" "${LIBS[@]}" "$BUILTINS"
    finish lua "Lua" "#1f2d8f" "#ffffff"
}

build_js() {
    local Q="$S/quickjs-0.17.0"
    "$CLANG" "${CFLAGS[@]}" -I"$Q" -D_GNU_SOURCE -DQJS_BUILD_LIBC "${LDFLAGS[@]}" \
        -o "$root/apps/js/app.wasm" "$Q/dtoa.c" "$Q/libregexp.c" "$Q/libunicode.c" "$Q/quickjs.c" \
        "$Q/quickjs-libc.c" "$here/qjs/nv_js.c" "${LIBS[@]}" "$BUILTINS"
    finish js "JS" "#f7df1e" "#1a1a1a"
}

build_sqlite3() {
    local Q="$S/sqlite-amalgamation-3530400"
    # No threads, processes, mmap, WAL shared memory or extensions under WASI. -Oz and no FTS5:
    # the riscv32 AOT image must stay under the device's 4 MB cap (-Os + FTS5 made it 4.3 MB).
    # USE_PREAD: without it the unix VFS seeks then reads, and a seek past EOF grows a FATFS file
    # (FatFs f_lseek) — every new database got 24 garbage bytes. pread goes through nv_wasm_wasi.
    "$CLANG" "${CFLAGS[@]}" -Oz -I"$Q" -D_WASI_EMULATED_GETPID -D_WASI_EMULATED_MMAN \
        -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_WAL -DSQLITE_NOHAVE_SYSTEM \
        -DSQLITE_OMIT_POPEN -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_ENABLE_MATH_FUNCTIONS \
        -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE -DUSE_PREAD=1 \
        -DHAVE_READLINE=0 "${LDFLAGS[@]}" -o "$root/apps/sqlite3/app.wasm" \
        "$Q/sqlite3.c" "$Q/shell.c" "${LIBS[@]}" -lwasi-emulated-getpid -lwasi-emulated-mman "$BUILTINS"
    finish sqlite3 "SQL" "#0f6cb3" "#ffffff"
}

targets=("$@")
[ ${#targets[@]} -gt 0 ] || targets=(lua js sqlite3)
for t in "${targets[@]}"; do
    echo "== $t"
    mkdir -p "$root/apps/$t"
    "build_$t"
done
echo "done. Publish: copy apps/<id>/ to the store folder (server/appstore serves apps/ directly)."
