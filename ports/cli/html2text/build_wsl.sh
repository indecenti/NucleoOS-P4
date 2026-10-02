#!/bin/bash
# build_wsl.sh SRC OUT — html2text 2.2.3 (C++) with the Linux wasi-sdk 34 in WSL: the Windows
# toolchain the other ports use has no libc++. No exceptions (wasi-sdk's libc++ is built without
# them; the bison parser then compiles with YY_EXCEPTIONS 0). Called by ports/cli/build.sh.
set -euo pipefail
src="$1"; out="$2"
W=/opt/wasi-sdk-34.0-x86_64-linux
cd "$src"
"$W/bin/clang++" --target=wasm32-wasip1 --sysroot="$W/share/wasi-sysroot" -O2 -fno-exceptions \
    -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS -DVERSION=2.2.3 -I. -w \
    -Wl,-z,stack-size=262144 -Wl,--strip-all -o "$out" \
    Area.cpp cmp_nocase.cpp format.cpp html2text.cpp HTMLControl.cpp html.cpp HTMLDriver.cpp \
    HTMLParser.cc iconvstream.cpp Properties.cpp sgml.cpp table.cpp \
    -lwasi-emulated-signal -lwasi-emulated-process-clocks
