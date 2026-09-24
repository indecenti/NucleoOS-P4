#!/bin/bash
# Build the PC test host for NucleoOS WASI ports (run inside WSL):
#   nvhost = WAMR (fast-interp + AOT, libc-wasi, thread manager, ref-types — the firmware's
#   feature set, x86_64) + the "nv" imports console programs use (nv_sjlj.h).
# AOT images for it: wamrc --target=x86_64 --bounds-checks=1 --enable-multi-thread (the device
# needs riscv32 ones; see ports/build.sh).
set -e
here="$(cd "$(dirname "$0")" && pwd)"
wamr="$(cd "$here/../../reference/wasm-micro-runtime" && pwd)"
lib=/root/nvhost-lib2   # no HW bound check: software checks like the ESP32-P4
if [ ! -f "$lib/libiwasm.a" ]; then
    mkdir -p "$lib"
    cmake -S "$wamr/product-mini/platforms/linux" -B "$lib" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DWAMR_BUILD_INTERP=1 -DWAMR_BUILD_FAST_INTERP=1 -DWAMR_BUILD_AOT=1 -DWAMR_BUILD_JIT=0 \
        -DWAMR_BUILD_LIBC_WASI=1 -DWAMR_BUILD_LIBC_BUILTIN=1 -DWAMR_BUILD_THREAD_MGR=1 \
        -DWAMR_BUILD_REF_TYPES=1 -DWAMR_BUILD_BULK_MEMORY=1 -DWAMR_BUILD_SIMD=0 \
        -DWAMR_DISABLE_HW_BOUND_CHECK=1 -DWAMR_DISABLE_STACK_HW_BOUND_CHECK=1 >/dev/null
    ninja -C "$lib" vmlib >/dev/null
fi
gcc -O2 -I"$wamr/core/iwasm/include" -o /root/nvhost "$here/nvhost.c" "$lib/libiwasm.a" -lm -lpthread -ldl
echo "OK /root/nvhost"
