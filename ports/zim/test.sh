#!/bin/bash
# test.sh — run the zim program (apps/zim/app.wasm) on the PC before it reaches the board, like
# ports/cli/test.sh: /root/nvhost (the firmware's WAMR feature set), interpreted and as x86_64 AOT,
# with ports/zim/_out/home as its "/" (like /sdcard/home) and the ZIM files in its zim/ folder: the
# synthetic ones of tests/host/corpus/nvzim, plus D:/zimtest/fixtures' DevDocs C and ScoutWiki if
# they are there (Kiwix files, not committed).
#
#   bash ports/zim/test.sh          (Git Bash; build first: bash ports/zim/build.sh)
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
w="/mnt/$(cygpath -m "$here" | sed -E 's|^([A-Za-z]):|\L\1|')"   # /mnt/d/NucleoV2/ports/zim
export MSYS_NO_PATHCONV=1
mkdir -p "$here/_out"

cat > "$here/_out/run_tests.sh" <<'EOF'
#!/bin/bash
set -u
w="$1"; root="${w%/ports/zim}"
[ -x /root/nvhost ] || bash "$root/ports/host/build.sh"
out=$w/_out; home=$out/home
rm -rf "$home"; mkdir -p "$home/zim" "$out/aot"
cp "$root/tests/host/corpus/nvzim/new.zim" "$root/tests/host/corpus/nvzim/old.zim" "$home/zim/"
real=0
for f in devdocs_en_c_2026-07.zim scoutwiki.org_it_all_maxi_2026-07.zim; do
    [ -f "/mnt/d/zimtest/fixtures/$f" ] && cp "/mnt/d/zimtest/fixtures/$f" "$home/zim/" && real=$((real + 1))
done
fail=0
check() {   # name expected-substring output
    if printf '%s' "$3" | grep -qF -- "$2"; then echo "  ok   $1"; else echo "  FAIL $1 (want: $2)"; printf '%s\n' "$3" | head -5 | sed 's/^/       | /'; fail=1; fi
}
status() {   # name expected-status actual
    if [ "$2" = "$3" ]; then echo "  ok   $1"; else echo "  FAIL $1 (status $3, want $2)"; fail=1; fi
}
/root/wamrc-build/wamrc --target=x86_64 --bounds-checks=1 --enable-multi-thread -o "$out/aot/zim.aot" \
    "$root/apps/zim/app.wasm" >/dev/null
for mode in wasm aot; do
    echo "== $mode"
    mod() { [ $mode = wasm ] && echo "$root/apps/zim/app.wasm" || echo "$out/aot/zim.aot"; }
    run() { timeout 120 /root/nvhost --dir=/::$home --mem=12 --stack=256 "$(mod)" "$@" 2>&1; }

    o=$(run ls);                                    check "ls"                "nvzim_test" "$o"
    check "ls old file"           "Old test" "$o"
    o=$(run search art);                            check "search everywhere" "nvzim_test: Article 000" "$o"
    o=$(run search -z nvzim_test città);            check "search accents"    "città minuscola" "$o"
    check "search capitalised"    "Città di prova" "$o"
    o=$(run search -z nvzim_test diabete);          check "search dedup"      "Diabete" "$o"
    [ "$(printf '%s\n' "$o" | grep -v 'warning:' | grep -c .)" = 1 ] && echo "  ok   search one per article" || { echo "  FAIL search one per article"; fail=1; }
    o=$(run get "article 001");                     check "get any case"      "Article 001: lorem ipsum" "$o"
    check "get source line"       "-- nvzim test file (nvzim_test, 2026-10-11), offline" "$o"
    o=$(run get -z nvzim_test R1);                  check "get redirect chain" "# Article 001" "$o"
    run get -z nvzim_test LoopA >/dev/null;         status "get redirect loop" 3 $?
    run get nothing_like_this >/dev/null;           status "get missing"       1 $?
    o=$(run get -z old "Page 07");                  check "get old xz file"   "Article 007" "$o"
    o=$(run get -z nvzim_test Article_199 --max 300); check "get big cluster"  "[…]" "$o"
    o=$(run get -z nvzim_test img/pic_01.png);      check "get binary refused" "save it with --out" "$o"
    o=$(run get -z nvzim_test img/pic_01.png --out /p.png); check "get --out" "/p.png: 358 bytes (image/png)" "$o"
    [ "$(stat -c %s $home/p.png 2>/dev/null)" = 358 ] && echo "  ok   --out file" || { echo "  FAIL --out file"; fail=1; }
    o=$(run info nvzim_test);                       check "info"              "Title: nvzim test file" "$o"
    check "info main page"        "Main page: Main Page" "$o"
    o=$(run check nvzim_test);                      check "check md5"         "nvzim_test: ok" "$o"
    o=$(run random -z nvzim_test);                  check "random"            "(nvzim_test, 2026-10-11), offline" "$o"
    o=$(run get -z nvz "Main Page");                check "name by a part"    "# Main Page" "$o"
    o=$(run get -z /zim/new.zim "Main Page");       check "name by file"      "# Main Page" "$o"
    run bogus >/dev/null;                           status "usage"            2 $?
    if [ $real = 2 ]; then
        o=$(run get -z devdocs io/fprintf --max 400);  check "devdocs by path"   "printf" "$o"
        o=$(run get Scautismo --intro --max 600);      check "scoutwiki intro"   "# Scautismo" "$o"
        o=$(run get -z scout Scautismo --sections);    check "scoutwiki sections" "## " "$o"
        o=$(run search -z scout campo);                check "scoutwiki search"  "Campo estivo" "$o"
    fi
done
[ $fail = 0 ] && echo "ALL OK" || echo "SOME FAILED"
exit $fail
EOF
wsl.exe -d Ubuntu-24.04 -- bash "$w/_out/run_tests.sh" "$w"
