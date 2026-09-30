#!/bin/bash
# fetch.sh — pinned third-party inputs of the Doom app, into ports/_src/doom (not committed).
#
#   doomgeneric (GPLv2)            the engine: Chocolate Doom-derived, portable to 5 hooks
#   chocolate-doom (GPLv2)         OPL music player (i_oplmusic.c), MIDI parser, OPL callback queue
#   rp2040-doom emu8950 (MIT)      fast OPL2 FM emulator (emu8950 + Graham Sanderson's OPL2 waveforms)
#   Freedoom 0.13.0 (BSD-3)        the free IWADs: only for the PC harness and the hosted download
#                                  (tools in ports/doom/README.md), never compiled in.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
dst="$here/../_src/doom"
mkdir -p "$dst"
cd "$dst"

get() {   # url file sha256
    if [ ! -f "$2" ]; then
        echo "fetch $1"
        mkdir -p "$(dirname "$2")"
        curl -sSfL -o "$2.part" "$1"
        mv "$2.part" "$2"
    fi
    echo "$3  $2" | sha256sum -c --quiet -
}

DG_SHA=dcb7a8dbc7a16ce3dda29382ac9aae9d77d21284
if [ ! -f doomgeneric/.pinned ] || [ "$(cat doomgeneric/.pinned)" != "$DG_SHA" ]; then
    echo "fetch doomgeneric @ $DG_SHA"
    rm -rf doomgeneric dg.git
    git init -q dg.git
    git -C dg.git fetch -q --depth 1 https://github.com/ozkl/doomgeneric.git "$DG_SHA"
    git -C dg.git -c advice.detachedHead=false checkout -q FETCH_HEAD
    mv dg.git/doomgeneric doomgeneric
    cp dg.git/LICENSE doomgeneric/LICENSE 2>/dev/null || true
    rm -rf dg.git
    echo "$DG_SHA" > doomgeneric/.pinned
fi

CHOCO=https://raw.githubusercontent.com/chocolate-doom/chocolate-doom/895f581c5d91497bdda0516612da803fe5843e28
get "$CHOCO/opl/opl.h" choco/opl.h 82133cff253a2fcce4feee87e0820bf962e7fced247d9920a2cda74196d68315
get "$CHOCO/opl/opl_queue.c" choco/opl_queue.c 6bad487968bec7cd66732cc3751071e57c228a558496c2c0d5d23221b3534664
get "$CHOCO/opl/opl_queue.h" choco/opl_queue.h 1fdc0259467f6801457ba1adbd5943eb335ee6ba34a7944808deeaf24952ba49
get "$CHOCO/src/i_oplmusic.c" choco/i_oplmusic.c 3e0f161fda6f17ed104f3381d0b1eb9c8b060797e8393cb2c00649d53bedbb7a
get "$CHOCO/src/midifile.c" choco/midifile.c 17c83a9b306c919334dda009f966af0f2d6daeb5687419b3099c5cb821ac4609
get "$CHOCO/src/midifile.h" choco/midifile.h 6df63dc0eb22cdb4082caf342ab68bc24d1c8b3e21de22396d8cc806a6d7d13b

RP=https://raw.githubusercontent.com/kilograham/rp2040-doom/29a453c980918a03e40fc8b69b024e7a3bdb5dc2
get "$RP/opl/emu8950.c" emu8950/emu8950.c da530f817360d3a2b35dc0f2a85799726121b8bdef6e667c10371eff3f0ee9f0
get "$RP/opl/emu8950.h" emu8950/emu8950.h 6b9d3366e941741246d239a5607df8d4bb3d29a453787392eed4a66ac090be72
get "$RP/opl/slot_render.h" emu8950/slot_render.h e0c3b03bfa5e02fc674c170ab9ab5882c86aa84af822454323140c3bce6c9cf9

get https://github.com/freedoom/freedoom/releases/download/v0.13.0/freedoom-0.13.0.zip freedoom-0.13.0.zip \
    3f9b264f3e3ce503b4fb7f6bdcb1f419d93c7b546f4df3e874dd878db9688f59
if [ ! -f freedoom/freedoom1.wad ]; then
    mkdir -p freedoom
    unzip -q -j -o freedoom-0.13.0.zip 'freedoom-0.13.0/freedoom1.wad' 'freedoom-0.13.0/freedoom2.wad' \
        'freedoom-0.13.0/COPYING.txt' -d freedoom
fi
echo "doom sources ready in $dst"
