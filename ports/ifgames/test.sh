#!/bin/bash
# test.sh — play every IF catalog app on the PC (WSL nvhost = the firmware's WAMR, see
# ports/test.sh) with a scripted stdin: the game's opening text must appear, SAVE must write a file
# in the app's "/" (a fresh folder per game), and the program must not hang.
# games.json "test": {"expect": text to find, "pre": keys to get past a title menu,
# "script": full stdin instead of the default} — both printf formats ("\n" = Enter).
#
#   bash ports/ifgames/test.sh [slug ...]      (Git Bash; run ports/test.sh once to build nvhost)
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
w="/mnt/$(cygpath -m "$here" | sed -E 's|^([A-Za-z]):|\L\1|')"   # /mnt/d/NucleoV2/ports/ifgames
root="${w%/ports/ifgames}"
export MSYS_NO_PATHCONV=1
mkdir -p "$here/../_out"
python - "$(cygpath -m "$here")/games.json" "$@" > "$here/../_out/if_tests.txt" <<'PY'
import json, sys
sel = sys.argv[2:]
for g in json.load(open(sys.argv[1], encoding="utf-8")):
    if not sel or g["slug"] in sel:
        t = g["test"]
        script = t.get("script", t.get("pre", "") + r"look\ninventory\nsave\nnvtest\nquit\ny\ny\n")
        script = script.replace("\n", r"\n")   # real newlines -> printf escapes (one line per game)
        print(g["slug"], t["expect"].replace(" ", "\x01"), script.replace(" ", "\x01"))
PY
cat > "$here/../_out/if_run.sh" <<EOF
#!/bin/bash
set -u
out=$root/ports/_out/ifhome; fail=0; n=0
while read -r slug expect script; do
    script=\${script%\$'\r'}; expect=\${expect//\$'\x01'/ }; script=\${script//\$'\x01'/ }
    home=\$out/\$slug; rm -rf \$home; mkdir -p \$home; n=\$((n+1))
    o=\$(printf "\$script" | timeout 60 /root/nvhost --dir=/::\$home --mem=8 $root/apps/if-\$slug/app.wasm 2>&1)
    rc=\$?
    saved=\$(ls \$home 2>/dev/null | head -1)
    # the game's own text must appear (the first 3 lines are our banner, which repeats the title)
    if printf '%s' "\$o" | tail -n +4 | grep -qF -- "\$expect" && [ -n "\$saved" ] && [ \$rc -ne 124 ]; then
        echo "  ok   if-\$slug  (save: \$saved, exit \$rc)"
    else
        echo "  FAIL if-\$slug  expect='\$expect' save='\$saved' exit=\$rc"; printf '%s\n' "\$o" | tail -15 | sed 's/^/       | /'; fail=1
    fi
done < $root/ports/_out/if_tests.txt
echo "\$n games tested"
exit \$fail
EOF
wsl.exe -d Ubuntu-24.04 -- bash "$root/ports/_out/if_run.sh"
