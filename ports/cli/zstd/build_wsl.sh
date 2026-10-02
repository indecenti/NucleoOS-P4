#!/bin/bash
# build_wsl.sh SRC OUT — zstd 1.5.7 CLI with gzip (zlib 1.3.1) and xz/lzma (liblzma from xz 5.8.4)
# support, single-threaded, for wasm32-wasip1, with the Linux wasi-sdk 34 and CMake in WSL.
# SRC (a /mnt path) holds zstd-1.5.7, zlib-1.3.1, xz-5.8.4; they are built in /tmp (CMake on
# /mnt is slow). One program answers as zstd, unzstd, zstdcat, gzip, gunzip, zcat, xz, unxz,
# xzcat, lzma, unlzma (nv_zstd_main.c).
set -euo pipefail
src="$1"; out="$2"; here="$(cd "$(dirname "$0")" && pwd)"
W=/opt/wasi-sdk-34.0-x86_64-linux
B=/tmp/nv_zstd_build; rm -rf "$B"; mkdir -p "$B"
cp -r "$src/zlib-1.3.1" "$src/xz-5.8.4" "$B/"
mkdir -p "$B/zstd"; cp -r "$src/zstd-1.5.7/lib" "$src/zstd-1.5.7/programs" "$B/zstd/"
CC=("$W/bin/clang" --target=wasm32-wasip1 --sysroot="$W/share/wasi-sysroot" -Os -w
    -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS -D_WASI_EMULATED_MMAN -D_WASI_EMULATED_GETPID)
# zlib: plain sources
Z="$B/zlib-1.3.1"; mkdir -p "$B/o/zlib"
for f in adler32 crc32 deflate infback inffast inflate inftrees trees zutil compress uncompr gzclose gzlib gzread gzwrite; do
    "${CC[@]}" -DHAVE_UNISTD_H -c -o "$B/o/zlib/$f.o" "$Z/$f.c"
done
# liblzma (CMake), no threads
cmake -S "$B/xz-5.8.4" -B "$B/xzb" -DCMAKE_TOOLCHAIN_FILE="$W/share/cmake/wasi-sdk-p1.cmake" \
    -DWASI_SDK_PREFIX="$W" -DCMAKE_BUILD_TYPE=MinSizeRel -DBUILD_SHARED_LIBS=OFF -DXZ_THREADS=no \
    -DXZ_NLS=OFF -DXZ_DOC=OFF -DXZ_TOOL_XZ=OFF -DXZ_TOOL_XZDEC=OFF -DXZ_TOOL_LZMADEC=OFF \
    -DXZ_TOOL_LZMAINFO=OFF -DXZ_TOOL_SCRIPTS=OFF -DXZ_SANDBOX=no >/dev/null
cmake --build "$B/xzb" --target liblzma -j8 >/dev/null
# zstd library + CLI
L="$B/zstd/lib"; P="$B/zstd/programs"; mkdir -p "$B/o/zstd"
ZF=(-DZSTD_GZCOMPRESS -DZSTD_GZDECOMPRESS -DZSTD_LZMACOMPRESS -DZSTD_LZMADECOMPRESS -DZSTD_NOBENCH
    -DZSTD_NODICT -DZSTD_NOTRACE -DZSTD_LEGACY_SUPPORT=0 -DZSTD_MULTITHREAD_SUPPORT_DEFAULT=0
    -DBACKTRACE_ENABLE=0 -DZSTD_DISABLE_ASM '-Dchown(p,u,g)=0' -I"$L" -I"$L/common" -I"$Z" -I"$B/xz-5.8.4/src/liblzma/api")
for f in "$L"/common/*.c "$L"/compress/*.c "$L"/decompress/*.c; do
    "${CC[@]}" "${ZF[@]}" -c -o "$B/o/zstd/lib-$(basename "$f" .c).o" "$f"
done
for f in zstdcli fileio fileio_asyncio util timefn; do
    "${CC[@]}" "${ZF[@]}" -Dmain=zstd_cli_main -c -o "$B/o/zstd/p-$f.o" "$P/$f.c"
done
"${CC[@]}" -c -o "$B/o/main.o" "$here/nv_zstd_main.c"
"${CC[@]}" -Wl,-z,stack-size=262144 -Wl,--strip-all -o "$out" "$B"/o/zstd/*.o "$B"/o/zlib/*.o "$B/o/main.o" \
    "$B/xzb/liblzma.a" -lwasi-emulated-signal -lwasi-emulated-process-clocks -lwasi-emulated-mman \
    -lwasi-emulated-getpid
