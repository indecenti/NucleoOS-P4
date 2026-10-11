#!/bin/bash
# build.sh — the zim program (ports/zim/cli/zim.c + nvzim + nvhtml) for the NucleoOS Terminal:
#   apps/zim/{app.wasm,app.aot,icon.z}
#   bash ports/zim/build.sh          (Git Bash; AOT via wamrc in WSL, like ports/cli/build.sh)
# Test on the PC first: bash ports/zim/test.sh. The manifest and guides in apps/zim are hand-written.
set -euo pipefail
here="$(cygpath -m "$(cd "$(dirname "$0")" && pwd)")"      # D:/.../ports/zim
ports="$(cygpath -m "$(cd "$here/.." && pwd)")"
root="$(cygpath -m "$(cd "$ports/.." && pwd)")"
bash "$here/fetch.sh" >/dev/null
S="$here/_src"
X="$S/xz-embedded-20240322"
CLANG="${CLANG:-/c/Program Files/LLVM/bin/clang.exe}"
WASI="${WASI:-D:/esp/wasi-sdk-34}"
WAMRC="${WAMRC:-/root/wamrc-build/wamrc}"
DISTRO="${DISTRO:-Ubuntu-24.04}"
wsl_path() { echo "/mnt/$(echo "${1:0:1}" | tr 'A-Z' 'a-z')${1:2}"; }
out="$root/apps/zim"
mkdir -p "$out"

mkdir -p "$here/_out"
log="$here/_out/build.log"
"$CLANG" --target=wasm32-wasip1 "--sysroot=$WASI/wasi-sysroot-34.0" -O2 -nodefaultlibs -Wall -Wno-unused-function \
    -D_WASI_EMULATED_PROCESS_CLOCKS -DZSTD_DISABLE_ASM -DZSTD_LEGACY_SUPPORT=0 -DXZ_USE_CRC64 \
    -I"$here/nvzim" -I"$here/nvhtml" -I"$S/zstd-1.5.7/lib" -I"$X/linux/include/linux" -I"$X/userspace" \
    -Wl,-z,stack-size=262144 -Wl,--strip-all -o "$out/app.wasm" \
    "$here/cli/zim.c" "$here/nvhtml/nvhtml.c" "$here/nvzim/nvzim.c" "$here/nvzim/nvzim_md5.c" \
    "$S"/zstd-1.5.7/lib/common/*.c "$S"/zstd-1.5.7/lib/decompress/*.c \
    "$X"/linux/lib/xz/xz_crc32.c "$X"/linux/lib/xz/xz_crc64.c "$X"/linux/lib/xz/xz_dec_stream.c \
    "$X"/linux/lib/xz/xz_dec_lzma2.c \
    -lc -lwasi-emulated-process-clocks "$WASI/libclang_rt-34.0/wasm32-unknown-wasip1/libclang_rt.builtins.a" \
    >"$log" 2>&1 || { grep -v -e 'zstd-1.5.7' -e 'xz-embedded' "$log" >&2; echo "zim: compile failed" >&2; exit 1; }
grep -v -e 'zstd-1.5.7' -e 'xz-embedded' "$log" || true   # our own warnings, if any
n=$(stat -c %s "$out/app.wasm")
[ "$n" -le 2097152 ] || { echo "zim: app.wasm $n bytes > 2 MB device cap" >&2; exit 1; }
MSYS_NO_PATHCONV=1 wsl.exe -d "$DISTRO" -- "$WAMRC" --target=riscv32 --target-abi=ilp32f --cpu=generic-rv32 \
    --cpu-features=+m,+a,+c,+f --enable-multi-thread -o "$(wsl_path "$out/app.aot")" "$(wsl_path "$out/app.wasm")" \
    >/dev/null
a=$(stat -c %s "$out/app.aot")
[ "$a" -le 4194304 ] || { echo "zim: app.aot $a bytes > 4 MB device cap" >&2; exit 1; }
python "$ports/make_icon.py" "$out/icon.z" "ZIM" "#1f4e79" "#f2c14e"
echo "  zim/app.wasm $n bytes, app.aot $a bytes"
