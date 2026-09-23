#!/bin/bash
# build.sh — build w4run (run from WSL/Linux): WAMR from reference/wasm-micro-runtime with the
# DEVICE's configuration (fast interpreter, software bounds checks — the P4 has no guard pages —
# WASI, reference types, bulk memory, thread manager), plus the firmware's WASM-4 host code.
#   bash tools/w4harness/build.sh            -> $W4_OUT/w4run   (default ~/w4harness)
set -e
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="${W4_OUT:-$HOME/w4harness}"
WAMR="$ROOT/reference/wasm-micro-runtime"
NV="$ROOT/components/nv_wasm"
mkdir -p "$OUT"

if [ ! -f "$OUT/wamr/libiwasm.a" ]; then
  echo "building WAMR (device config) ..."
  cmake -S "$WAMR/product-mini/platforms/linux" -B "$OUT/wamr" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DWAMR_BUILD_INTERP=1 -DWAMR_BUILD_FAST_INTERP=1 -DWAMR_BUILD_AOT=0 -DWAMR_BUILD_JIT=0 \
    -DWAMR_BUILD_FAST_JIT=0 -DWAMR_BUILD_LIBC_WASI=1 -DWAMR_BUILD_LIBC_BUILTIN=1 \
    -DWAMR_BUILD_REF_TYPES=1 -DWAMR_BUILD_BULK_MEMORY=1 -DWAMR_BUILD_SIMD=0 \
    -DWAMR_BUILD_THREAD_MGR=1 -DWAMR_BUILD_LIB_PTHREAD=0 \
    -DWAMR_DISABLE_HW_BOUND_CHECK=1 -DWAMR_DISABLE_STACK_HW_BOUND_CHECK=1 \
    -DWAMR_BUILD_TARGET=X86_64 > "$OUT/wamr.cfg.log" 2>&1
fi
ninja -C "$OUT/wamr" > "$OUT/wamr.build.log" 2>&1

gcc -O2 -c "$NV/w4/w4_framebuffer.c" -o "$OUT/w4_framebuffer.o"
gcc -O2 -c "$NV/w4/w4_apu.c" -o "$OUT/w4_apu.o"
g++ -O2 -std=c++17 -Wall -Wextra -I"$HERE/shim" -I"$NV" -I"$WAMR/core/iwasm/include" \
  "$HERE/w4run.cpp" "$NV/nv_wasm_w4.cpp" "$OUT/w4_framebuffer.o" "$OUT/w4_apu.o" \
  "$OUT/wamr/libiwasm.a" -lm -lpthread -ldl -o "$OUT/w4run"
echo "OK $OUT/w4run"
