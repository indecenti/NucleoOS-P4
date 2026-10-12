#!/bin/bash
# test.sh — test the full-screen programs on the PC (WSL) before they reach the board:
#   1. tests/test_nv_tty.c: the tty shim (in-band resize reports, poll/select, TIOCGWINSZ), native;
#   2. sc-im and WordGrinder under /root/nvhost (the firmware's WAMR + the "nv" imports), with the
#      Terminal's environment (TERM, COLUMNS, LINES, LANG, NUCLEO_UTC_OFFSET) and a 16 MB memory
#      cap like the device's ram_budget: command mode, file formats, dates, memory limits, safe
#      saving; then their screens driven in tmux (keys, a resize report, a mouse click).
# Workbooks with every cell type come from tests/xgen.c (libxlsxwriter, the same build sc-im uses).
#
#   bash ports/tui/test.sh            (Git Bash; run ports/tui/build.sh first)
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
w="/mnt/$(cygpath -m "$here" | sed -E 's|^([A-Za-z]):|\L\1|')"   # /mnt/d/.../ports/tui
export MSYS_NO_PATHCONV=1
mkdir -p "$here/_out"

cat > "$here/_out/run_tests.sh" <<'EOF'
#!/bin/bash
set -u
w="$1"; apps="${w%/ports/tui}/apps"
W=/opt/wasi-sdk-34.0-x86_64-linux; P=/root/nv_tui/prefix
host="${w%/tui}/host"
# nvhost with --env/--raw (ports/host/nvhost.c): rebuilt when its source is newer
if [ ! -x /root/nvhost ] || [ "$host/nvhost.c" -nt /root/nvhost ]; then bash "$host/build.sh" >/dev/null; fi
out=/root/nv_tui/test; home=$out/home
rm -rf "$out"; mkdir -p "$home/tmp" "$out/bin"
fail=0
check() {   # name expected-substring output
    if printf '%s' "$3" | grep -qF -- "$2"; then echo "  ok   $1"; else echo "  FAIL $1 (want: $2)"; fail=1; fi
}
nocheck() {   # name unexpected-substring output
    if printf '%s' "$3" | grep -qF -- "$2"; then echo "  FAIL $1 (unexpected: $2)"; fail=1; else echo "  ok   $1"; fi
}

echo "== tty shim (native)"
gcc -O1 -I"$w/compat" -o "$out/bin/test_nv_tty" "$w/tests/test_nv_tty.c" "$w/compat/nv_tty.c" \
    && "$out/bin/test_nv_tty" || fail=1

ENV=(--env=TERM=xterm-256color --env=LANG=C.UTF-8 --env=HOME=/)
run() { local app=$1 m=$2; shift 2; timeout 120 /root/nvhost --dir=/::$home --dir=/tmp::$home/tmp \
        "${ENV[@]}" --env=NUCLEO_UTC_OFFSET=${TZ_OFF:-7200} --mem=$m --stack=256 "$apps/$app/app.wasm" "$@"; }
S() { run sc-im 16 "$@"; }
WG() { run wordgrinder 16 "$@"; }

echo "== sc-im: command mode"
o=$(printf 'let A0 = 10\nlet A1 = 32\nlet A2 = @sum(A0:A1)\nrecalc\ngetnum A2\n' | S --nocurses 2>&1)
check "formula"                 "42" "$o"
printf 'nome,qta,prezzo\nmela,3,0.5\npera,2,0.8\n' > $home/t.csv
o=$(S --nocurses --quit_afterload --export_mkd /t.csv 2>&1)
check "csv -> markdown"         "|    mela    |       3.00 |       0.50 |" "$o"
o=$(printf 'let D2 = B2*C2\nrecalc\nEXECUTE "w! /t.xlsx"\n' | S --nocurses /t.csv 2>&1)
check "save xlsx"               'File "/t.xlsx" written' "$o"
o=$(S --nocurses --quit_afterload --export_csv /t.xlsx 2>&1)
check "xlsx round trip"         "pera,2.00,0.80,1.60" "$o"

echo "== sc-im: xlsx import"
$W/bin/clang --target=wasm32-wasip1 --sysroot=$W/share/wasi-sysroot -O1 -Wl,-z,stack-size=262144 \
    -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_GETPID -I$P/include -I"$w/compat" -o "$out/bin/xgen.wasm" \
    "$w/tests/xgen.c" "$w/compat/nv_posix.c" -L$P/lib -lxlsxwriter -lz -lwasi-emulated-signal \
    -lwasi-emulated-getpid 2>/dev/null
xgen() { /root/nvhost --dir=/::$home --dir=/tmp::$home/tmp --env=OUT=$1 --env=MODE=$2 --stack=256 "$out/bin/xgen.wasm"; }
xgen /mix.xlsx mix; xgen /inl.xlsx inl
o=$(S --nocurses --quit_afterload --export_csv /mix.xlsx 2>&1)
check "shared strings, UTF-8"   "affitto ''giugno'' àè,650.50" "$o"
check "date (Excel serial)"     "15/06/2026" "$o"
check "time"                    "13:30:15" "$o"
check "boolean"                 "pagato,1.00" "$o"
check "formula value"           "totale,730.75" "$o"
o=$(TZ_OFF=0 S --nocurses --quit_afterload --export_csv /mix.xlsx 2>&1
    TZ_OFF=-18000 S --nocurses --quit_afterload --export_csv /mix.xlsx 2>&1)
