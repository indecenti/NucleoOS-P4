#!/bin/bash
# build_wasi.sh — zimdump for the NucleoOS Terminal (WASI + riscv32 AOT), to try nvzim on the board:
#   ports/zim/_out/zimdump/{manifest.json,app.wasm,app.aot}
#   bash ports/zim/tools/build_wasi.sh          (Git Bash; AOT via wamrc in WSL)
# Same toolchain as ports/cli/build.sh. A test tool, pushed by hand: not a store app.
set -euo pipefail
here="$(cygpath -m "$(cd "$(dirname "$0")" && pwd)")"
zim="$(cygpath -m "$(cd "$here/.." && pwd)")"
bash "$zim/fetch.sh" >/dev/null
S="$zim/_src"
X="$S/xz-embedded-20240322"
out="$zim/_out/zimdump"; mkdir -p "$out"
CLANG="${CLANG:-/c/Program Files/LLVM/bin/clang.exe}"
WASI="${WASI:-D:/esp/wasi-sdk-34}"
WAMRC="${WAMRC:-/root/wamrc-build/wamrc}"
DISTRO="${DISTRO:-Ubuntu-24.04}"
wsl_path() { echo "/mnt/$(echo "${1:0:1}" | tr 'A-Z' 'a-z')${1:2}"; }

"$CLANG" --target=wasm32-wasip1 "--sysroot=$WASI/wasi-sysroot-34.0" -O2 -nodefaultlibs -Wall -Wno-unused-function \
    -D_WASI_EMULATED_PROCESS_CLOCKS -DZSTD_DISABLE_ASM -DZSTD_LEGACY_SUPPORT=0 -DXZ_USE_CRC64 \
    -I"$zim/nvzim" -I"$S/zstd-1.5.7/lib" -I"$X/linux/include/linux" -I"$X/userspace" \
    -Wl,-z,stack-size=262144 -Wl,--strip-all -o "$out/app.wasm" \
    "$here/zimdump.c" "$zim/nvzim/nvzim.c" "$zim/nvzim/nvzim_md5.c" \
    "$S"/zstd-1.5.7/lib/common/*.c "$S"/zstd-1.5.7/lib/decompress/*.c \
    "$X"/linux/lib/xz/xz_crc32.c "$X"/linux/lib/xz/xz_crc64.c "$X"/linux/lib/xz/xz_dec_stream.c \
    "$X"/linux/lib/xz/xz_dec_lzma2.c \
    -lc -lwasi-emulated-process-clocks "$WASI/libclang_rt-34.0/wasm32-unknown-wasip1/libclang_rt.builtins.a" 2>&1 \
    | grep -v -e 'zstd-1.5.7' -e 'xz-embedded' || true
MSYS_NO_PATHCONV=1 wsl.exe -d "$DISTRO" -- "$WAMRC" --target=riscv32 --target-abi=ilp32f --cpu=generic-rv32 \
    --cpu-features=+m,+a,+c,+f --enable-multi-thread -o "$(wsl_path "$out/app.aot")" "$(wsl_path "$out/app.wasm")" >/dev/null
cat > "$out/manifest.json" <<'JSON'
{
  "id": "zimdump",
  "name": "zimdump",
  "version": "0.1",
  "abi": 8,
  "console": true,
  "ram_budget": 16777216,
  "stack_kb": 256,
  "permissions": ["home"],
  "author": "NucleoOS",
  "license": "PolyForm-Noncommercial-1.0.0",
  "description": "nvzim test tool (ports/zim): ZIM info, entries, articles, suggestions, checksum. Not a store app.",
  "usage": "zimdump info|entries|get|suggest|titles|verify|stats FILE [ARGS]"
}
JSON
echo "  $(stat -c %s "$out/app.wasm") app.wasm, $(stat -c %s "$out/app.aot") app.aot -> $out"
