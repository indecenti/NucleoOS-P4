#!/bin/bash
# build.sh — Arduboy games as NucleoOS apps (apps/ab-<slug>): an Arduboy2-compatible shim + the
# pinned open-source sketches listed in games.py. Runs the work in WSL (abtool.py).
#
#   bash ports/arduboy/build.sh libs              patch the pinned Arduboy libraries + build the shim
#   bash ports/arduboy/build.sh build [slug ...]  build apps (all of games.py by default): wasm, AOT,
#                                                 PC harness run (1200 frames), icon, shots, manifest,
#                                                 GUIDE.md/GUIDE.en.md
#   bash ports/arduboy/build.sh test [slug ...]   PC harness only
#   bash ports/arduboy/build.sh wamr [slug ...]   run the built app.wasm under WAMR (interp + x86-64 AOT)
#   bash ports/arduboy/build.sh pin <slug> [ref]  pin a game repository (commit + sha256) into fetch.sh
#   bash ports/arduboy/build.sh docs              GAMES.md + catalog_entries.json
#
# Needs in WSL (Ubuntu-24.04): wasi-sdk 34 in /opt/wasi-sdk-34.0-x86_64-linux, wamrc
# (/root/wamrc-build/wamrc), gcc/g++, python3 + Pillow, curl; for "wamr" the libiwasm.a of
# ports/host/build.sh. Harness output: ports/_src/arduboy/gen/<slug>/test/*.ppm|pgm.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
if [ "$(uname -s)" = Linux ]; then
    exec python3 "$here/abtool.py" "$@"
fi
win="$(cygpath -m "$here")"                                     # D:/NucleoV2/ports/arduboy
lin="/mnt/$(echo "${win:0:1}" | tr 'A-Z' 'a-z')${win:2}"
MSYS_NO_PATHCONV=1 exec wsl.exe -d "${DISTRO:-Ubuntu-24.04}" -- python3 "$lin/abtool.py" "$@"
