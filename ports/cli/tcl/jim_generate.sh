#!/bin/bash
# jim_generate.sh <jimtcl source dir> — WSL side of the Jim Tcl build (run by ports/cli/build.sh).
# autosetup's configure, with host = wasm32-wasip1 and the Linux wasi-sdk in /opt for its
# compile checks, writes jimautoconf.h, jim-config.h, _load-static-exts.c and
# _unicode_mapping.c; its bootstrap
# interpreter jimsh0 turns the Tcl-coded extensions into C (_<name>.c).
set -e
cd "$1"
W=/opt/wasi-sdk-34.0-x86_64-linux
./configure --host=wasm32-wasip1 CC_FOR_BUILD=gcc \
    CC="$W/bin/clang --target=wasm32-wasip1 --sysroot=$W/share/wasi-sysroot -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS" \
    LDFLAGS="-lwasi-emulated-signal -lwasi-emulated-process-clocks" \
    --disable-lineedit --disable-ssl --disable-docs \
    --without-ext=exec,posix,signal,load,syslog,tty,history,readline,sqlite3,redis,sdl,zlib,mk,eventloop,aio-ssl \
    >/dev/null
# Link checks pass for these but NucleoOS has no sockets/select/pipes: keep aio plain-file only.
sed -i -E 's@^#define (HAVE_SHUTDOWN|HAVE_SELECT|HAVE_PIPE|HAVE_SYS_SOCKET_H|HAVE_SYS_UN_H|HAVE_ARPA_INET_H|HAVE_NETINET_IN_H|HAVE_INET_NTOP|HAVE_LINK|HAVE_SYMLINK|HAVE_READLINK) 1@/* #undef \1 (NucleoOS) */@' jimautoconf.h
make _load-static-exts.c _unicode_mapping.c >/dev/null
for e in binary ensemble glob jsonencode nshelper oo stdlib tclcompat tree initjimsh; do
    ./jimsh0 ./make-c-ext.tcl $e.tcl > _$e.c
done
