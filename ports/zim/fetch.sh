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

# (Windows tar cannot create the symlinks among zstd's CLI tests: unused here)
[ -d zstd-1.5.7 ] || tar xf zstd-1.5.7.tar.gz 2>/dev/null || [ -d zstd-1.5.7/lib ]
[ -d xz-embedded-20240322 ] || tar xf xz-embedded-20240322.tar.gz
