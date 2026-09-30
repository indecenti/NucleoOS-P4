#!/bin/bash
# fetch.sh — download the CHIP-8 Community Archive (John Earnest, CC0) the chip8 app bundles its
# games from, into ports/_src (not committed). Pinned to a commit, checked by sha256.
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

# chip8Archive has no releases: pinned to a commit, fetched as a GitHub archive tarball.
C8A_SHA=761e3ffc63f43e6a849e80714122feff2afca208
get "https://github.com/JohnEarnest/chip8Archive/archive/$C8A_SHA.tar.gz" "chip8Archive-$C8A_SHA.tar.gz" \
    96449b34bd88b3347c7a1b4aac6932d3d3d18685f8c7661787e767075ac1c42f
[ -d "chip8Archive-$C8A_SHA" ] || tar xzf "chip8Archive-$C8A_SHA.tar.gz"
echo "sources ready in $src/chip8Archive-$C8A_SHA"
