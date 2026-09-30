#!/bin/bash
# test.sh — run the ports/cli programs on the PC host (WSL) before they reach the board, like
# ports/test.sh: /root/nvhost = the firmware's WAMR feature set + the nv.try_call/nv.throw
# imports; each program runs interpreted and as x86_64 AOT, with a scripted stdin like the
# Terminal's and ports/cli/_out/home as its "/" (like /sdcard/home).
#
#   bash ports/cli/test.sh          (Git Bash; builds nvhost on first use)
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
w="/mnt/$(cygpath -m "$here" | sed -E 's|^([A-Za-z]):|\L\1|')"   # /mnt/d/NucleoV2/ports/cli
apps="${w%/ports/cli}/apps"
export MSYS_NO_PATHCONV=1
mkdir -p "$here/_out"

cat > "$here/_out/run_tests.sh" <<'EOF'
#!/bin/bash
set -u
w="$1"; apps="$2"
[ -x /root/nvhost ] || bash "${w%/cli}/host/build.sh"
out=$w/_out; home=$out/home
rm -rf "$home"; mkdir -p "$home/figlet-fonts" "$out/aot"
fail=0
check() {   # name expected-substring output
    if printf '%s' "$3" | grep -qF -- "$2"; then echo "  ok   $1"; else echo "  FAIL $1 (want: $2)"; fail=1; fi
}
for id in berry wren tcl pforth bc figlet jq scheme; do
    /root/wamrc-build/wamrc --target=x86_64 --bounds-checks=1 --enable-multi-thread \
        -o "$out/aot/$id.aot" "$apps/$id/app.wasm" >/dev/null
