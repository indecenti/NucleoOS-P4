#!/bin/bash
# build_wsl.sh SRC OUT — dateutils 0.4.12 as one WASI program (nv_dateutils_main.c dispatches).
# A native configure + make in WSL first generates what the tarball lacks (config.h, the gperf
# tables and yuck option parsers); then the sources compile again for wasm32-wasip1.
set -euo pipefail
src="$1"; out="$2"; here="$(cd "$(dirname "$0")" && pwd)"
W=/opt/wasi-sdk-34.0-x86_64-linux
cd "$src"
if [ ! -f src/dadd.yucc ] || [ ! -f src/config.h ]; then
    ./configure --disable-shared --without-old-links >/dev/null
    make -j8 >/dev/null
fi
o="$src/_wasm"; rm -rf "$o"; mkdir -p "$o"
CC=("$W/bin/clang" --target=wasm32-wasip1 --sysroot="$W/share/wasi-sysroot" -O2 -w
    -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS -D_WASI_EMULATED_MMAN
    -DHAVE_CONFIG_H -D_POSIX_C_SOURCE=200112L -D_XOPEN_SOURCE=600 -D_BSD_SOURCE -D_DEFAULT_SOURCE
    -DHAVE_VERSION_H '-DTZDIR="/zoneinfo"' '-DTZMAP_DIR="/zoneinfo"' -Isrc -Ilib -Ibuild-aux)
for f in version date-core time-core dt-core strops token dt-locale tzraw tzmap leaps dt-core-tz-glue; do
    "${CC[@]}" -c -o "$o/lib-$f.o" "lib/$f.c"
done
for f in alist dt-io-zone dt-io prchunk; do "${CC[@]}" -c -o "$o/io-$f.o" "src/$f.c"; done
for p in dadd ddiff dseq dconv dround dtest dgrep dzone strptime; do
    "${CC[@]}" -Dmain=${p}_main "-Dtzset()=((void)0)" -Dprog=prog_${p} -c -o "$o/p-$p.o" "src/$p.c"   # WASI has no tzset (UTC only)
done
"${CC[@]}" -c -o "$o/main.o" "$here/nv_dateutils_main.c"
"${CC[@]}" -Wl,-z,stack-size=262144 -Wl,--strip-all -o "$out" "$o"/*.o \
    -lwasi-emulated-signal -lwasi-emulated-process-clocks -lwasi-emulated-mman -lm
