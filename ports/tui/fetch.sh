#!/bin/bash
# fetch.sh — download the pinned upstream sources of the ports/tui programs (into ports/tui/_src,
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
        tar xf "$1" 2>/dev/null || [ -d "$2" ]   # gz or xz, by content
        if [ -n "${3:-}" ]; then
            for f in $(sed -n 's|^+++ b/||p' "$here/$3"); do sed -i 's/\r$//' "$2/$f"; done
            patch -s -p1 -d "$2" < "$here/$3"
        fi
    fi
}

# ncurses 6.5 (MIT-style "X11" license) — curses for every full-screen program
get https://ftp.gnu.org/gnu/ncurses/ncurses-6.5.tar.gz ncurses-6.5.tar.gz \
    136d91bc269a9a5785e5f9e980bc76ab57428f604ce3e5a5a90cebc767971cc6
# sc-im 0.8.5 (BSD-4-Clause) — spreadsheet; xlsx/ods import needs libxml2 + libzip, xlsx export
# libxlsxwriter, all three on zlib; .xls import libxls
get https://github.com/andmarti1424/sc-im/archive/refs/tags/v0.8.5.tar.gz sc-im-0.8.5.tar.gz \
    49adb76fc55bc3e6ea8ee414f41428db4aef947e247718d9210be8d14a6524bd
get https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz zlib-1.3.1.tar.gz \
    9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
get https://github.com/nih-at/libzip/releases/download/v1.11.4/libzip-1.11.4.tar.gz libzip-1.11.4.tar.gz \
    82e9f2f2421f9d7c2466bbc3173cd09595a88ea37db0d559a9d0a2dc60dc722e
get https://download.gnome.org/sources/libxml2/2.14/libxml2-2.14.6.tar.xz libxml2-2.14.6.tar.xz \
    7ce458a0affeb83f0b55f1f4f9e0e55735dbfc1a9de124ee86fb4a66b597203a
get https://github.com/jmcnamara/libxlsxwriter/archive/refs/tags/v1.2.4.tar.gz libxlsxwriter-1.2.4.tar.gz \
    e0db59fc248a5ffa465a05ea83a9d466d4bca0e53ab42771515d4ebb467a41c1
get https://github.com/libxls/libxls/releases/download/v1.6.3/libxls-1.6.3.tar.gz libxls-1.6.3.tar.gz \
    b2fb836ea0b5253a352fb5ca55742e29f06f94f9421c5b8eeccef2e5d43f622c
# WordGrinder 0.9 (MIT; Luau MIT, cmark BSD-2, fmt MIT, MiniZip zlib) — word processor. 0.9 is
# not tagged upstream: pinned to the 2026-06-09 head of the main branch.
WG=25cdfc66a28a82e0ea18f965045bae79f02d7dbb
get https://github.com/davidgiven/wordgrinder/archive/$WG.tar.gz wordgrinder-$WG.tar.gz \
    9590a4b46cbf291a4d59e15b3821f580dd93c54111a06814ce69cc7745047911

unpack ncurses-6.5.tar.gz ncurses-6.5
unpack sc-im-0.8.5.tar.gz sc-im-0.8.5 scim/scim.patch
unpack zlib-1.3.1.tar.gz zlib-1.3.1
unpack libzip-1.11.4.tar.gz libzip-1.11.4
unpack libxml2-2.14.6.tar.xz libxml2-2.14.6
unpack libxlsxwriter-1.2.4.tar.gz libxlsxwriter-1.2.4 deps/libxlsxwriter.patch
unpack libxls-1.6.3.tar.gz libxls-1.6.3
if [ ! -d wordgrinder ]; then
    unpack wordgrinder-$WG.tar.gz wordgrinder-$WG wordgrinder/wordgrinder.patch
    mv wordgrinder-$WG wordgrinder
fi
