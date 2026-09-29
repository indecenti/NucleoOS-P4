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

[ -d lua-5.4.9 ] || tar xzf lua-5.4.9.tar.gz
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
