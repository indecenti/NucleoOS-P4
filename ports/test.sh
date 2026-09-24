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
for id in lua js sqlite3; do
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
done
exit \$fail
EOF
wsl.exe -d Ubuntu-24.04 -- bash "$w/_out/run_tests.sh"