nocheck "date in any time zone (-1 day)" "14/06/2026" "$o"
nocheck "date in any time zone (+1 day)" "16/06/2026" "$o"
o=$(S --nocurses --quit_afterload --export_csv --sheet=2 /mix.xlsx 2>&1)
check "--sheet=N"               "x,42.00" "$o"
o=$(S --nocurses --quit_afterload --export_csv --sheet=Secondo /mix.xlsx 2>&1)
check "--sheet=NAME"            "x,42.00" "$o"
o=$(S --nocurses --quit_afterload --export_csv /inl.xlsx 2>&1)
check "inline strings"          "luce,80.25" "$o"
o=$(printf 'EXECUTE "w! /mix2.xlsx"\n' | S --nocurses /mix.xlsx 2>&1)
check "invalid sheet name saved" 'File "/mix2.xlsx" written' "$o"
o=$(S --nocurses --quit_afterload --export_csv /mix2.xlsx 2>&1)
check "date survives a save"    "15/06/2026" "$o"
cp "$w/_src/libxls-1.6.3/test/files/test2.xls" $home/old.xls
o=$(S --nocurses --quit_afterload --export_csv /old.xls 2>&1)
check "old .xls: text"          "cette ligne est écrite en français" "$o"
check "old .xls: quotes kept"   "''doubly quoted string''" "$o"
nocheck "old .xls: no syntax error" "syntax error" "$o"
printf 'not a zip' > $home/bad.xlsx
o=$(S --nocurses --quit_afterload --export_csv /bad.xlsx 2>&1); rc=$?
check "bad file: an error, no crash" "can't open zip archive" "$o"

echo "== sc-im: memory (16 MB, as on the device)"
python3 -c "
import random
random.seed(2)
with open('$home/big.csv', 'w') as f:
    f.write('id,nome,qta,prezzo\n')
    for i in range(8000): f.write(f'{i},articolo {i},{random.randint(1, 50)},{random.randint(100, 9999) / 100}\n')"
o=$(printf 'EXECUTE "w! /big.xlsx"\n' | S --nocurses /big.csv 2>&1)
check "8000 rows saved"         'File "/big.xlsx" written' "$o"
n=$(S --nocurses --quit_afterload --export_csv /big.xlsx 2>/dev/null | wc -l)
check "8000 rows read back"     "8001" "$n"
cp $home/t.xlsx $home/keep.xlsx; before=$(md5sum < $home/keep.xlsx)
o=$(printf 'EXECUTE "w! /keep.xlsx"\n' | run sc-im 4 --nocurses /big.csv 2>&1)
check "out of memory: not saved" "could not be saved" "$o"
check "out of memory: file kept" "$before" "$(md5sum < $home/keep.xlsx)"
nocheck "no temporary file left" "tmp~" "$(ls $home)"

echo "== WordGrinder"
o=$(WG --help 2>&1)
check "starts (precompiled Lua)" "WordGrinder version 0.9" "$o"
printf '# Titolo\n\nCiao **mondo**, *corsivo* e àèìòù.\n\n- uno\n- due\n' > $home/t.md
WG --convert /t.md /t.odt >/dev/null 2>&1; WG --convert /t.odt /t.txt >/dev/null 2>&1
check "md -> odt -> txt"        "Ciao mondo, corsivo e àèìòù." "$(cat $home/t.txt 2>/dev/null)"
WG --convert /t.odt /t.html >/dev/null 2>&1
check "odt keeps bold"          "<b>mondo</b>" "$(cat $home/t.html 2>/dev/null)"
check "odt keeps the list"      "<li>uno</li>" "$(cat $home/t.html 2>/dev/null)"

echo "== screens (tmux)"
T() { tmux capture-pane -p -t nvt; }
scr() { tmux kill-session -t nvt 2>/dev/null
        tmux new-session -d -s nvt -x 100 -y 28 "/root/nvhost --raw --dir=/::$home \
            --dir=/tmp::$home/tmp ${ENV[*]} --env=NUCLEO_UTC_OFFSET=7200 --env=COLUMNS=100 \
            --env=LINES=28 --mem=16 --stack=256 $apps/$1/app.wasm $2; echo EXIT=\$?; sleep 30"; }
scr sc-im /t.xlsx; sleep 3
check "sc-im shows the sheet"   "mela" "$(T)"
tmux send-keys -t nvt -l $'\e[<0;22;6M\e[<0;22;6m'; sleep 0.8      # click: column 22, line 6 = B2
check "mouse click selects B2"  "B2" "$(T | head -1)"
tmux send-keys -t nvt -l $'\e[48;12;60;0;0t'; sleep 1.5
check "resize report: redrawn"  "E" "$(T | sed -n 3p)"
nocheck "resize report: 60 columns" "F" "$(T | sed -n 3p)"
nocheck "resize report: no garbage" "48;12" "$(T)"
tmux send-keys -t nvt ':q!' Enter; sleep 1.5
check "sc-im quits"             "EXIT=0" "$(T)"
cp $home/t.odt $home/o.odt; before=$(md5sum < $home/o.odt)
scr wordgrinder /o.odt; sleep 3
check "WordGrinder imports .odt" "Ciao mondo" "$(T)"
tmux send-keys -t nvt C-s; sleep 1.5
check "save writes a .wg"       "o.wg" "$(ls $home)"
check "the .odt is untouched"   "$before" "$(md5sum < $home/o.odt)"
tmux send-keys -t nvt C-q; sleep 1.5
tmux kill-session -t nvt 2>/dev/null

exit $fail
EOF
wsl.exe -d Ubuntu-24.04 -- bash "$w/_out/run_tests.sh" "$w" 2>&1 | tee "$here/_out/test.log"
grep -q FAIL "$here/_out/test.log" && { echo "TESTS FAILED"; exit 1; }
echo "all tests passed"
