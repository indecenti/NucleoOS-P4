#!/bin/bash
# fetch.sh — download the pinned upstream source of Simon Tatham's Portable Puzzle Collection (MIT)
# into ports/_src (not committed). The tarball is upstream's release 20250730.a7c7826 as mirrored by
# Debian (sgt-puzzles orig tarball; chiark.greenend.org.uk only serves the moving "latest" build).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
src="$here/../_src"
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

PUZZLES_VER=20250730.a7c7826
get "https://deb.debian.org/debian/pool/main/s/sgt-puzzles/sgt-puzzles_$PUZZLES_VER.orig.tar.xz" \
    "sgt-puzzles_$PUZZLES_VER.orig.tar.xz" \
    875f78df7359135bac47de0bc4d2c05061719d144638c3373e0ade640ca0d73f
[ -d "sgt-puzzles-$PUZZLES_VER" ] || tar xJf "sgt-puzzles_$PUZZLES_VER.orig.tar.xz"
echo "sources ready in $src/sgt-puzzles-$PUZZLES_VER"
