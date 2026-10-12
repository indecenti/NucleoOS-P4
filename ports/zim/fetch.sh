#!/bin/bash
# fetch.sh — pinned upstream sources for the ZIM reader (docs/ZIM_READER_PLAN.md), into
# ports/zim/_src (not committed). Same tarballs and checksums as ports/cli/fetch.sh.
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

# zstd 1.5.7 (BSD-3-Clause) — only the decoder is used
get https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz zstd-1.5.7.tar.gz \
    eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3

# XZ Embedded 2024-03-22 (0BSD) — xz clusters of ZIM 5 files, single-call mode (no dictionary)
get https://github.com/tukaani-project/xz-embedded/archive/refs/tags/v20240322.tar.gz xz-embedded-20240322.tar.gz \
    82ed01b76a7f1dadac4fe24ce0fe46493662af348a065bf9967417171b6be3cc

# stb_truetype 1.26 (public domain / MIT) — the Biblioteca app rasterises its fonts with it
get https://raw.githubusercontent.com/nothings/stb/2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_truetype.h     stb_truetype.h ecd30b05e0dd4fea3a13c26810dd9e1992dc379049482c393d5a19e6b5090aab

# LodePNG (zlib) — the pictures of the collections in the Biblioteca app; compiled as C
get https://raw.githubusercontent.com/lvandeve/lodepng/e584a8490db1b76b81bf298c11c09d3b55d9c270/lodepng.cpp     lodepng.cpp 6aec876971ec860527859458d3095220fc5ee1258ec37eb59e1e7dd9cf136a81
get https://raw.githubusercontent.com/lvandeve/lodepng/e584a8490db1b76b81bf298c11c09d3b55d9c270/lodepng.h     lodepng.h 9a1fca0238c38333bdc8545336961b73fd0b76957bbacf033186991be9d7cf85
[ -f lodepng.c ] || cp lodepng.cpp lodepng.c

# The pictures of the articles in the Biblioteca app (F4): libwebp 1.6.0 (BSD-3-Clause), only its
# decoder is built; stb_image 2.30 (MIT or public domain) for JPEG and GIF; NanoSVG (zlib) for the
# SVG images, the formulas of Wikipedia above all.
get https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.6.0.tar.gz libwebp-1.6.0.tar.gz \
    e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564
get https://raw.githubusercontent.com/nothings/stb/2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_image.h \
    stb_image.h 594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3
get https://raw.githubusercontent.com/memononen/nanosvg/239e102ec2c691f2902e20ace2ed36ee4a35cfe6/src/nanosvg.h \
    nanosvg.h e34fd5d084be106cea972d19ce5d27fd96d17ba89f8d06bdceee058420c8b2b0
get https://raw.githubusercontent.com/memononen/nanosvg/239e102ec2c691f2902e20ace2ed36ee4a35cfe6/src/nanosvgrast.h \
    nanosvgrast.h 79a9c5f4db19debf9f3a648a1589e96d92854f245a5cb4f3d823f263785234d8
[ -d libwebp-1.6.0 ] || tar xf libwebp-1.6.0.tar.gz
# NanoSVG's rasteriser with a fix for an undefined float-to-int overflow (found by the fuzzer of
# the Biblioteca app): a patched copy, the download itself stays as its checksum says
if [ ! nanosvgrast_nv.h -nt "$here/biblioteca/nanosvgrast.patch" ] || [ ! nanosvgrast_nv.h -nt nanosvgrast.h ]; then
    cp nanosvgrast.h nanosvgrast_nv.h
    patch -s nanosvgrast_nv.h < "$here/biblioteca/nanosvgrast.patch"
fi

# (Windows tar cannot create the symlinks among zstd's CLI tests: unused here)
[ -d zstd-1.5.7 ] || tar xf zstd-1.5.7.tar.gz 2>/dev/null || [ -d zstd-1.5.7/lib ]
[ -d xz-embedded-20240322 ] || tar xf xz-embedded-20240322.tar.gz
