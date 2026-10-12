#!/bin/bash
# run_native.sh — the WSL half of ports/zim/biblioteca/test.sh: build the app natively with ASan and
# UBSan, then run the scripts (all of test/*.script, or the names given) on a scratch home whose
# zim/ folder links the fixtures. Any sanitizer report fails the run.
set -uo pipefail
T="$(cd "$(dirname "$0")" && pwd)"
B="$(cd "$T/.." && pwd)"
Z="$(cd "$B/.." && pwd)"
S="$Z/_src"
G="$Z/_out/bib_gen"
O="$Z/_out/bib_shots"
X="$S/xz-embedded-20240322"
BIN=/tmp/bib_native
srcs=("$B"/app.c "$B"/doc.c "$B"/gfx.c "$B"/font.c "$B"/kbd.c "$B"/lib.c "$B"/i18n.c "$G"/fontdata.c "$T"/native_host.c
      "$Z"/nvzim/nvzim.c "$Z"/nvzim/nvzim_md5.c "$Z"/nvhtml/nvhtml.c "$S"/lodepng.c
      "$S"/zstd-1.5.7/lib/common/*.c "$S"/zstd-1.5.7/lib/decompress/*.c
      "$X"/linux/lib/xz/xz_crc32.c "$X"/linux/lib/xz/xz_crc64.c "$X"/linux/lib/xz/xz_dec_stream.c "$X"/linux/lib/xz/xz_dec_lzma2.c)
newest=0
for f in "${srcs[@]}" "$B"/*.h "$T"/*.c; do t=$(stat -c %Y "$f"); [ "$t" -gt "$newest" ] && newest=$t; done
if [ ! -x "$BIN" ] || [ "$(stat -c %Y "$BIN")" -lt "$newest" ]; then
    echo "== build"
    gcc -std=gnu11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter \
        -Wno-sign-compare -Wno-missing-field-initializers -DNV_SIM -DBIB_NATIVE -DZSTD_DISABLE_ASM -DZSTD_LEGACY_SUPPORT=0 \
        -DXZ_USE_CRC64 -DXZ_DEC_SINGLE -DLODEPNG_NO_COMPILE_ENCODER -DLODEPNG_NO_COMPILE_DISK \
        -DLODEPNG_NO_COMPILE_ANCILLARY_CHUNKS -DLODEPNG_NO_COMPILE_CPP \
        -I"$B" -I"$G" -I"$Z"/nvzim -I"$Z"/nvhtml -I"$S" -I"$Z/../../sdk/include" -I"$S"/zstd-1.5.7/lib \
        -I"$S"/zstd-1.5.7/lib/common -I"$X"/linux/include/linux -I"$X"/userspace \
        -o "$BIN" "${srcs[@]}" -lm 2>&1 | grep -v "^$" | grep -E "error|warning" | grep -v "_src/" | head -40
    [ -x "$BIN" ] && [ "$(stat -c %Y "$BIN")" -ge "$newest" ] || { echo "BUILD FAILED"; exit 1; }
fi
# The same app without sanitizers and with the device's memory budget: 16 MiB of linear memory less
# the static data (~0.8 MiB: fonts, tables) and the 256 KB stack, less a margin for malloc's own use.
CAP=/tmp/bib_native_cap
if [ ! -x "$CAP" ] || [ "$(stat -c %Y "$CAP")" -lt "$newest" ]; then
    gcc -std=gnu11 -O1 -g -w -DNV_SIM -DBIB_NATIVE -DBIB_MEMCAP=$((14 * 1048576 + 524288)) -DBIB_NO_PREGROW -DZSTD_DISABLE_ASM \
        -DZSTD_LEGACY_SUPPORT=0 -DXZ_USE_CRC64 -DLODEPNG_NO_COMPILE_ENCODER -DLODEPNG_NO_COMPILE_DISK \
        -DLODEPNG_NO_COMPILE_ANCILLARY_CHUNKS -DLODEPNG_NO_COMPILE_CPP \
        -I"$B" -I"$G" -I"$Z"/nvzim -I"$Z"/nvhtml -I"$S" -I"$Z/../../sdk/include" -I"$S"/zstd-1.5.7/lib \
        -I"$S"/zstd-1.5.7/lib/common -I"$X"/linux/include/linux -I"$X"/userspace \
        -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free -o "$CAP" "${srcs[@]}" -lm || { echo "BUILD FAILED (cap)"; exit 1; }
fi
ROOT=/tmp/bibroot
rm -rf "$ROOT"
mkdir -p "$ROOT/zim" "$ROOT/appdata"
for f in "$FIXTURES"/*.zim; do ln -s "$f" "$ROOT/zim/$(basename "$f")"; done
names=("$@")
[ ${#names[@]} -gt 0 ] || names=($(cd "$T" && ls *.script | sed 's/\.script$//'))
fail=0
for n in "${names[@]}"; do
    mkdir -p "$O/$n"
    rm -f "$O/$n"/*.ppm "$O/$n"/*.png
    rm -rf "$ROOT/appdata" && mkdir -p "$ROOT/appdata"   # every script starts from a fresh install
    [ -f "$T/$n.empty" ] && rm -f "$ROOT/zim"/*   # a library with nothing in it
    echo "== $n"
    if ! BIB_ROOT="$ROOT" ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        "$BIN" "$T/$n.script" "$O/$n" >"$O/$n/log.txt" 2>&1; then
        echo "FAIL $n"; tail -30 "$O/$n/log.txt"; fail=1
    else
        grep -E "^native:|^frame" "$O/$n/log.txt"
    fi
    rm -rf "$ROOT/appdata" && mkdir -p "$ROOT/appdata"   # again, within the device's memory budget
    mkdir -p "$O/$n/cap"
    BIB_ROOT="$ROOT" "$CAP" "$T/$n.script" "$O/$n/cap" >"$O/$n/cap/log.txt" 2>&1
    grep -E "^memory:" "$O/$n/cap/log.txt" || { echo "FAIL $n (memory budget run)"; tail -5 "$O/$n/cap/log.txt"; fail=1; }
    rm -rf "$O/$n/cap"/*.ppm
    if grep -qE "ERROR: AddressSanitizer|runtime error|LeakSanitizer" "$O/$n/log.txt"; then
        echo "FAIL $n (sanitizer)"; grep -E -A12 "ERROR: AddressSanitizer|runtime error|LeakSanitizer" "$O/$n/log.txt" | head -40; fail=1
    fi
    [ -f "$T/$n.empty" ] && for f in "$FIXTURES"/*.zim; do ln -sf "$f" "$ROOT/zim/$(basename "$f")"; done
done
[ $fail = 0 ] && echo "BIB NATIVE OK"
exit $fail
