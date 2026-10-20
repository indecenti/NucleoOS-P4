#!/bin/bash
# fetch.sh — pinned upstream sources of the Libri app (docs/BOOKS_PLAN.md), into ports/books/_src
# (not committed). The same tarballs and checksums as ports/zim/fetch.sh and ports/fetch.sh.
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

# stb_truetype 1.26 (public domain / MIT): the fonts, rasterised at run time
get https://raw.githubusercontent.com/nothings/stb/2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_truetype.h \
    stb_truetype.h ecd30b05e0dd4fea3a13c26810dd9e1992dc379049482c393d5a19e6b5090aab
# LodePNG (zlib): PNG pictures; compiled as C
get https://raw.githubusercontent.com/lvandeve/lodepng/e584a8490db1b76b81bf298c11c09d3b55d9c270/lodepng.cpp \
    lodepng.cpp 6aec876971ec860527859458d3095220fc5ee1258ec37eb59e1e7dd9cf136a81
get https://raw.githubusercontent.com/lvandeve/lodepng/e584a8490db1b76b81bf298c11c09d3b55d9c270/lodepng.h \
    lodepng.h 9a1fca0238c38333bdc8545336961b73fd0b76957bbacf033186991be9d7cf85
[ -f lodepng.c ] || cp lodepng.cpp lodepng.c
# stb_image 2.30 (MIT or public domain): JPEG, GIF, BMP
get https://raw.githubusercontent.com/nothings/stb/2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_image.h \
    stb_image.h 594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3
# NanoSVG (zlib): SVG pictures and covers; the rasteriser gets the fix of the Biblioteca app
get https://raw.githubusercontent.com/memononen/nanosvg/239e102ec2c691f2902e20ace2ed36ee4a35cfe6/src/nanosvg.h \
    nanosvg.h e34fd5d084be106cea972d19ce5d27fd96d17ba89f8d06bdceee058420c8b2b0
get https://raw.githubusercontent.com/memononen/nanosvg/239e102ec2c691f2902e20ace2ed36ee4a35cfe6/src/nanosvgrast.h \
    nanosvgrast.h 79a9c5f4db19debf9f3a648a1589e96d92854f245a5cb4f3d823f263785234d8
patch_file="$here/../zim/biblioteca/nanosvgrast.patch"
if [ ! nanosvgrast_nv.h -nt "$patch_file" ] || [ ! nanosvgrast_nv.h -nt nanosvgrast.h ]; then
    cp nanosvgrast.h nanosvgrast_nv.h
    patch -s nanosvgrast_nv.h < "$patch_file"
fi
# libwebp 1.6.0 (BSD-3-Clause): WebP pictures (EPUB 3.3 core media type); only the decoder is built
get https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.6.0.tar.gz libwebp-1.6.0.tar.gz \
    e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564
[ -d libwebp-1.6.0 ] || tar xf libwebp-1.6.0.tar.gz
# TJpgDec R0.03 (ChaN, free use with the copyright notice kept): JPEG decoded at 1/2..1/8 of its size
# while it is read (comic pages); its configuration is ours (tjpgdcnf.h, copied over the archive's)
get http://elm-chan.org/fsw/tjpgd/arc/tjpgd3.zip tjpgd3.zip 052fe3efbc9a8be29f31597ad009c5b51a4f6905878eb28569e0ab3d46d0c013
if [ ! -f tjpgd/src/tjpgd.c ]; then mkdir -p tjpgd && (cd tjpgd && unzip -o -q ../tjpgd3.zip); fi
cp "$here/tjpgdcnf.h" tjpgd/src/tjpgdcnf.h
# miniz 3.1.2 (MIT): tinfl inflates the members of EPUB, CBZ and .fb2.zip files and the fonts
get https://github.com/richgel999/miniz/archive/refs/tags/3.1.2.tar.gz miniz-3.1.2.tar.gz \
    98468f8924934b723276680f85238b6c78bf1f8b49b4459cc9b7214a20e2e9fb
[ -d miniz-3.1.2 ] || tar xzf miniz-3.1.2.tar.gz
