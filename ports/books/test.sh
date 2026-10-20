#!/bin/bash
# test.sh — the Libri app on the PC before the board (Git Bash; builds and runs in WSL): the engine
# checked on every fixture book (test/book_check.c), then the app built natively with ASan/UBSan
# against test/native_host.c and driven by the scripts in test/*.script, with screenshots as PNG
# in ports/books/_out/shots/<script>/.
#
#   bash ports/books/test.sh [SCRIPT...]      (default: every script)
# Environment: FIXTURES (WSL path, default /mnt/d/bookstest/fixtures: downloaded public-domain books
# plus the ones test/make_fixtures.py makes), BK_LANG (default it).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
bash "$here/fetch.sh" >/dev/null
G="$here/_out/gen"
mkdir -p "$G" "$here/_out/shots"
python "$(cygpath -m "$here/gen_icons.py")" "$(cygpath -m "$G/icons.h")" >/dev/null
python "$(cygpath -m "$here/gen_fontdata.py")" "$(cygpath -m "$G/fontdata.c")" >/dev/null
wsl_path() { local p; p="$(cygpath -m "$1")"; echo "/mnt/$(echo "${p:0:1}" | tr 'A-Z' 'a-z')${p:2}"; }
scripts=()
for s in "$@"; do scripts+=("$s"); done
rc=0
MSYS_NO_PATHCONV=1 wsl.exe -d "${DISTRO:-Ubuntu-24.04}" -- env FIXTURES="${FIXTURES:-/mnt/d/bookstest/fixtures}" \
    BK_LANG="${BK_LANG:-it}" bash "$(wsl_path "$here/test/run_native.sh")" "${scripts[@]}" || rc=$?
python - "$(cygpath -m "$here/_out/shots")" <<'EOF'
import os, sys
from PIL import Image
root = sys.argv[1]
n = 0
for d, _, files in os.walk(root):
    for f in files:
        if f.endswith(".ppm"):
            p = os.path.join(d, f)
            Image.open(p).save(p[:-4] + ".png")
            os.remove(p)
            n += 1
print("screenshots: %d PNG in %s" % (n, root))
EOF
exit $rc
