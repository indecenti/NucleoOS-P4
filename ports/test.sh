#!/bin/bash
# test.sh — run the terminal programs on the PC host (WSL) before they reach the board:
# nvhost = the firmware's WAMR feature set (fast-interp + AOT without HW bound checks, libc-wasi)
# + the nv.try_call/nv.throw imports. Each program runs interpreted and as x86_64 AOT, fed a
# scripted stdin like the Terminal would, with ports/_out/home as its "/" (like /sdcard/home).
#
#   bash ports/test.sh          (Git Bash; builds nvhost on first use)
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
w="/mnt/$(cygpath -m "$here" | sed -E 's|^([A-Za-z]):|\L\1|')"   # /mnt/d/NucleoV2/ports
apps="${w%/ports}/apps"
export MSYS_NO_PATHCONV=1

cat > "$here/_out/run_tests.sh" <<EOF
#!/bin/bash
set -u
[ -x /root/nvhost ] || bash $w/host/build.sh
out=$w/_out; home=\$out/home
rm -rf \$home; mkdir -p \$home \$out/aot
fail=0
check() {   # name expected-substring output
    if printf '%s' "\$3" | grep -qF -- "\$2"; then echo "  ok   \$1"; else echo "  FAIL \$1 (want: \$2)"; fail=1; fi
}
for id in lua js sqlite3 basic cjson md zip; do
    /root/wamrc-build/wamrc --target=x86_64 --bounds-checks=1 --enable-multi-thread \
        -o \$out/aot/\$id.aot $apps/\$id/app.wasm >/dev/null
done
for mode in wasm aot; do
    echo "== \$mode"
    mod() { [ \$mode = wasm ] && echo $apps/\$1/app.wasm || echo \$out/aot/\$1.aot; }
    run() { local id=\$1; shift; /root/nvhost --dir=/::\$home --mem=8 \$(mod \$id) "\$@" 2>&1; }

    o=\$(printf 'x = 6*7\nprint(x)\nerror("boom")\nprint(pcall(error, "caught"))\nprint("alive")\n' | run lua)
    check "lua repl"            "42" "\$o"
    check "lua error recovery"  "alive" "\$o"
    check "lua pcall"           "false	caught" "\$o"
    printf 'local f = io.open("/t.txt", "w") f:write("hello") f:close()\nprint(io.open("/t.txt"):read("a"))\n' > \$home/s.lua
    check "lua script + files"  "hello" "\$(run lua /s.lua)"

    o=\$(printf 'const a = [1,2,3].map(x => x * 2)\na\nthrow new Error("e1")\nlet o = {\n  k: 1\n}\no\n' | run js)
    check "js repl"             "[ 2, 4, 6 ]" "\$o"
    check "js multi-line"       "{ k: 1 }" "\$o"
    check "js error"            "Error: e1" "\$o"
    printf 'import * as std from "qjs:std";\nstd.writeFile ? 0 : 0;\nconst f = std.open("/j.txt", "w"); f.puts("js-file"); f.close();\nsetTimeout(() => console.log(std.loadFile("/j.txt")), 10);\n' > \$home/m.mjs
    check "js module + timer"   "js-file" "\$(run js /m.mjs)"
    check "js -e"               "3" "\$(run js -e 'console.log(1+2)')"

    rm -f \$home/t.db
    o=\$(printf "create table t(a, b);\ninsert into t values (1, 'uno'), (2, 'due');\nselect b from t where a = 2;\nselect json_object('n', count(*)) from t;\n" | run sqlite3 /t.db)
    check "sqlite query"        "due" "\$o"
    check "sqlite json"         '{"n":2}' "\$o"
    check "sqlite persists"     "uno" "\$(echo 'select b from t where a = 1;' | run sqlite3 /t.db)"

    printf '10 poke 0, 111\n20 peek 0, a\n30 print "peek", a\n40 input b\n50 print "typed", b\n60 for i = 1 to 3\n70 print i\n80 next i\n90 gosub 200\n100 end\n200 print "sub"\n210 return\n' > \$home/p.bas
    o=\$(printf '99\n' | run basic /p.bas)
    check "basic peek/poke"     "peek 111" "\$o"
    check "basic input"         "typed 99" "\$o"
    check "basic for/next"      "1
2
3" "\$o"
    check "basic gosub"         "sub" "\$o"

    printf '{"name":"basic","permissions":["home","net"],"n":2}' > \$home/m.json
    o=\$(run cjson /m.json)
    check "cjson pretty"        '"name":  "basic"' "\$o"
    check "cjson query"         '"home"' "\$(run cjson -q .permissions /m.json)"
    check "cjson compact"       '{"name":"basic"' "\$(run cjson -c /m.json)"
    printf '{"r":"u","t":"hi"}\n{"r":"a","t":"yo"}\n' > \$home/c.jsonl
    check "cjson jsonl"         '"t":  "yo"' "\$(run cjson -l /c.jsonl)"
    check "cjson bad json"      "parse error" "\$(printf '{bad' | run cjson)"

    o=\$(printf '# Hi\n\n**bold** and a [link](http://x).\n\n- a\n- b\n' | run md)
    check "md basic"            "<strong>bold</strong>" "\$o"
    check "md heading"          "<h1>Hi</h1>" "\$o"
    check "md list"             "<li>a</li>" "\$o"
    o=\$(printf -- '- [x] done\n- [ ] todo\n\n| a | b |\n|---|---|\n| 1 | 2 |\n' | run md --gfm)
    check "md gfm tasklist"     'checkbox" class="task-list-item-checkbox" disabled checked' "\$o"
    check "md gfm table"        "<table>" "\$o"

    printf 'hello zip' > \$home/a.txt
    o=\$(run zip /archive.zip /a.txt)
    check "zip create"          "a.txt" "\$o"
    check "zip list"            "a.txt" "\$(run zip -l /archive.zip)"
    rm -rf \$home/out; o=\$(run zip -x /archive.zip /out)
    check "zip extract"         "a.txt" "\$o"
    check "zip extract content" "hello zip" "\$(cat \$home/out/a.txt 2>&1)"
done
exit \$fail
EOF
wsl.exe -d Ubuntu-24.04 -- bash "$w/_out/run_tests.sh"
