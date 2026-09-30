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

# DeHackEd patch loader (Chocolate Doom, same commit): new monsters, weapons, texts of the PWADs
get "$CHOCO/src/deh_defs.h" choco/deh/deh_defs.h 45488839a600e6f7514acd6fbc038cbad575edf465e1900e7e27850a7a00db37
get "$CHOCO/src/deh_io.c" choco/deh/deh_io.c c2532f1726ebdee1746971134cdd1e72a558238f046e3d676b28f86301aa0bb5
get "$CHOCO/src/deh_io.h" choco/deh/deh_io.h 4fab019c399b633681019de3dc21b3f930868a6ea78516a8f9cdf0f8079e82aa
get "$CHOCO/src/deh_main.c" choco/deh/deh_main.c b6a5f3005412997179690a5919b72662efcad55ad3ea9ac1e3e13ef9a295d092
get "$CHOCO/src/deh_main.h" choco/deh/deh_main.h c3608548c5809ee23c135e718935f6a3a12c5c7f80c1f7194c284154cee7435c
get "$CHOCO/src/deh_mapping.c" choco/deh/deh_mapping.c 8eb1153221a608178f3a48b304a434df62cfe946a1032eb2bafeae409736c9ab
get "$CHOCO/src/deh_mapping.h" choco/deh/deh_mapping.h 2f31787a36fa67acb3d589f2b8a9bc192c8986bc44ab13da45d389e1d5c8bac2
get "$CHOCO/src/deh_str.c" choco/deh/deh_str.c 8c12e341e577fb101a11e6de680b8ae277e401f2cbe3c5f838af1ab78bba40f0
get "$CHOCO/src/deh_str.h" choco/deh/deh_str.h e8e8c414ce1761da10cdc7b127bc0b35855a035201e0fd008d5601bc4d63bd2f
get "$CHOCO/src/deh_text.c" choco/deh/deh_text.c aa5de4717efa89a0cafc525da21fed49788a020a7d7d19f4cef78bcac1aaaff6
get "$CHOCO/src/doom/deh_ammo.c" choco/deh/deh_ammo.c 1e456ee7e1619e94e7ffadc7a0df51dab449d1fc5d7075c32eccfc0e057c411f
get "$CHOCO/src/doom/deh_bexstr.c" choco/deh/deh_bexstr.c c0204b700ad558c48ccc5fd957f4ff9fafb3c02995f42f2e24f50dfd15b7f930
get "$CHOCO/src/doom/deh_cheat.c" choco/deh/deh_cheat.c 7e71c8dba50849b3aa456977cbec3686d097d1503548a07238df7283bc7fa135
get "$CHOCO/src/doom/deh_doom.c" choco/deh/deh_doom.c a219400346437323301340982c5446d19559bb4c5622dafb89e9950dd07e61fe
get "$CHOCO/src/doom/deh_frame.c" choco/deh/deh_frame.c 1c41e9db18493de9abd598b59fda0ace8e8af9c02dd196ebec4f1f203d04710f
get "$CHOCO/src/doom/deh_misc.c" choco/deh/deh_misc.c 176e4656eda8a5ceab296613e0f6512c9103cb4d1599ef425b776e87f5359256
get "$CHOCO/src/doom/deh_misc.h" choco/deh/deh_misc.h e3512f25913aba23a46a36e2ec09c4ee924d70163bb77b1cd80ccc9f37fd0c71
get "$CHOCO/src/doom/deh_ptr.c" choco/deh/deh_ptr.c 36acde57c6147375d9168cfc506605e65261ac1cc0c2403fc400992a20dcfd71
get "$CHOCO/src/doom/deh_sound.c" choco/deh/deh_sound.c e92e89ce81a808852378048ee1b6c9721c41eba45aad94462762516c6f7c9aec
get "$CHOCO/src/doom/deh_thing.c" choco/deh/deh_thing.c 9c2c067fcfed28eb783f14a75581a9d79350b5f7132296b17a3ee072b757c004
get "$CHOCO/src/doom/deh_weapon.c" choco/deh/deh_weapon.c 78d0cab702ad249e4cc144dcdf8aa8e7e1c39bf7451d95bdbba94ee777b503b8

# DeuTex/NWT-style merge of PWAD sprites and flats (Chocolate Doom -merge)
get "$CHOCO/src/w_merge.c" choco/deh/w_merge.c b8700dbdbaa2a97e25affaef8e4171abc7c51a1c64cd94adb7acb01926e41784

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
