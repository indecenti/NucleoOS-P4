#!/bin/bash
# build.sh — MicroPython for NucleoOS (apps/python). Linux or WSL; everything it needs is pinned
# and fetched into ports/_src (not committed):
#
#   bash ports/micropython/build.sh            apps/python/app.wasm (+ icon.z)
#   bash ports/micropython/build.sh test       upstream test suite: native build, then the wasm
#                                              build under the PC host (ports/host, WAMR) with a
#                                              GC-stress variant (collects every 4 blocks)
#
# The wasm goes through Binaryen: --flatten --spill-pointers copies every pointer that is live
# across a call to the C shadow stack, which the garbage collector scans (README.md).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
S="$here/../_src"
mkdir -p "$S"
TOP="$S/micropython-1.26.1"

tool() {   # dir url sha256
    [ -d "$S/$1" ] && return
    curl -sSfL -o "$S/$1.tgz" "$2"
    echo "$3  $S/$1.tgz" | sha256sum -c --quiet -
    tar xzf "$S/$1.tgz" -C "$S"
}
# MicroPython (MIT), its core patched for the host's protected call (README.md)
if [ ! -d "$TOP" ]; then
    f="$S/micropython-1.26.1.tar.xz"
    [ -f "$f" ] || curl -sSfL -o "$f" https://github.com/micropython/micropython/releases/download/v1.26.1/micropython-1.26.1.tar.xz
    echo "12be6514df6272c0fcb328122b534af6b12abdd52435c19f40ee1707cc43ac98  $f" | sha256sum -c --quiet -
    tar xJf "$f" -C "$S"
    patch -s -p1 -d "$TOP" < "$here/micropython.patch"
fi
WASI_SDK="${WASI_SDK:-$S/wasi-sdk-27.0-x86_64-linux}"
[ -n "${WASI_SDK_SET:-}" ] || tool wasi-sdk-27.0-x86_64-linux \
    https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-27/wasi-sdk-27.0-x86_64-linux.tar.gz \
    b7d4d944c88503e4f21d84af07ac293e3440b1b6210bfd7fe78e0afd92c23bc2
tool binaryen-version_123 \
    https://github.com/WebAssembly/binaryen/releases/download/version_123/binaryen-version_123-x86_64-linux.tar.gz \
    e959f2170af4c20c552e9de3a0253704d6a9d2766e8fdb88e4d6ac4bae9388fe
WASM_OPT="$S/binaryen-version_123/bin/wasm-opt"
FEATURES=(--enable-bulk-memory --enable-sign-ext --enable-mutable-globals
          --enable-nontrapping-float-to-int --enable-reference-types --enable-multivalue)

# mpy-cross (host compiler for the frozen modules), built alone: make would pass it our variables
[ -x "$TOP/mpy-cross/build/mpy-cross" ] || env -u MAKEFLAGS make -s -C "$TOP/mpy-cross" -j"$(nproc)" >/dev/null

wasm() {   # build-dir out [COPT]
    make -s -C "$here" TOP="$TOP" WASI_SDK="$WASI_SDK" BUILD="$1" COPT="${3:-}" -j"$(nproc)"
    "$WASM_OPT" "${FEATURES[@]}" "$here/$1/app.wasm" --flatten --spill-pointers -O2 -o "$2"
}

if [ "${1:-}" = test ]; then
    make -s -C "$here" TOP="$TOP" NATIVE=1 FROZEN_MANIFEST= -j"$(nproc)"   # 64-bit: no frozen mpz
    ex=(-e tls -e thread -e socket -e asyncio -e select -e ssl -e unittest)
    dirs=(-d basics micropython float misc import extmod stress)
    (cd "$TOP/tests" && MICROPY_MICROPYTHON="$here/build-native/micropython" \
        python3 run-tests.py -j"$(nproc)" "${ex[@]}" "${dirs[@]}" | grep -E "tests (performed|passed|failed)") || true
    NVHOST="${NVHOST:-/root/nvhost}"
    [ -x "$NVHOST" ] || { echo "no $NVHOST: build it with ports/host/build.sh" >&2; exit 1; }
    wasm build-stress "$here/build-stress/app.opt.wasm" -DNV_GC_STRESS=4
    cat > "$here/build-stress/run.sh" <<RUN
#!/bin/bash
a=(); for x in "\$@"; do if [ -e "\$x" ]; then a+=("\$(realpath "\$x")"); else a+=("\$x"); fi; done
exec "$NVHOST" --dir=/::/ --stack=256 --mem=64 "$here/build-stress/app.opt.wasm" "\${a[@]}"
RUN
    chmod +x "$here/build-stress/run.sh"
    (cd "$TOP/tests" && MICROPY_MICROPYTHON="$here/build-stress/run.sh" \
        python3 run-tests.py -j"$(nproc)" "${ex[@]}" "${dirs[@]}" | grep -E "tests (performed|passed|failed)") || true
    exit 0
fi

out="$root/apps/python"
wasm build-wasi "$out/app.wasm"
n=$(stat -c %s "$out/app.wasm")
[ "$n" -le 2097152 ] || { echo "app.wasm $n bytes > 2 MB device cap" >&2; exit 1; }
echo "  python/app.wasm  $n bytes"
python3 "$here/../make_icon.py" "$out/icon.z" "Py" "#3770a0" "#ffd43b"
