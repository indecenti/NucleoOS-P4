#!/bin/bash
# build.sh — the Libri app (ports/books: the ebook reader of docs/BOOKS_PLAN.md) into apps/libri/:
#
#   app.wasm   wasm32-wasip1 reactor exporting run (own RGB565 frame buffer + wasi-libc)
#   app.aot    riscv32 AOT image for the ESP32-P4 (what the device runs)
#   icon.z     launcher/store icon
#
#   bash ports/books/build.sh            (Git Bash on Windows)
#   bash ports/books/build.sh --no-aot   (skip wamrc)
#
# Same toolchain as the Biblioteca app (ports/zim/biblioteca/build.sh). Test on the PC first:
# bash ports/books/test.sh. The manifest and the guides in apps/libri are hand-written.
set -euo pipefail
here="$(cygpath -m "$(cd "$(dirname "$0")" && pwd)")"
ports="$(cygpath -m "$(cd "$here/.." && pwd)")"
root="$(cygpath -m "$(cd "$ports/.." && pwd)")"
bash "$here/fetch.sh" >/dev/null
S="$here/_src"
G="$here/_out/gen"
W="$S/libwebp-1.6.0"
Z="$ports/zim"
WEBP=("$W"/src/dec/*.c "$W"/src/dsp/{alpha_processing,cpu,dec,filters,rescaler,upsampling,yuv}*.c "$W"/src/dsp/lossless.c
      "$W"/src/dsp/lossless_{avx2,mips_dsp_r2,msa,neon,sse2,sse41}.c
      "$W"/src/utils/{bit_reader_utils,color_cache_utils,filters_utils,huffman_utils,palette,quant_levels_dec_utils}.c
      "$W"/src/utils/{random_utils,rescaler_utils,thread_utils,utils}.c)
CLANG="${CLANG:-/c/Program Files/LLVM/bin/clang.exe}"
WASI="${WASI:-D:/esp/wasi-sdk-34}"
WAMRC="${WAMRC:-/root/wamrc-build/wamrc}"
DISTRO="${DISTRO:-Ubuntu-24.04}"
wsl_path() { echo "/mnt/$(echo "${1:0:1}" | tr 'A-Z' 'a-z')${1:2}"; }
out="$root/apps/libri"
mkdir -p "$out" "$G" "$here/_out"
python "$here/gen_icons.py" "$G/icons.h" >/dev/null
python "$here/gen_fontdata.py" "$G/fontdata.c" >/dev/null
python "$here/gen_hyph.py" "$G/hyphdata.c" >/dev/null
python "$here/gen_text.py" "$here/NOTICES.txt" "$G/notices.c" bk_notices

log="$here/_out/build.log"
# -O2; a reactor with a 256 KB C stack in linear memory (frame buffers, surfaces, the glyph cache,
# the chapters and their layout come from malloc, capped by the manifest ram_budget).
# BK_HW_ROTATE=1: portrait turned by the system (ABI 19, firmware 1.2.151+); the manifest for it is
# _out/manifest.abi19.json (apps/libri keeps ABI 18 until that firmware is public)
HW=()
if [ "${BK_HW_ROTATE:-0}" = 1 ]; then
    HW=(-DBK_HW_ROTATE)
    python -c "import json,sys; m=json.load(open(sys.argv[1],encoding='utf-8')); m['abi']=19; json.dump(m,open(sys.argv[2],'w',encoding='utf-8'),ensure_ascii=False,indent=2)" \
        "$out/manifest.json" "$here/_out/manifest.abi19.json"
fi
"$CLANG" --target=wasm32-wasip1 "--sysroot=$WASI/wasi-sysroot-34.0" -O2 -nodefaultlibs -Wall -Wno-unused-function "${HW[@]}" \
    -D_WASI_EMULATED_PROCESS_CLOCKS -DLODEPNG_NO_COMPILE_ENCODER -DLODEPNG_NO_COMPILE_DISK \
    -DLODEPNG_NO_COMPILE_ANCILLARY_CHUNKS -DLODEPNG_NO_COMPILE_CPP \
    -I"$here" -I"$G" -I"$Z/nvhtml" -I"$S" -I"$S/tjpgd/src" -I"$S/miniz-3.1.2" -I"$ports/miniz" -I"$root/sdk/include" \
    -I"$W" -I"$W/src" \
    -mexec-model=reactor -Wl,--export=run -Wl,-z,stack-size=262144 -Wl,--strip-all -o "$out/app.wasm" \
    "$here"/app.c "$here"/library.c "$here"/reader.c "$here"/catalog.c "$here"/lib.c "$here"/i18n.c "$here"/kbd.c \
    "$here"/book.c "$here"/conv.c "$here"/zip.c "$here"/xml.c "$here"/lay.c "$here"/hyph.c "$G"/hyphdata.c \
    "$here"/json.c "$here"/font.c "$here"/gfx.c "$here"/img.c "$here"/inflate.c "$G/fontdata.c" "$G/notices.c" "$Z/nvhtml/nvhtml.c" \
    "$S/lodepng.c" "$S/tjpgd/src/tjpgd.c" "${WEBP[@]}" \
    -lc -lm -lwasi-emulated-process-clocks "$WASI/libclang_rt-34.0/wasm32-unknown-wasip1/libclang_rt.builtins.a" \
    >"$log" 2>&1 || { grep -v -e 'lodepng' -e 'stb_' -e 'libwebp' -e 'nanosvg' -e 'tjpgd' -e 'miniz' "$log" >&2; echo "libri: compile failed" >&2; exit 1; }
grep -v -e 'lodepng' -e 'stb_' -e 'libwebp' -e 'nanosvg' -e 'tjpgd' -e 'miniz' "$log" || true   # our own warnings, if any
n=$(stat -c %s "$out/app.wasm")
[ "$n" -le 2097152 ] || { echo "libri: app.wasm $n bytes > 2 MB device cap" >&2; exit 1; }
if [ "${1:-}" != "--no-aot" ]; then
    MSYS_NO_PATHCONV=1 wsl.exe -d "$DISTRO" -- "$WAMRC" --target=riscv32 --target-abi=ilp32f --cpu=generic-rv32 \
        --cpu-features=+m,+a,+c,+f --enable-multi-thread -o "$(wsl_path "$out/app.aot")" "$(wsl_path "$out/app.wasm")" \
        >/dev/null
    a=$(stat -c %s "$out/app.aot")
    [ "$a" -le 4194304 ] || { echo "libri: app.aot $a bytes > 4 MB device cap" >&2; exit 1; }
    echo "  libri/app.wasm $n bytes, app.aot $a bytes"
else
    echo "  libri/app.wasm $n bytes"
fi
python "$here/make_icon.py" "$out/icon.z"
