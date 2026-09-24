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

[ -d lua-5.4.9 ] || tar xzf lua-5.4.9.tar.gz
[ -d quickjs-0.17.0 ] || tar xzf quickjs-ng-0.17.0.tar.gz
[ -d sqlite-amalgamation-3530400 ] || unzip -q sqlite-amalgamation-3530400.zip
echo "sources ready in $src"
