#!/usr/bin/env bash
# Ricompila ../mpeg1.wasm (decoder MPEG-1/MP2 del Video Player web) da mpeg1_wasm.c + pl_mpeg.h.
#
# Serve: clang con backend wasm32 + wasm-ld (LLVM >= 16, es. winget install LLVM.LLVM) e il
# sysroot + builtins di wasi-sdk (asset di release wasi-sysroot-<v>.tar.gz e libclang_rt-<v>.tar.gz).
# Percorsi sovrascrivibili con le variabili CLANG / WASI_SYSROOT / WASI_BUILTINS.
#
#   bash sd/web/apps/video-player/wasm/build_mpeg1_wasm.sh
#
# Il modulo è un "reactor" WASI senza stdio: nessun import (o solo stub banali), export con
# prefisso mp_ + memory. Dopo la build rigenera i .gz gemelli (il server li preferisce).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
out="$here/../mpeg1.wasm"

CLANG="${CLANG:-}"
if [ -z "$CLANG" ]; then
  if command -v clang >/dev/null 2>&1; then CLANG=clang
  elif [ -x "/c/Program Files/LLVM/bin/clang.exe" ]; then CLANG="/c/Program Files/LLVM/bin/clang.exe"
  else echo "clang non trovato (installa LLVM o imposta CLANG=...)" >&2; exit 1; fi
fi
WASI_SYSROOT="${WASI_SYSROOT:-/d/esp/wasi-sdk-34/wasi-sysroot-34.0}"
WASI_BUILTINS="${WASI_BUILTINS:-/d/esp/wasi-sdk-34/libclang_rt-34.0/wasm32-unknown-wasip1/libclang_rt.builtins.a}"
[ -f "$WASI_SYSROOT/lib/wasm32-wasip1/libc.a" ] || { echo "sysroot WASI mancante: $WASI_SYSROOT" >&2; exit 1; }
[ -f "$WASI_BUILTINS" ] || { echo "builtins mancanti: $WASI_BUILTINS" >&2; exit 1; }

"$CLANG" --target=wasm32-wasip1 --sysroot="$WASI_SYSROOT" \
  -O3 -flto -Wall -Wno-unused-function -Wno-unused-variable \
  -nodefaultlibs -mexec-model=reactor \
  -Wl,--strip-all -Wl,--gc-sections -Wl,--lto-O3 \
  -Wl,-z,stack-size=262144 -Wl,--initial-memory=16777216 -Wl,--max-memory=1073741824 \
  -o "$out" "$here/mpeg1_wasm.c" -lc "$WASI_BUILTINS"

# gemelli compressi per il server web della board
for f in "$out" "$here/mpeg1_wasm.c" "$here/pl_mpeg.h" "$here/build_mpeg1_wasm.sh" "$here/build_mpeg1_wasm.ps1"; do
  gzip -9 -n -k -f "$f"
done
ls -l "$out" "$out.gz"
