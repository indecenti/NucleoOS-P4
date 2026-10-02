#!/bin/bash
# fetch.sh — download the pinned upstream sources of the ports/cli programs (into ports/cli/_src,
# not committed) and apply the NucleoOS patches. Checksums pin the exact tarballs tested.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
src="$here/_src"
mkdir -p "$src"
cd "$src"

get() {   # url file sha256
    if [ ! -f "$2" ]; then
        echo "fetch $1"
        curl -sSfL -o "$2.part" "$1"
        mv "$2.part" "$2"
    fi
    echo "$3  $2" | sha256sum -c --quiet -
}
unpack() {   # tarball dir [patch]
    if [ ! -d "$2" ]; then
        # (bc ships symlinks among its locales/tests that Windows tar cannot create: unused)
        tar xf "$1" 2>/dev/null || [ -d "$2" ]   # gz or xz, by content
        if [ -n "${3:-}" ]; then
            # the patches are LF; some upstream files are CRLF (berry.c): normalise those first
            for f in $(sed -n 's|^+++ b/||p' "$here/$3"); do sed -i 's/\r$//' "$2/$f"; done
            patch -s -p1 -d "$2" < "$here/$3"
        fi
    fi
}

# Berry 1.1.0 (MIT) — scripting language for microcontrollers
get https://github.com/berry-lang/berry/archive/refs/tags/v1.1.0.tar.gz berry-1.1.0.tar.gz \
    b8eb94a44378ecd2f281cf7e244b8617b99f51d500638622b6126e60e730a693
# Wren 0.4.0 (MIT) — the VM only; ports/cli/wren/nv_wren_main.c is the front end
get https://github.com/wren-lang/wren/archive/refs/tags/0.4.0.tar.gz wren-0.4.0.tar.gz \
    23c0ddeb6c67a4ed9285bded49f7c91714922c2e7bb88f42428386bf1cf7b339
# Jim Tcl 0.84 (BSD-2-Clause)
get https://github.com/msteveb/jimtcl/archive/refs/tags/0.84.tar.gz jimtcl-0.84.tar.gz \
    435095b436b38b96dd85e8cda13878144813bf52066057f76368db178dd8fea2
# pForth 2.0.1 (0BSD)
get https://github.com/philburk/pforth/archive/refs/tags/v2.0.1.tar.gz pforth-2.0.1.tar.gz \
    f4c417d7d1f2c187716263484bdc534d3224b6d159e049d00828a89fa5d6894d
# bc 7.1.0 by Gavin D. Howard (BSD-2-Clause)
get https://github.com/gavinhoward/bc/archive/refs/tags/7.1.0.tar.gz bc-7.1.0.tar.gz \
    e30f44bcf6ea4f2a7a25de06267df17d0a9dfef8a9d4e59822d0b794bfc15b24
# FIGlet 2.2.5 (BSD-3-Clause)
get https://github.com/cmatsuoka/figlet/archive/refs/tags/2.2.5.tar.gz figlet-2.2.5.tar.gz \
    4d366c4a618ecdd6fdb81cde90edc54dbff9764efb635b3be47a929473f13930
# jq 1.8.2 (MIT) + its regex engine Oniguruma 6.9.10 (BSD-2-Clause)
get https://github.com/jqlang/jq/releases/download/jq-1.8.2/jq-1.8.2.tar.gz jq-1.8.2.tar.gz \
    71b8d6e8f5fe81f6c6d0d110e3892251f6ce76ed095abd315e26e6e1193af3af
get https://github.com/kkos/oniguruma/releases/download/v6.9.10/onig-6.9.10.tar.gz onig-6.9.10.tar.gz \
    2a5cfc5ae259e4e97f86b68dfffc152cdaffe94e2060b770cb827238d769fc05
# TinyScheme 1.42 (BSD-3-Clause) — the Scheme. s7 was dropped (2 MB of wasm code at -O2: over the
# app.wasm/app.aot caps, and ~10 setjmp re-entry sites), chibi-scheme too (its R7RS environment
# loads ~100 .sld/.scm files plus POSIX C modules at run time, ~0.3 s natively just to start).
get https://downloads.sourceforge.net/project/tinyscheme/tinyscheme/tinyscheme-1.42/tinyscheme-1.42.tar.gz \
    tinyscheme-1.42.tar.gz 17b0b1bffd22f3d49d5833e22a120b339039d2cfda0b46d6fc51dd2f01b407ad
