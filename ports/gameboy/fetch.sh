#!/bin/bash
# fetch.sh — pinned third-party inputs of the Game Boy apps, into ports/_src/gameboy (not committed).
#
#   Peanut-GB (MIT) emulator core + its MiniGB APU (MIT), pinned to a commit.
#   Bundled homebrew ROMs, each redistributable (licenses and sources in ports/gameboy/ROMS.md):
#     tobu.gb    Tobu Tobu Girl, Tangram Games — code MIT, assets CC BY 4.0
#     2048.gb    2048-gb, Sanqui — zlib
#     libbet.gb  Libbet and the Magic Floor, Damian Yerrick — zlib
#   Tobu Tobu Girl and 2048 come from the Homebrew Hub database (gbdev/database) at a pinned
#   commit, Libbet from its upstream GitHub release. The sha256 pins the exact bytes tested.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
dst="$here/../_src/gameboy"
mkdir -p "$dst"
cd "$dst"

get() {   # url file sha256
    if [ ! -f "$2" ]; then
        echo "fetch $1"
        curl -sSfL -o "$2.part" "$1"
        mv "$2.part" "$2"
    fi
    echo "$3  $2" | sha256sum -c --quiet -
}

PEANUT=https://raw.githubusercontent.com/deltabeard/Peanut-GB/d0bcca771c83a2638c93a9ae61f3d51f226dc905
get "$PEANUT/peanut_gb.h" peanut_gb.h \
    cf7777068492474d084f7dca0b00724d4dcdeab7f9a6c91a5d3a82e60d1cf728
get "$PEANUT/examples/sdl2/minigb_apu/minigb_apu.c" minigb_apu.c \
    cd173eb5979f1210de6eff6b75e76445db09643494f1cf471df43c5f223ec1f2
get "$PEANUT/examples/sdl2/minigb_apu/minigb_apu.h" minigb_apu.h \
    60d3c838f55653804d66f51d27a33cf14518498a68d1e0506625c67ca061fad1
get "$PEANUT/examples/sdl2/minigb_apu/LICENSE" minigb_apu.LICENSE \
    d286317975f85b441cedcd46acf4afa3c97fc1f52466f1fbd1c8dc158fde5472

GBDB=https://raw.githubusercontent.com/gbdev/database/50293559a496a3e20382fbf6a2e84b70ec622f88/entries
get "$GBDB/tobutobugirl/tobu.gb" tobu.gb \
    5d3871cae77db2287807e8914fd21f76203e73aa3fec20955489d45cb4571d8f
get "$GBDB/2048gb/2048.gb" 2048.gb \
    3ea2376b15b34bd26b10e6b31d2753bf8b086098dbdb5ee84d9c0f03d66d3d7f
get https://github.com/pinobatch/libbet/releases/download/v0.08/libbet.gb libbet.gb \
    3607412031c8287cf878299ce96e581e85b852dde703806343b95576fa3ff1a9
echo "gameboy sources ready in $dst"
