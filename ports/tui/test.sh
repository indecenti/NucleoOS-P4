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

echo "== Excel formula notation (native)"
gcc -O1 -I"$w/scim" -o "$out/bin/test_nv_fx" "$w/tests/test_nv_fx.c" "$w/scim/nv_fx.c" \
    && "$out/bin/test_nv_fx" || fail=1

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
check "csv -> markdown"         "| mela       |       3.00 |       0.50 |" "$o"
o=$(printf 'let D2 = B2*C2\nrecalc\nEXECUTE "w! /t.xlsx"\n' | S --nocurses /t.csv 2>&1)
check "save xlsx"               'File "/t.xlsx" written' "$o"
o=$(S --nocurses --quit_afterload --export_csv /t.xlsx 2>&1)
check "xlsx round trip"         "pera,2.00,0.80,1.60" "$o"

echo "== Fogli engine (sc-im --nv_engine)"
E() { S --nocurses --nv_engine "$@" 2>&1 | tr '\t\036' '|^'; }
o=$(printf '%s\n' "nv lang it" "nv set 0 0 Voce" "nv set 0 1 Importo" "nv set 1 0 Affitto" "nv set 1 1 750" \
    "nv set 2 0 Spesa" "nv set 2 1 382,6" "nv set 3 0 Totale" "nv set 3 1 =SOMMA(B2:B3)" "nv set 4 1 12,5%" \
    "nv set 5 1 12/10/2026" "nv set 6 1 '007" "nv set 7 1 =SE(B2>700;\"alto\";\"basso\")" "nv view 0 0 9 2" \
    "nv cell 3 1" "nv stats 1 1 3 1" "nv tsv 0 0 3 1 fx" "nv lang en" "nv check 0 =SUM(B2:B3)" \
    "nv check 1 =VLOOKUP(1,A1:B3,2)" "nv set 8 1 1.234" "nv lang it" "nv view 8 1 8 1" "nv bold 0 0 0 1 1" \
    "nv fmt 1 1 3 1 #,##0.00 €" "nv view 0 0 3 1" "nv undo" "nv view 1 1 1 1" "nv redo" "nv clear 20 0 65535 3" \
    "nv copy 0 0 65535 701" "nv save /f.xlsx" "nv info" "nv sync 9" | E)
check "engine: ready"            "^0" "$o"
check "engine: localized number" "C|2|1|n|r|0|382,6" "$o"
check "engine: formula (it)"     "C|3|1|f|r|0|1132,6" "$o"
check "engine: formula bar (it)" "X|3|1|=SOMMA(B2:B3)" "$o"
check "engine: percent"          "C|4|1|n|r|0|12,50%" "$o"
check "engine: date"             "C|5|1|d|r|0|12/10/2026" "$o"
check "engine: 'text"            "C|6|1|s|l|0|007" "$o"
check "engine: text formula"     "C|7|1|g|l|0|alto" "$o"
check "engine: stats"            "T|3|3|2265,2|755,0666667|382,6|1132,6" "$o"
check "engine: tsv with formula" "L|4|Totale|1132.6 [=SUM(B2:B3)]" "$o"
check "engine: check ok"         "K|0|1" "$o"
check "engine: check refused"    "K|1|0" "$o"
check "engine: English value"    "C|8|1|n|r|0|1,234" "$o"
check "engine: bold"             "C|0|0|s|l|1|Voce" "$o"
check "engine: currency"         "C|1|1|n|r|0|750,00 €" "$o"
check "engine: undo"             "C|1|1|n|r|0|750" "$o"
check "engine: save"             "OK|save" "$o"
check "engine: named"            "D|/f.xlsx|0|" "$o"
check "engine: done"             "^9" "$o"
o=$(printf '%s\n' "nv lang en" "nv view 0 0 7 1" "nv cell 3 1" "nv sync 2" | E /f.xlsx)
check "xlsx: formula kept"       "X|3|1|=SUM(B2:B3)" "$o"
check "xlsx: bold kept"          "C|0|0|s|l|1|Voce" "$o"
check "xlsx: format kept"        "C|1|1|n|r|0|750.00 €" "$o"
check "xlsx: text formula kept"  "C|7|1|g|l|0|alto" "$o"
python3 -c "import zipfile,sys; sys.stdout.write(zipfile.ZipFile(sys.argv[1]).read(\"xl/worksheets/sheet1.xml\").decode())" $home/f.xlsx > $out/f.xml 2>/dev/null
check "xlsx: Excel notation"     "<f>SUM(B2:B3)</f>" "$(cat $out/f.xml)"

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
