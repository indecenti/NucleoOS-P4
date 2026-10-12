#!/bin/bash
# build_wsl.sh SRC PREFIX — the static WASI libraries the office ports share, with the Linux
# wasi-sdk 34 in WSL: zlib 1.3.1, libzip 1.11.4 (deflate only, read side), libxml2 2.14.6 (tree
# and SAX parser only: no HTML, XPath, validation, threads, iconv), libxlsxwriter 1.2.4 (parts in
# memory streams, its own MiniZip) and libxls 1.6.3 (the old .xls format). Installed into PREFIX;
# each one is skipped when its library is already there. CMake runs on WSL's own disk (on /mnt/d
# it takes 10+ minutes).
set -euo pipefail
src="$1"; prefix="$2"
W=/opt/wasi-sdk-34.0-x86_64-linux
CC="$W/bin/clang --target=wasm32-wasip1 --sysroot=$W/share/wasi-sysroot"
EMU="-D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS -D_WASI_EMULATED_GETPID -D_WASI_EMULATED_MMAN"
TC="$W/share/cmake/wasi-sdk-p1.cmake"
B=/root/nv_tui/build/deps; mkdir -p "$B" "$prefix/lib" "$prefix/include"
cmake_wasi() {   # srcdir builddir [cmake args...]
    local s="$1" b="$2"; shift 2
    cmake -S "$s" -B "$b" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$TC" -DWASI_SDK_PREFIX="$W" \
        -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_C_FLAGS="$EMU -w" -DCMAKE_INSTALL_PREFIX="$prefix" \
        -DBUILD_SHARED_LIBS=OFF "$@" > "$b.cmake.log" 2>&1 || { tail -30 "$b.cmake.log"; exit 1; }
    ninja -C "$b" install > "$b.ninja.log" 2>&1 || { grep -m20 -B2 -A8 "error" "$b.ninja.log"; exit 1; }
}

if [ ! -f "$prefix/lib/libz.a" ]; then
    rm -rf "$B/zlib"; mkdir -p "$B/zlib"
    for f in adler32 crc32 deflate infback inffast inflate inftrees trees zutil compress uncompr \
             gzclose gzlib gzread gzwrite; do
        $CC -Os -w -DHAVE_UNISTD_H -c -o "$B/zlib/$f.o" "$src/zlib-1.3.1/$f.c"
    done
    "$W/bin/llvm-ar" rcs "$prefix/lib/libz.a" "$B"/zlib/*.o
    cp "$src/zlib-1.3.1/zlib.h" "$src/zlib-1.3.1/zconf.h" "$prefix/include/"
    echo "  zlib: $(stat -c %s "$prefix/lib/libz.a") bytes"
fi

if [ ! -f "$prefix/lib/libzip.a" ]; then
    rm -rf "$B/libzip-src" "$B/libzip"; cp -r "$src/libzip-1.11.4" "$B/libzip-src"
    cmake_wasi "$B/libzip-src" "$B/libzip" -DZLIB_INCLUDE_DIR="$prefix/include" \
        -DZLIB_LIBRARY="$prefix/lib/libz.a" -DENABLE_COMMONCRYPTO=OFF -DENABLE_GNUTLS=OFF \
        -DENABLE_MBEDTLS=OFF -DENABLE_OPENSSL=OFF -DENABLE_WINDOWS_CRYPTO=OFF -DENABLE_BZIP2=OFF \
        -DENABLE_LZMA=OFF -DENABLE_ZSTD=OFF -DBUILD_TOOLS=OFF -DBUILD_REGRESS=OFF \
        -DBUILD_OSSFUZZ=OFF -DBUILD_EXAMPLES=OFF -DBUILD_DOC=OFF
    echo "  libzip: $(stat -c %s "$prefix/lib/libzip.a") bytes"
fi

if [ ! -f "$prefix/lib/libxml2.a" ]; then
    rm -rf "$B/libxml2-src" "$B/libxml2"; cp -r "$src/libxml2-2.14.6" "$B/libxml2-src"
    off=""
    for o in CATALOG DEBUG HTML HTTP ICONV ICU ISO8859X LEGACY LZMA MODULES OUTPUT PATTERN \
             PROGRAMS PUSH PYTHON READLINE REGEXPS SAX1 TESTS THREADS TLS VALID XINCLUDE XPATH \
             XPTR ZLIB C14N READER SCHEMAS SCHEMATRON WRITER RELAXNG; do
        off="$off -DLIBXML2_WITH_$o=OFF"
    done
    cmake_wasi "$B/libxml2-src" "$B/libxml2" $off
    echo "  libxml2: $(stat -c %s "$prefix/lib/libxml2.a") bytes"
fi

if [ ! -f "$prefix/lib/libxlsxwriter.a" ]; then
    X="$src/libxlsxwriter-1.2.4"; rm -rf "$B/xlsxw"; mkdir -p "$B/xlsxw"
    XF=(-Os -w -I"$X/include" -I"$prefix/include" -DUSE_FMEMOPEN -DUSE_STANDARD_TMPFILE -DUSE_FILE32API -DNOCRYPT)
    for f in "$X"/src/*.c "$X"/third_party/minizip/ioapi.c "$X"/third_party/minizip/zip.c \
             "$X"/third_party/md5/md5.c; do
        $CC "${XF[@]}" -c -o "$B/xlsxw/$(basename "$f" .c).o" "$f"
    done
    "$W/bin/llvm-ar" rcs "$prefix/lib/libxlsxwriter.a" "$B"/xlsxw/*.o
    cp -r "$X/include/xlsxwriter" "$X/include/xlsxwriter.h" "$prefix/include/"
    echo "  libxlsxwriter: $(stat -c %s "$prefix/lib/libxlsxwriter.a") bytes"
fi

if [ ! -f "$prefix/lib/libxlsreader.a" ]; then
    X="$src/libxls-1.6.3"; rm -rf "$B/xls"; mkdir -p "$B/xls"
    cp "$(dirname "$0")/libxls_config.h" "$B/xls/config.h"
    for f in endian locale ole xls xlstool; do
        $CC -Os -w -I"$B/xls" -I"$X/include" -c -o "$B/xls/$f.o" "$X/src/$f.c"
    done
    "$W/bin/llvm-ar" rcs "$prefix/lib/libxlsreader.a" "$B"/xls/*.o
    cp -r "$X/include/libxls" "$X/include/xls.h" "$prefix/include/"
    echo "  libxls: $(stat -c %s "$prefix/lib/libxlsreader.a") bytes"
fi
