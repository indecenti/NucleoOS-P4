#!/bin/bash
# fetch.sh — download the pinned upstream sources the ports build from (into ports/_src, which is
# not committed). Checksums pin the exact tarballs these ports were tested with.
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

get https://www.lua.org/ftp/lua-5.4.9.tar.gz lua-5.4.9.tar.gz \
    2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6
get https://github.com/quickjs-ng/quickjs/archive/refs/tags/v0.17.0.tar.gz quickjs-ng-0.17.0.tar.gz \
    559bc4c420475e55c7ab4510adbc562f55d7524d75e8e89d79ce4bb02f5687d9
get https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip sqlite-amalgamation-3530400.zip \
    1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d
# ubasic has no tagged releases: pinned to a commit, fetched as a GitHub archive tarball.
UBASIC_SHA=cc07193c231e21ecb418335aba5b199a08d4685c
get "https://github.com/adamdunkels/ubasic/archive/$UBASIC_SHA.tar.gz" "ubasic-$UBASIC_SHA.tar.gz" \
    52b6f8bdbd3caa579e150f1a41fe16261c7b64d35055779f73e45247a8254145
get https://github.com/DaveGamble/cJSON/archive/refs/tags/v1.7.19.tar.gz cJSON-1.7.19.tar.gz \
    7fa616e3046edfa7a28a32d5f9eacfd23f92900fe1f8ccd988c1662f30454562
get https://github.com/mity/md4c/archive/refs/tags/v0.6.0.tar.gz md4c-0.6.0.tar.gz \
    4d151298125a81da3b2efa2e0eed8bdb7a9318569804e4fa4d7a2375ab83ef70
get https://github.com/richgel999/miniz/archive/refs/tags/3.1.2.tar.gz miniz-3.1.2.tar.gz \
    98468f8924934b723276680f85238b6c78bf1f8b49b4459cc9b7214a20e2e9fb

# MojoZork (zlib) + Zork I-III story files (MIT, Microsoft 2025), each pinned to a commit.
MOJOZORK_SHA=ff7e00742a00acec8e175ddefb97520fb270df2d
get "https://github.com/icculus/mojozork/archive/$MOJOZORK_SHA.tar.gz" "mojozork-$MOJOZORK_SHA.tar.gz" \
    44d9512bde049318e7431d3494e986725cadac2ab6e0b8c82858645c723651a8
get https://raw.githubusercontent.com/historicalsource/zork1/97b7b3d68c075dd9af7da499c3e9690ada3471fd/COMPILED/zork1.z3 \
    zork1.z3 37084966477dff679282de42974b2077156b1bd68fad92a65d4ea94d8eb64d79
get https://raw.githubusercontent.com/historicalsource/zork2/3da9661098809788a99cef00f00c865c6c204f96/COMPILED/zork2.z3 \
    zork2.z3 3ae7d5558943e9721f3e4b273c8a7faec1a03a604e1ae4ee1cde472c21cb24ac
get https://raw.githubusercontent.com/historicalsource/zork3/3ec9ed412b5f3cafe65d83c727d07db1fe4a86a8/COMPILED/zork3.z3 \
    zork3.z3 b637a242865d059890184164ce8dec28554cc80901dcbf26c740b2d1ed0d4eb8

# Glulxe + CheapGlk (MIT, Andrew Plotkin): Glulx interpreter on stdio.
GLULXE_SHA=56ab8743bab565de307bd892c555d8d8897ed517
CHEAPGLK_SHA=14d8aaf6e4150669762bd4646a5368e75c1eeee6
get "https://github.com/erkyrath/glulxe/archive/$GLULXE_SHA.tar.gz" "glulxe-$GLULXE_SHA.tar.gz" \
    f1dcb430fafd451f68b14e62d55c26cf4fd35c49f09ed2b8a0bf0d3544d59e5d
get "https://github.com/erkyrath/cheapglk/archive/$CHEAPGLK_SHA.tar.gz" "cheapglk-$CHEAPGLK_SHA.tar.gz" \
    dbf925ef2ae208c8c44d857f73107d8fc3e30325b2fd17180a903438557a085a

[ -d lua-5.4.9 ] || tar xzf lua-5.4.9.tar.gz
[ -d "glulxe-$GLULXE_SHA" ] || tar xzf "glulxe-$GLULXE_SHA.tar.gz"
[ -d "cheapglk-$CHEAPGLK_SHA" ] || tar xzf "cheapglk-$CHEAPGLK_SHA.tar.gz"
[ -d "mojozork-$MOJOZORK_SHA" ] || tar xzf "mojozork-$MOJOZORK_SHA.tar.gz"
[ -d quickjs-0.17.0 ] || tar xzf quickjs-ng-0.17.0.tar.gz
[ -d sqlite-amalgamation-3530400 ] || unzip -q sqlite-amalgamation-3530400.zip
[ -d cJSON-1.7.19 ] || tar xzf cJSON-1.7.19.tar.gz
[ -d md4c-0.6.0 ] || tar xzf md4c-0.6.0.tar.gz
[ -d miniz-3.1.2 ] || tar xzf miniz-3.1.2.tar.gz
if [ ! -d "ubasic-$UBASIC_SHA" ]; then
    tar xzf "ubasic-$UBASIC_SHA.tar.gz"
    patch -p1 -d "ubasic-$UBASIC_SHA" < "$here/basic/ubasic.patch"
fi
echo "sources ready in $src"
