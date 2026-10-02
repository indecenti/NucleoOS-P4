#!/bin/bash
# build_wsl.sh SRC OUT — PDFio 1.6.5 (+ zlib 1.3.1) and three of its example tools as one WASI
# program (nv_pdfio_main.c), with the Linux wasi-sdk 34 in WSL. No libpng (image import unused).
set -euo pipefail
src="$1"; out="$2"; here="$(cd "$(dirname "$0")" && pwd)"
W=/opt/wasi-sdk-34.0-x86_64-linux
B=/tmp/nv_pdfio_build; rm -rf "$B"; mkdir -p "$B/o"
cp -r "$src/pdfio-1.6.5" "$src/zlib-1.3.1" "$B/"
CC=("$W/bin/clang" --target=wasm32-wasip1 --sysroot="$W/share/wasi-sysroot" -Os -w
    -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS -D_WASI_EMULATED_MMAN -D_WASI_EMULATED_GETPID
    -DHAVE_TIMEGM=1 -DHAVE_TM_GMTOFF=1 -I"$B/pdfio-1.6.5" -I"$B/zlib-1.3.1" -include "$here/nv_pdfio_port.h")
for f in adler32 crc32 deflate infback inffast inflate inftrees trees zutil compress uncompr; do
    "${CC[@]}" -DHAVE_UNISTD_H -c -o "$B/o/z-$f.o" "$B/zlib-1.3.1/$f.c"
done
for f in "$B"/pdfio-1.6.5/pdfio-*.c "$B/pdfio-1.6.5/ttf.c"; do
    "${CC[@]}" -c -o "$B/o/p-$(basename "$f" .c).o" "$f"
done
for e in pdf2text pdfioinfo pdfiomerge; do
    "${CC[@]}" -Dmain=${e}_main -c -o "$B/o/e-$e.o" "$B/pdfio-1.6.5/examples/$e.c"
done
"${CC[@]}" -c -o "$B/o/main.o" "$here/nv_pdfio_main.c"
"${CC[@]}" -Wl,-z,stack-size=262144 -Wl,--strip-all -o "$out" "$B"/o/*.o -lm \
    -lwasi-emulated-signal -lwasi-emulated-process-clocks -lwasi-emulated-mman -lwasi-emulated-getpid
