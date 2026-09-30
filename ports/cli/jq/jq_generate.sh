#!/bin/bash
# jq_generate.sh <jq source dir> <oniguruma source dir> — WSL side of the jq build (run by
# ports/cli/build.sh). autoconf's configure for host wasm32-wasip1 (the Linux wasi-sdk in /opt
# runs its checks; the resulting -D list lands in the Makefile's DEFS), then jq's generated
# sources src/builtin.inc (builtin.jq as a C string), src/version.h and src/config_opts.inc.
# jq is configured --without-oniguruma (its "builtin" mode wants a git submodule); build.sh
# compiles Oniguruma itself and adds -DHAVE_LIBONIG=1, so only Oniguruma's src/config.h is
# needed from its configure.
set -e
W=/opt/wasi-sdk-34.0-x86_64-linux
cd "$1"
./configure --host=wasm32-wasip1 \
    CC="$W/bin/clang --target=wasm32-wasip1 --sysroot=$W/share/wasi-sysroot -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS" \
    LDFLAGS="-lwasi-emulated-signal -lwasi-emulated-process-clocks" \
    --without-oniguruma --disable-docs --disable-maintainer-mode --disable-valgrind --enable-all-static \
    >/dev/null
make src/builtin.inc src/version.h src/config_opts.inc >/dev/null
cd "$2"
./configure --host=wasm32-wasip1 \
    CC="$W/bin/clang --target=wasm32-wasip1 --sysroot=$W/share/wasi-sysroot" \
    --disable-shared --enable-static --disable-posix-api >/dev/null
