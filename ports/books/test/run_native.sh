#!/bin/bash
# run_native.sh — the WSL half of ports/books/test.sh: builds book_check and the app natively with
# ASan and UBSan, checks every fixture book, then runs the scripts (all of test/*.script, or the
# names given) on a scratch home whose books/ folder links the fixtures. Any sanitizer report fails.
set -uo pipefail
T="$(cd "$(dirname "$0")" && pwd)"
B="$(cd "$T/.." && pwd)"
S="$B/_src"
G="$B/_out/gen"
O="$B/_out/shots"
Z="$B/../zim"
W="$S/libwebp-1.6.0"
BIN=/tmp/bk_native
CHECK=/tmp/bk_check
engine=("$B"/book.c "$B"/conv.c "$B"/zip.c "$B"/xml.c "$B"/lay.c "$B"/hyph.c "$G"/hyphdata.c "$B"/json.c "$B"/font.c "$B"/gfx.c
        "$B"/img.c "$B"/inflate.c "$G"/fontdata.c "$Z"/nvhtml/nvhtml.c "$S"/lodepng.c "$S"/tjpgd/src/tjpgd.c
        "$W"/src/dec/*.c "$W"/src/dsp/{alpha_processing,cpu,dec,filters,rescaler,upsampling,yuv}*.c "$W"/src/dsp/lossless.c
        "$W"/src/dsp/lossless_{avx2,mips_dsp_r2,msa,neon,sse2,sse41}.c
        "$W"/src/utils/{bit_reader_utils,color_cache_utils,filters_utils,huffman_utils,palette,quant_levels_dec_utils}.c
        "$W"/src/utils/{random_utils,rescaler_utils,thread_utils,utils}.c)
app=("$G"/notices.c "$B"/app.c "$B"/library.c "$B"/reader.c "$B"/catalog.c "$B"/lib.c "$B"/i18n.c "$B"/kbd.c "$T"/native_host.c)
INC=(-I"$B" -I"$G" -I"$Z"/nvhtml -I"$S" -I"$S"/tjpgd/src -I"$S"/miniz-3.1.2 -I"$B/../miniz" -I"$B/../../sdk/include" -I"$W" -I"$W/src")
DEF=(-DNV_SIM -DBK_NATIVE -DLODEPNG_NO_COMPILE_ENCODER -DLODEPNG_NO_COMPILE_DISK -DLODEPNG_NO_COMPILE_ANCILLARY_CHUNKS
     -DLODEPNG_NO_COMPILE_CPP)
newest=0
for f in "${engine[@]}" "${app[@]}" "$B"/*.h "$T"/*.c; do t=$(stat -c %Y "$f"); [ "$t" -gt "$newest" ] && newest=$t; done
san=(-fsanitize=address,undefined -fno-omit-frame-pointer)
warn=(-Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers)
if [ ! -x "$CHECK" ] || [ "$(stat -c %Y "$CHECK")" -lt "$newest" ]; then
    echo "== build book_check"
    gcc -std=gnu11 -O1 -g "${san[@]}" "${warn[@]}" "${DEF[@]}" "${INC[@]}" -o "$CHECK" "$T"/book_check.c "$T"/book_check_stubs.c "${engine[@]}" -lm 2>&1 \
        | grep -E "error|warning" | grep -v "_src/" | head -60
    [ -x "$CHECK" ] && [ "$(stat -c %Y "$CHECK")" -ge "$newest" ] || { echo "BUILD FAILED (book_check)"; exit 1; }
fi
if [ ! -x "$BIN" ] || [ "$(stat -c %Y "$BIN")" -lt "$newest" ]; then
    echo "== build app"
    gcc -std=gnu11 -O1 -g "${san[@]}" "${warn[@]}" "${DEF[@]}" "${INC[@]}" -o "$BIN" "${app[@]}" "${engine[@]}" -lm 2>&1 \
        | grep -E "error|warning" | grep -v "_src/" | head -60
    [ -x "$BIN" ] && [ "$(stat -c %Y "$BIN")" -ge "$newest" ] || { echo "BUILD FAILED"; exit 1; }
fi
# the same app without sanitizers inside the device's memory budget: 11 MiB of linear memory less the
# static data (~1.2 MiB: the deflated fonts, tables) and the 256 KB stack, less a margin for malloc's own use
CAP=/tmp/bk_native_cap
if [ ! -x "$CAP" ] || [ "$(stat -c %Y "$CAP")" -lt "$newest" ]; then
    gcc -std=gnu11 -O1 -g -w "${DEF[@]}" -DBK_MEMCAP=$((9 * 1048576 + 262144)) -DBK_NO_PREGROW "${INC[@]}" \
        -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free,--wrap=strdup -o "$CAP" "${app[@]}" "${engine[@]}" -lm || { echo "BUILD FAILED (cap)"; exit 1; }
fi
python3 "$T/make_catalog.py" "$FIXTURES" "$B/_out/testdata" >/dev/null || { echo "FAIL make_catalog"; exit 1; }
fail=0
echo "== hyphenation"
gcc -std=gnu11 -O1 -g "${san[@]}" "${DEF[@]}" "${INC[@]}" -o /tmp/bk_hyph "$T"/hyph_test.c "$B"/hyph.c "$G"/hyphdata.c "$B"/inflate.c "$B"/font.c "$B"/gfx.c "$G"/fontdata.c     "$T"/book_check_stubs.c -lm && UBSAN_OPTIONS=halt_on_error=1 /tmp/bk_hyph | grep -E "FAIL|HYPH" || { echo "FAIL hyphenation"; fail=1; }
echo "== book_check"
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$CHECK" -q "$FIXTURES"/* > "$O/check.txt" 2>&1 || fail=1
cat "$O/check.txt"
grep -qE "ERROR: AddressSanitizer|runtime error" "$O/check.txt" && { echo "FAIL book_check (sanitizer)"; fail=1; }
ROOT=/tmp/bkroot
names=("$@")
[ ${#names[@]} -gt 0 ] || names=($(cd "$T" && ls *.script | sed 's/\.script$//'))
for n in "${names[@]}"; do
    mkdir -p "$O/$n"
    rm -f "$O/$n"/*.ppm "$O/$n"/*.png
    rm -rf "$ROOT" && mkdir -p "$ROOT/appdata" "$ROOT/books"   # every script starts from a fresh install
    if [ -f "$T/$n.books" ]; then while read -r f; do [ -n "$f" ] && ln -s "$FIXTURES/$f" "$ROOT/books/$f"; done < "$T/$n.books"
    else for f in "$FIXTURES"/*; do ln -s "$f" "$ROOT/books/$(basename "$f")"; done; fi
    echo "== $n"
    if ! BK_ROOT="$ROOT" BK_TESTDATA="${BK_DATA:-$B/_out/testdata}" ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        "$BIN" "$T/$n.script" "$O/$n" >"$O/$n/log.txt" 2>&1; then
        echo "FAIL $n"; tail -30 "$O/$n/log.txt"; fail=1
    else
        grep -E "^native:" "$O/$n/log.txt"
    fi
    if grep -qE "ERROR: AddressSanitizer|runtime error|LeakSanitizer" "$O/$n/log.txt"; then
        echo "FAIL $n (sanitizer)"; grep -E -A14 "ERROR: AddressSanitizer|runtime error|LeakSanitizer" "$O/$n/log.txt" | head -50; fail=1
    fi
    rm -rf "$ROOT/appdata" && mkdir -p "$ROOT/appdata"   # again, within the device's memory budget
    mkdir -p "$O/$n/cap"
    BK_ROOT="$ROOT" BK_TESTDATA="${BK_DATA:-$B/_out/testdata}" "$CAP" "$T/$n.script" "$O/$n/cap" >"$O/$n/cap/log.txt" 2>&1
    grep -E "^memory:" "$O/$n/cap/log.txt" || { echo "FAIL $n (memory budget run)"; tail -5 "$O/$n/cap/log.txt"; fail=1; }
    rm -rf "$O/$n/cap"/*.ppm
done
[ $fail = 0 ] && echo "BOOKS NATIVE OK"
exit $fail