done
for mode in wasm aot; do
    echo "== $mode"
    mod() { [ $mode = wasm ] && echo "$apps/$1/app.wasm" || echo "$out/aot/$1.aot"; }
    run() { local id=$1; shift; timeout 60 /root/nvhost --dir=/::$home --mem=8 --stack=256 "$(mod $id)" "$@" 2>&1; }

    # --- berry: REPL, exceptions (nv_try_call), try blocks in loops, files, os.exit
    o=$(printf '6 * 7\ndef q(x) return x * x end\nq(12)\ntry raise "errore", "qualcosa" except .. as e, m print(e, m) end\n1/0\nprint("vivo")\nimport os os.exit()\nprint("NO")\n' | run berry)
    check "berry repl"            "42" "$o"
    check "berry def"             "144" "$o"
    check "berry try/except"      "errore qualcosa" "$o"
    check "berry error recovery"  "vivo" "$o"
    printf 'var n = 0\ndef t(i) if i %% 3 == 0 raise "value_error" end return i end\nfor i:0..20000 try n += t(i) except "value_error" n -= 1 end end\nprint("n", n)\nf = open("/b.txt", "w") f.write("berry-file") f.close()\nprint(open("/b.txt").read())\ndef out() try try raise "a" except "b" end except "a" return "nested ok" end end\nprint(out())\n' > $home/t.be
    o=$(run berry /t.be)
    check "berry 20000 exceptions" "n 133340000" "$o"
    check "berry files"           "berry-file" "$o"
    check "berry nested try"      "nested ok" "$o"

    # --- wren: REPL expressions/statements, classes, io module, script args
    o=$(printf '1 + 2 * 3\nvar l = [1, 2, 3, 4]\nl.map {|n| n * n }.toList\nclass C {\n  construct new(n) { _n = n }\n  f() { "%%(_n): ok" }\n}\nC.new("Fido").f()\nfoo.bar\n"x" + 1\n5 * 5\n' | run wren)
    check "wren expression"       "7" "$o"
    check "wren closure"          "[1, 4, 9, 16]" "$o"
    check "wren multi-line class" "Fido: ok" "$o"
    check "wren compile error"    "Variable is used but not defined" "$o"
    check "wren runtime error"    "Right operand must be a string" "$o"
    check "wren after errors"     "25" "$o"
    printf 'import "io" for File, Stdin, Process\nFile.write("/w.txt", "wren-file")\nSystem.print(File.read("/w.txt"))\nSystem.print(Process.arguments)\nSystem.print("got %%(Stdin.readLine())")\n' > $home/t.wren
    o=$(echo riga | run wren /t.wren a b)
    check "wren io files"         "wren-file" "$o"
    check "wren args"             "[a, b]" "$o"
    check "wren stdin"            "got riga" "$o"

    # --- tcl (Jim): interactive, procs, dicts, regexp, files, lsort -command errors
    o=$(printf 'expr {6 * 7}\nproc sq {x} { expr {$x * $x} }\nsq 12\nfoo\ndict get [dict create a 1 b 2] b\nputs hi\n' | run tcl)
    check "tcl repl"              ". 42" "$o"
    check "tcl proc"              "144" "$o"
    check "tcl error recovery"    'invalid command name "foo"' "$o"
    check "tcl dict"              ". 2" "$o"
    printf 'set f [open /t.txt w]; puts $f tcl-file; close $f\nset f [open /t.txt]; puts [gets $f]; close $f\nregexp {(\\d+)-(\\d+)} 2026-09 -> a m; puts "m=$m"\nfor {set i 0} {$i < 2000} {incr i} { catch {lsort -command {apply {{a b} {error bad}}} {3 1 2}} }\nputs [lsort -command {apply {{a b} {expr {$b - $a}}}} {3 1 2}]\nputs $argv\n' > $home/t.tcl
    o=$(run tcl /t.tcl x y)
    check "tcl files"             "tcl-file" "$o"
    check "tcl regexp"            "m=09" "$o"
    check "tcl lsort -command"    "3 2 1" "$o"
    check "tcl argv"              "x y" "$o"

    # --- pforth: arithmetic, definitions, floats, include, division by zero, EOF exit
    printf ': ciao ." Ciao dal Forth!" cr ;\nciao\n' > $home/c.fth
    o=$(printf '2 3 + .\n: QUADRATO ( n -- n*n ) DUP * ;    \\ nuova\n12 QUADRATO .\n10 0 DO I . LOOP\nVARIABLE CONTO  5 CONTO !  CONTO @ 2 * .\n2.5e0 3.0e0 F* F.\n1 0 /\ninclude c.fth\n7 6 * .\n' | run pforth -q)
    check "pforth add"            "5" "$o"
    check "pforth colon def"      "144" "$o"
    check "pforth loop"           "0 1 2 3 4 5 6 7 8 9" "$o"
    check "pforth variable"       "10" "$o"
    check "pforth float"          "7.500000" "$o"
    check "pforth div by zero"    "THROW code = -10" "$o"
    check "pforth include"        "Ciao dal Forth!" "$o"
    check "pforth after error"    "42" "$o"
    check "pforth file + exit"    "Ciao dal Forth!" "$(run pforth /c.fth </dev/null)"

    # --- bc: precision, recovery after math/parse errors, math library, quit
    o=$(printf '2^200\n1/0\nscale = 30\n1/7\n3 +* 4\nsqrt(2)\nobase = 16\n255\nquit\n99\n' | run bc -q)
    check "bc bignum"             "1606938044258990275541962092341162602522202993782792835301376" "$o"
    check "bc math error"         "divide by 0" "$o"
    check "bc scale"              ".142857142857142857142857142857" "$o"
    check "bc parse error"        "Parse error" "$o"
    check "bc after errors"       "1.414213562373095048801688724209" "$o"
    check "bc obase"              "FF" "$o"
    printf 'define fatt(n) {\n  if (n < 2) return 1\n  return n * fatt(n - 1)\n}\nfatt(30)\n' > $home/f.bc
    o=$(printf '4*a(1)\nquit\n' | run bc -lq /f.bc)
    check "bc file function"      "265252859812191058636308480000000" "$o"
    check "bc -l pi"              "3.14159265358979323844" "$o"
    o=$(for i in $(seq 1 3000); do echo '1/0'; done; echo '6*7')
    check "bc 3000 errors"        "42" "$(printf '%s\n' "$o" | run bc -q 2>/dev/null)"

    # --- figlet: built-in fonts, stdin, --list, user fonts from the home folder
    o=$(run figlet Nucleo)
    check "figlet standard"       '|  \| | | | |/ __|' "$o"
    check "figlet slant"          "/ ____(_)___ _____" "$(run figlet -f slant Ciao)"
    check "figlet --list"         "smslant" "$(run figlet --list)"
    check "figlet stdin"          "| '_| / _\` / _\` |" "$(printf 'riga\n' | run figlet -f small)"
    check "figlet bad font"       "Unable to open font file" "$(run figlet -f nonexist x)"

    # --- jq: filters, raw output, regex (Oniguruma), bignums, errors
    printf '{"name":"nucleo","apps":[{"id":"lua","size":300},{"id":"jq","size":541}],"n":1.10}\n' > $home/d.json
    check "jq select"             '"jq"' "$(run jq '.apps[] | select(.size > 400) | .id' /d.json)"
    check "jq add"                "841" "$(run jq -c '[.apps[].size] | add' /d.json)"
    check "jq raw"                "NUCLEO" "$(run jq -r '.name | ascii_upcase' /d.json)"
    check "jq --arg"              '"size":300' "$(run jq -c --arg n lua '.apps[] | select(.id == $n)' /d.json)"
    check "jq stdin + -n"         "[0,1,4,9,16]" "$(run jq -nc '[range(5)] | map(. * .)')"
    check "jq regex"              '["bar","baz"]' "$(run jq -nc '"foo bar baz" | [scan("ba.")]')"
    check "jq decnum"             "100000000000000000001" "$(run jq -n '100000000000000000001')"
    check "jq syntax error"       "compile error" "$(run jq 'bad (' /d.json)"

    # --- scheme (TinyScheme): REPL, 64-bit ints, errors, call/cc, files, load, args, big lists
    printf '(define (saluta nome) (string-append "Ciao, " nome "!"))\n(display (saluta "mondo"))\n(newline)\n(display *args*)\n' > $home/s.scm
    o=$(printf '(+ 1 2 3)\n(define (fatt n) (if (< n 2) 1 (* n (fatt (- n 1)))))\n(fatt 20)\n(car (quote ()))\n(map (lambda (x) (* x x)) (list 1 2 3 4))\n(call/cc (lambda (k) (+ 1 (k 42))))\n(define p (open-output-file "/dati.txt")) (write (quote (1 "due" 3.5)) p) (close-output-port p)\n(read (open-input-file "/dati.txt"))\n(load "/s.scm")\n(length (let loop ((i 0) (l (quote ()))) (if (< i 60000) (loop (+ i 1) (cons i l)) l)))\n(exit)\n(display "NO")\n' | run scheme)
    check "scheme repl"           "ts> 6" "$o"
    check "scheme int64"          "2432902008176640000" "$o"
    check "scheme error"          "Error: car: argument 1 must be: pair" "$o"
    check "scheme after error"    "(1 4 9 16)" "$o"
    check "scheme call/cc"        "42" "$o"
    check "scheme files"          '(1 "due" 3.5)' "$o"
    check "scheme load"           "Ciao, mondo!" "$o"
    check "scheme 60000 list"     "60000" "$o"
    check "scheme file + args"    "(uno due)" "$(run scheme /s.scm uno due)"
    check "scheme -e"             "42" "$(run scheme -e '(display (* 6 7))')"
done
exit $fail
EOF
wsl.exe -d Ubuntu-24.04 -- bash "$w/_out/run_tests.sh" "$w" "$apps"
