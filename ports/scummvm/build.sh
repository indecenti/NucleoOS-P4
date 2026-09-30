#!/bin/bash
# build.sh — ScummVM 2.9.1 as a NucleoOS WASI app (backends/platform/nucleo, see backend/).
#
#   bash ports/scummvm/build.sh [engine[,engine...]] [app-id]     (inside WSL)
#   default: the eight engines of the freeware games, app "scummvm"
#
# Needs, in WSL: the wasi-sdk 34 Linux toolchain in /opt/wasi-sdk-34.0-x86_64-linux (its clang,
# wasm-ld and libc++ sysroot) and the ScummVM source (ports/_src/scummvm-2.9.1.tar.gz, extracted
# into ~/svm on first run: the build is much faster on the WSL filesystem than on /mnt/d).
# Output: apps/<app-id>/app.wasm (+ app.aot when wamrc is there), sizes printed.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
engines="${1:-sky,queen,lure,drascula,dreamweb,cge,cge2,parallaction}"
app="${2:-scummvm}"
WASI="${WASI:-/opt/wasi-sdk-34.0-x86_64-linux}"
WAMRC="${WAMRC:-/root/wamrc-build/wamrc}"
ver=2.9.1
src="$HOME/svm/scummvm-$ver"
bld="$HOME/svm/build-${app}"
P="$HOME/svm/prefix"
[ -f "$P/lib/libvorbisidec.a" ] || bash "$here/deps.sh"

if [ ! -d "$src" ]; then
    mkdir -p "$HOME/svm"
    tar xzf "$root/ports/_src/scummvm-$ver.tar.gz" -C "$HOME/svm"
fi
# Backend + configure patch (idempotent; the backend is re-synced every run).
mkdir -p "$src/backends/platform/nucleo"
cp -u "$here"/backend/* "$src/backends/platform/nucleo/"
python3 "$here/patch_configure.py" "$src"

mkdir -p "$bld"
cd "$bld"
if [ ! -f config.mk ] || [ "$here/build.sh" -nt config.mk ]; then
    CXX="$WASI/bin/wasm32-wasip1-clang++" \
    CXXFLAGS="-O2 -fno-exceptions -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS" \
    LDFLAGS="-Wl,-z,stack-size=524288 -Wl,--strip-all" \
    LIBS="-lwasi-emulated-signal -lwasi-emulated-process-clocks" \
    AR="$WASI/bin/llvm-ar" RANLIB="$WASI/bin/llvm-ranlib" STRIP="$WASI/bin/llvm-strip" \
    "$src/configure" --host=wasm32-wasip1 --backend=nucleo \
        --disable-all-engines --enable-engine="$engines" --disable-detection-full \
        --enable-release --disable-debug --enable-optimizations \
        --disable-highres --disable-scalers --disable-hq-scalers --disable-edge-scalers --disable-aspect \
        --disable-mt32emu --disable-lua --disable-nuked-opl --disable-bink \
        --disable-translation --disable-taskbar --disable-cloud --disable-system-dialogs \
        --disable-eventrecorder --disable-tts --disable-updates --disable-enet \
        --disable-sdlnet --disable-libcurl --disable-readline --disable-libunity --disable-gtk \
        --disable-alsa --disable-sndio --disable-fluidsynth --disable-fluidlite --disable-sonivox \
        --disable-freetype2 --disable-fribidi --disable-png --disable-jpeg --disable-gif \
        --disable-theoradec --disable-vpx --disable-faad --disable-mpeg2 --disable-a52 \
        --disable-vorbis --enable-tremor --with-tremor-prefix="$P" --with-ogg-prefix="$P" \
        --enable-mad --with-mad-prefix="$P" --disable-flac --enable-zlib --with-zlib-prefix="$P" \
        --disable-opengl-game --disable-nasm \
        > configure.log 2>&1 || { tail -40 configure.log; exit 1; }
fi
make -j"$(nproc)" > make.log 2>&1 || { grep -m20 -B2 -A6 "error" make.log; exit 1; }

out="$root/apps/$app"
mkdir -p "$out"
cp scummvm "$out/app.wasm"
echo "app.wasm: $(stat -c %s "$out/app.wasm") bytes"
if [ "${AOT:-0}" = 1 ] && [ -x "$WAMRC" ]; then   # slow (~30 min): AOT=1 for releases
    "$WAMRC" --target=riscv32 --target-abi=ilp32f --cpu=generic-rv32 --cpu-features=+m,+a,+c,+f \
        --enable-multi-thread -o "$out/app.aot" "$out/app.wasm" >/dev/null
    echo "app.aot:  $(stat -c %s "$out/app.aot") bytes"
fi
