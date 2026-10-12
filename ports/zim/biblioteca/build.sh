#!/bin/bash
# build.sh — the Biblioteca app (ports/zim/biblioteca: the reader of ZIM files, F3 of
# docs/ZIM_READER_PLAN.md) into apps/biblioteca/:
#
#   app.wasm   wasm32-wasip1 reactor exporting run (own RGB565 frame buffer + wasi-libc)
#   app.aot    riscv32 AOT image for the ESP32-P4 (what the device runs)
#   icon.z     launcher/store icon
#
#   bash ports/zim/biblioteca/build.sh            (Git Bash on Windows)
#   bash ports/zim/biblioteca/build.sh --no-aot   (skip wamrc)
#
# Same toolchain as ports/zim/build.sh. Test on the PC first: bash ports/zim/biblioteca/test.sh.
# The manifest and the guides in apps/biblioteca are hand-written.
set -euo pipefail
here="$(cygpath -m "$(cd "$(dirname "$0")" && pwd)")"      # D:/.../ports/zim/biblioteca
Z="$(cygpath -m "$(cd "$here/.." && pwd)")"
ports="$(cygpath -m "$(cd "$Z/.." && pwd)")"
root="$(cygpath -m "$(cd "$ports/.." && pwd)")"
bash "$Z/fetch.sh" >/dev/null
S="$Z/_src"
X="$S/xz-embedded-20240322"
G="$Z/_out/bib_gen"
W="$S/libwebp-1.6.0"
# libwebp: the decoder only (the lists of its Makefile.am; the SIMD files build empty for wasm32)
WEBP=("$W"/src/dec/*.c "$W"/src/dsp/{alpha_processing,cpu,dec,filters,rescaler,upsampling,yuv}*.c "$W"/src/dsp/lossless.c
      "$W"/src/dsp/lossless_{avx2,mips_dsp_r2,msa,neon,sse2,sse41}.c
      "$W"/src/utils/{bit_reader_utils,color_cache_utils,filters_utils,huffman_utils,palette,quant_levels_dec_utils}.c
      "$W"/src/utils/{random_utils,rescaler_utils,thread_utils,utils}.c)
CLANG="${CLANG:-/c/Program Files/LLVM/bin/clang.exe}"
WASI="${WASI:-D:/esp/wasi-sdk-34}"
WAMRC="${WAMRC:-/root/wamrc-build/wamrc}"
DISTRO="${DISTRO:-Ubuntu-24.04}"
wsl_path() { echo "/mnt/$(echo "${1:0:1}" | tr 'A-Z' 'a-z')${1:2}"; }
out="$root/apps/biblioteca"
mkdir -p "$out" "$G"
python "$here/gen_icons.py" "$G/icons.h" >/dev/null
python "$here/gen_fontdata.py" "$G/fontdata.c" >/dev/null

log="$Z/_out/bib_build.log"
# -O2; a reactor with a 256 KB C stack in linear memory (the frame buffer, the glyph cache, the
# decoded cluster and the page come from malloc, capped by the manifest ram_budget).
"$CLANG" --target=wasm32-wasip1 "--sysroot=$WASI/wasi-sysroot-34.0" -O2 -nodefaultlibs -Wall -Wno-unused-function \
    -D_WASI_EMULATED_PROCESS_CLOCKS -DZSTD_DISABLE_ASM -DZSTD_LEGACY_SUPPORT=0 -DXZ_USE_CRC64 \
    -DLODEPNG_NO_COMPILE_ENCODER -DLODEPNG_NO_COMPILE_DISK -DLODEPNG_NO_COMPILE_ANCILLARY_CHUNKS \
    -DLODEPNG_NO_COMPILE_CPP \
    -I"$here" -I"$G" -I"$Z/nvzim" -I"$Z/nvhtml" -I"$S" -I"$root/sdk/include" -I"$S/zstd-1.5.7/lib" -I"$W" -I"$W/src" \
    -I"$X/linux/include/linux" -I"$X/userspace" \
    -mexec-model=reactor -Wl,--export=run -Wl,-z,stack-size=262144 -Wl,--strip-all -o "$out/app.wasm" \
    "$here"/app.c "$here"/doc.c "$here"/gfx.c "$here"/font.c "$here"/img.c "$here"/kbd.c "$here"/lib.c "$here"/i18n.c \
    "$G/fontdata.c" "${WEBP[@]}" \
    "$Z/nvhtml/nvhtml.c" "$Z/nvzim/nvzim.c" "$Z/nvzim/nvzim_md5.c" "$S/lodepng.c" \
    "$S"/zstd-1.5.7/lib/common/*.c "$S"/zstd-1.5.7/lib/decompress/*.c \
    "$X"/linux/lib/xz/xz_crc32.c "$X"/linux/lib/xz/xz_crc64.c "$X"/linux/lib/xz/xz_dec_stream.c \
    "$X"/linux/lib/xz/xz_dec_lzma2.c \
    -lc -lm -lwasi-emulated-process-clocks "$WASI/libclang_rt-34.0/wasm32-unknown-wasip1/libclang_rt.builtins.a" \
    >"$log" 2>&1 || { grep -v -e 'zstd-1.5.7' -e 'xz-embedded' -e 'lodepng' -e 'stb_' -e 'libwebp' -e 'nanosvg' "$log" >&2; echo "biblioteca: compile failed" >&2; exit 1; }
grep -v -e 'zstd-1.5.7' -e 'xz-embedded' -e 'lodepng' -e 'stb_' -e 'libwebp' -e 'nanosvg' "$log" || true   # our own warnings, if any
n=$(stat -c %s "$out/app.wasm")
[ "$n" -le 2097152 ] || { echo "biblioteca: app.wasm $n bytes > 2 MB device cap" >&2; exit 1; }
if [ "${1:-}" != "--no-aot" ]; then
    MSYS_NO_PATHCONV=1 wsl.exe -d "$DISTRO" -- "$WAMRC" --target=riscv32 --target-abi=ilp32f --cpu=generic-rv32 \
        --cpu-features=+m,+a,+c,+f --enable-multi-thread -o "$(wsl_path "$out/app.aot")" "$(wsl_path "$out/app.wasm")" \
        >/dev/null
    a=$(stat -c %s "$out/app.aot")
    [ "$a" -le 4194304 ] || { echo "biblioteca: app.aot $a bytes > 4 MB device cap" >&2; exit 1; }
    echo "  biblioteca/app.wasm $n bytes, app.aot $a bytes"
else
    echo "  biblioteca/app.wasm $n bytes"
fi
python "$here/make_icon.py" "$out/icon.z"