# libqrencode 4.1.1 (LGPL-2.1-or-later) — the qrencode tool; qrencode.patch: Terminal output by default
get https://github.com/fukuchi/libqrencode/archive/refs/tags/v4.1.1.tar.gz qrencode-4.1.1.tar.gz     5385bc1b8c2f20f3b91d258bf8ccc8cf62023935df2d2676b5b67049f31a049c
# GNU units 2.24 (GPL-3.0-or-later) — the program and its unit database
get https://ftp.gnu.org/gnu/units/units-2.24.tar.gz units-2.24.tar.gz     1e502c4edfacf20b29284716c72e5ddb51a495a2365d7b03e7960494c4a0c902
# Eigenmath (George Weigt, BSD-2-Clause) — symbolic math; pinned commit of 2026-09-17
EIGEN=bf89927b523847bd0230a1a52d02265870ea8f04
get https://github.com/georgeweigt/eigenmath/archive/$EIGEN.tar.gz eigenmath-$EIGEN.tar.gz     7795321820013e1c7d202ebdb57c7c96ad69ceabf6e761c4ee36ca4142b58bac
# lowdown 3.0.1 (Kristaps Dzonsons, ISC) — Markdown to terminal text, HTML, man, LaTeX, ODT
get https://github.com/kristapsdz/lowdown/archive/refs/tags/VERSION_3_0_1.tar.gz lowdown-3.0.1.tar.gz     242a0e3d391c705d96d1a09d02978bd391af959cb42e05d7efd187ca23c98b24
# html2text 2.2.3 (GPL-2.0-or-later) — HTML to plain text; html2text.patch: no dup(), UTF-8 fallback, no overstrike
get https://github.com/grobian/html2text/archive/refs/tags/v2.2.3.tar.gz html2text-2.2.3.tar.gz     29e4b04e7cc7b9b6acb7db76edf4739d3a72a672f37452267e707d40249520ee
# dateutils 0.4.12 (Sebastian Freundt, BSD-3-Clause) — date arithmetic; one multi-call program here
get https://github.com/hroptatyr/dateutils/releases/download/v0.4.12/dateutils-0.4.12.tar.xz dateutils-0.4.12.tar.xz     1e0593116e1a229242255cf890f210cbe120e6f05e9d877faf8d85da675ade1a
# zstd 1.5.7 (BSD-3-Clause) + zlib 1.3.1 (Zlib) + xz 5.8.4 / liblzma (0BSD): one archiver for
# .zst .gz .xz; zstd.patch caps xz compression at preset 1 (the app has 16 MB)
get https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz zstd-1.5.7.tar.gz \
    eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
get https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz zlib-1.3.1.tar.gz \
    9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
get https://github.com/tukaani-project/xz/releases/download/v5.8.4/xz-5.8.4.tar.gz xz-5.8.4.tar.gz \
    0014c7886930454fe8bd4228665b51af55eeae560ea135c9c4cd33f55b2591d9
# PDFio 1.6.5 (Michael R Sweet, Apache-2.0) — PDF text, info and merge tools
get https://github.com/michaelrsweet/pdfio/releases/download/v1.6.5/pdfio-1.6.5.tar.gz pdfio-1.6.5.tar.gz \
    2b9e1db7c4a72cbc896098a6682a1e51fc2bfb979f00bec8bb515ee79c338084

unpack berry-1.1.0.tar.gz berry-1.1.0 berry/berry.patch
unpack wren-0.4.0.tar.gz wren-0.4.0
unpack jimtcl-0.84.tar.gz jimtcl-0.84 tcl/jimtcl.patch
unpack pforth-2.0.1.tar.gz pforth-2.0.1 pforth/pforth.patch
unpack bc-7.1.0.tar.gz bc-7.1.0 bc/bc.patch
unpack figlet-2.2.5.tar.gz figlet-2.2.5
unpack jq-1.8.2.tar.gz jq-1.8.2
unpack onig-6.9.10.tar.gz onig-6.9.10
unpack tinyscheme-1.42.tar.gz tinyscheme-1.42 scheme/tinyscheme.patch
unpack qrencode-4.1.1.tar.gz libqrencode-4.1.1 qrencode/qrencode.patch
unpack units-2.24.tar.gz units-2.24
unpack html2text-2.2.3.tar.gz html2text-2.2.3 html2text/html2text.patch
unpack dateutils-0.4.12.tar.xz dateutils-0.4.12 dateutils/dateutils.patch
unpack zstd-1.5.7.tar.gz zstd-1.5.7 zstd/zstd.patch
unpack zlib-1.3.1.tar.gz zlib-1.3.1
unpack xz-5.8.4.tar.gz xz-5.8.4
unpack pdfio-1.6.5.tar.gz pdfio-1.6.5
unpack lowdown-3.0.1.tar.gz lowdown-VERSION_3_0_1
unpack eigenmath-$EIGEN.tar.gz eigenmath-$EIGEN eigenmath/eigenmath.patch
echo "sources ready in $src"
