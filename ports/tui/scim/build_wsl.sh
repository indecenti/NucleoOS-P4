#!/bin/bash
# build_wsl.sh SRC PREFIX OUT — sc-im 0.8.5 as one WASI program (curses UI + --nocurses command
# mode) with xlsx/ods import (libxml2 + libzip), xls import (libxls) and xlsx export
# (libxlsxwriter), linked against the libraries ../ncurses and ../deps installed into PREFIX.
# Linux wasi-sdk 34, in WSL.
set -euo pipefail
src="$1"; prefix="$2"; out="$3"; here="$(cd "$(dirname "$0")" && pwd)"
W=/opt/wasi-sdk-34.0-x86_64-linux
B=/root/nv_tui/build/scim; rm -rf "$B"; mkdir -p "$B/o"
cp -r "$src/sc-im-0.8.5/src" "$B/src"
cd "$B/src"
# the grammar and the keyword tables generated from it (upstream Makefile rules)
bison -y -d gram.y 2>/dev/null && mv y.tab.c gram.c
sed -f eres.sed < gram.y > experres.h
sed -f sres.sed < gram.y > statres.h
cp doc "$B/doc" && (cd "$B" && xxd -i doc > "$B/scim_help.h")

CC=("$W/bin/clang" --target=wasm32-wasip1 --sysroot="$W/share/wasi-sysroot" -Os -w
    -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS -D_WASI_EMULATED_GETPID
    -I"$here/include" -I"$here/../compat" -I"$prefix/include" -I"$prefix/include/ncursesw"
    -I"$prefix/include/libxml2" -I"$B" -I. -include nv_posix.h
    -DNCURSES -D_XOPEN_SOURCE_EXTENDED -D_GNU_SOURCE '-DSNAME="sc-im"' '-DHELP_PATH="/"'
    '-DLIBDIR="/"' '-DDFLT_PAGER="less"' '-DDFLT_EDITOR="edit"' -DUSECOLORS
    '-DHISTORY_FILE="sc-iminfo"' '-DHISTORY_DIR=".cache"' '-DCONFIG_FILE="scimrc"'
    '-DCONFIG_DIR=".config/sc-im"' '-DINS_HISTORY_FILE="sc-iminfo"' -DUNDO -DMAXROWS=65536
    -DUSELOCALE -DMOUSE -DAUTOBACKUP -DNO_WORDEXP -DODS -DXLSX -DXLSX_EXPORT -DXLS
    '-DDEFAULT_COPY_TO_CLIPBOARD_CMD=""' '-DDEFAULT_PASTE_FROM_CLIPBOARD_CMD=""'
    '-DDEFAULT_OPEN_FILE_UNDER_CURSOR_CMD=""')
STRIP=(-Wl,--strip-all); [ -n "${NV_KEEP_NAMES:-}" ] && STRIP=()   # NV_KEEP_NAMES=1: function names kept
# stdin and TIOCGWINSZ through ../compat/nv_tty.c (in-band resize reports). wasm-ld does not
# pull the real read/poll/select/ioctl out of libc for the __real_ calls by itself (they became
# calls to function 0, an invalid module): -u makes it.
TTY_LD=(-Wl,--wrap=read,--wrap=poll,--wrap=select,--wrap=ioctl -Wl,-u,read,-u,poll,-u,select,-u,ioctl)
# libxlsxwriter's in-memory parts through ../compat/nv_memstream.c: a failed write is an error
MS_LD=(-Wl,--wrap=open_memstream)
# local time (NUCLEO_UTC_OFFSET) through ../compat/nv_localtime.c
LT_LD=(-Wl,--wrap=localtime,--wrap=localtime_r,--wrap=mktime)
objs=()
for f in *.c actions/*.c formats/*.c cmds/*.c utils/*.c; do
    o="$B/o/$(echo "$f" | tr / _ | sed 's/\.c$/.o/')"
    "${CC[@]}" -c -o "$o" "$f" || { echo "FAILED: $f" >&2; exit 1; }
    objs+=("$o")
done
"${CC[@]}" -c -o "$B/o/nv_scim_shim.o" "$here/nv_scim_shim.c"
"${CC[@]}" -c -o "$B/o/nv_posix.o" "$here/../compat/nv_posix.c"
"${CC[@]}" -c -o "$B/o/nv_memstream.o" "$here/../compat/nv_memstream.c"
"${CC[@]}" -c -o "$B/o/nv_localtime.o" "$here/../compat/nv_localtime.c"
"${CC[@]}" -Wl,-z,stack-size=262144 "${STRIP[@]}" "${TTY_LD[@]}" "${MS_LD[@]}" "${LT_LD[@]}" \
    -o "$out" "${objs[@]}" "$B/o/nv_scim_shim.o" "$B/o/nv_posix.o" "$B/o/nv_memstream.o" \
    "$B/o/nv_localtime.o" -L"$prefix/lib" -lxlsxwriter -lxlsreader -lxml2 -lzip -lz -lncursesw \
    -lnvtty -lm -lwasi-emulated-signal -lwasi-emulated-process-clocks -lwasi-emulated-getpid
echo "  sc-im: $(stat -c %s "$out") bytes"
