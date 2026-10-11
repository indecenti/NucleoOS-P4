#!/bin/bash
# build.sh — zimbench (F0 measurements, docs/ZIM_READER_PLAN.md) for the NucleoOS Terminal:
#   ports/zim/_out/zimbench/{manifest.json,app.wasm,app.aot}
#   bash ports/zim/bench/build.sh          (Git Bash on Windows; AOT via wamrc in WSL)
#   bash ports/zim/bench/build.sh native   (WSL gcc build in _out/zimbench-native, to test on the PC)
# Same toolchain as ports/cli/build.sh. Not a store app: it is pushed by hand, never exported.
set -euo pipefail
here="$(cygpath -m "$(cd "$(dirname "$0")" && pwd)")"
zim="$(cygpath -m "$(cd "$here/.." && pwd)")"
bash "$zim/fetch.sh" >/dev/null
Z="$zim/_src/zstd-1.5.7/lib"
out="$zim/_out/zimbench"; mkdir -p "$out"
ZSRC=("$Z"/common/*.c "$Z"/decompress/*.c)
ZF=(-DZSTD_DISABLE_ASM -DZSTD_LEGACY_SUPPORT=0 -DZSTD_NO_TRACE -I"$Z")
wsl_path() { echo "/mnt/$(echo "${1:0:1}" | tr 'A-Z' 'a-z')${1:2}"; }
DISTRO="${DISTRO:-Ubuntu-24.04}"

if [ "${1:-}" = native ]; then
    mkdir -p "$zim/_out/zimbench-native"
    srcs=(); for f in "${ZSRC[@]}" "$here/zimbench.c"; do srcs+=("$(wsl_path "$f")"); done
    MSYS_NO_PATHCONV=1 wsl.exe -d "$DISTRO" -- gcc -O2 -Wall -Wno-unused-function -DZSTD_DISABLE_ASM \
        -DZSTD_LEGACY_SUPPORT=0 -DZSTD_NO_TRACE -I"$(wsl_path "$Z")" -o "$(wsl_path "$zim/_out/zimbench-native/zimbench")" "${srcs[@]}"
    echo "  _out/zimbench-native/zimbench"
    exit 0
fi

CLANG="${CLANG:-/c/Program Files/LLVM/bin/clang.exe}"
WASI="${WASI:-D:/esp/wasi-sdk-34}"
SYSROOT="$WASI/wasi-sysroot-34.0"
BUILTINS="$WASI/libclang_rt-34.0/wasm32-unknown-wasip1/libclang_rt.builtins.a"
WAMRC="${WAMRC:-/root/wamrc-build/wamrc}"

"$CLANG" --target=wasm32-wasip1 "--sysroot=$SYSROOT" -O2 -nodefaultlibs -Wall -Wno-unused-function \
    -D_WASI_EMULATED_PROCESS_CLOCKS "${ZF[@]}" -Wl,-z,stack-size=262144 -Wl,--strip-all \
    -o "$out/app.wasm" "${ZSRC[@]}" "$here/zimbench.c" -lc -lwasi-emulated-process-clocks "$BUILTINS"
MSYS_NO_PATHCONV=1 wsl.exe -d "$DISTRO" -- "$WAMRC" --target=riscv32 --target-abi=ilp32f --cpu=generic-rv32 \
    --cpu-features=+m,+a,+c,+f --enable-multi-thread -o "$(wsl_path "$out/app.aot")" "$(wsl_path "$out/app.wasm")" \
    >/dev/null
cp "$here/manifest.json" "$out/manifest.json"
echo "  $(stat -c %s "$out/app.wasm") app.wasm, $(stat -c %s "$out/app.aot") app.aot -> $out"
