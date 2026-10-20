#!/bin/bash
# build_wsl.sh SRC PREFIX — ncurses 6.5 (wide characters) as a static WASI library for the
# full-screen ports, with the Linux wasi-sdk 34 in WSL. No terminfo database on the device: the
# xterm-256color entry (what the NucleoOS Terminal emulates, libvterm) and two plain fallbacks
# are compiled in. termios comes from ../compat (WASI has none). Installs headers, libncursesw.a
# and libnvtty.a (../compat) into PREFIX; ncurses is skipped when PREFIX already holds it.
set -euo pipefail
src="$1"; prefix="$2"; here="$(cd "$(dirname "$0")" && pwd)"
W=/opt/wasi-sdk-34.0-x86_64-linux
CC="$W/bin/clang --target=wasm32-wasip1 --sysroot=$W/share/wasi-sysroot"
CPPFLAGS="-I$here/../compat -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS -D_WASI_EMULATED_GETPID"
if [ -f "$prefix/lib/libncursesw.a" ] && [ -f "$prefix/include/ncursesw/curses.h" ]; then
    # ncurses is there: only refresh libnvtty (cheap, and the compat sources change more often)
    T=$(mktemp -d)
    $CC $CPPFLAGS -Os -c -o "$T/nv_termios.o" "$here/../compat/nv_termios.c"
    $CC $CPPFLAGS -Os -c -o "$T/nv_tty.o" "$here/../compat/nv_tty.c"
    rm -f "$prefix/lib/libnvtty.a"
    "$W/bin/llvm-ar" rcs "$prefix/lib/libnvtty.a" "$T/nv_termios.o" "$T/nv_tty.o"
    rm -rf "$T"
    exit 0
fi
B=/root/nv_tui/build/ncurses; rm -rf "$B"; mkdir -p "$B"
cp -r "$src/ncurses-6.5" "$B/"
# libnvtty first: termios (compat/nv_termios.c) so configure finds tcgetattr, or term.h falls
# back to sgtty; and the stdin side (compat/nv_tty.c: in-band resize reports), which a program
# gets by linking with -Wl,--wrap=read,--wrap=poll,--wrap=select,--wrap=ioctl (TIOCGWINSZ, in
# termios.h, also makes configure enable curses' resize handling: KEY_RESIZE, resizeterm).
mkdir -p "$prefix/lib" "$prefix/include"
$CC $CPPFLAGS -Os -c -o "$B/nv_termios.o" "$here/../compat/nv_termios.c"
$CC $CPPFLAGS -Os -c -o "$B/nv_tty.o" "$here/../compat/nv_tty.c"
"$W/bin/llvm-ar" rcs "$prefix/lib/libnvtty.a" "$B/nv_termios.o" "$B/nv_tty.o"
cp "$here/../compat/termios.h" "$prefix/include/"
cd "$B/ncurses-6.5"
# The compiled-in entries keep their extended capabilities (infocmp -x): the modified keys
# (Ctrl/Shift/Alt + arrows, Home, End... kUP5, kEND2) a USB or KeyDeck keyboard sends.
sed -i 's/"$infocmp_path" -E "$x"/"$infocmp_path" -x -E "$x"/; s/"$infocmp_path" -e "$x"/"$infocmp_path" -x -e "$x"/' \
    ncurses/tinfo/MKfallback.sh
./configure --host=wasm32-unknown-wasi --build=x86_64-pc-linux-gnu --prefix="$prefix" \
    CC="$CC" CPPFLAGS="$CPPFLAGS" CFLAGS="-Os -w" AR="$W/bin/llvm-ar" RANLIB="$W/bin/llvm-ranlib" \
    LDFLAGS="-L$prefix/lib" \
    LIBS="-lnvtty -lwasi-emulated-signal -lwasi-emulated-process-clocks -lwasi-emulated-getpid" \
    --without-shared --with-normal --without-debug --without-profile --without-cxx \
    --without-cxx-binding --without-ada --without-progs --without-tests --without-manpages \
    --without-pkg-config --disable-pc-files --enable-widec --disable-database \
    --with-fallbacks=xterm-256color,xterm,vt100 --disable-home-terminfo --disable-termcap \
    --without-gpm --without-dlsym --without-pthread --disable-stripping --with-build-cc=gcc \
    --with-tic-path=/usr/bin/tic --with-infocmp-path=/usr/bin/infocmp \
    --disable-root-environ --disable-root-access --enable-sigwinch \
    > "$B/configure.log" 2>&1 || { tail -30 "$B/configure.log"; exit 1; }
make -j8 libs > "$B/make.log" 2>&1 || { grep -m20 -E "error|Error" "$B/make.log"; tail -20 "$B/make.log"; exit 1; }
make install.libs install.includes > "$B/install.log" 2>&1 || { tail -20 "$B/install.log"; exit 1; }
echo "  ncursesw: $(stat -c %s "$prefix/lib/libncursesw.a") bytes"
