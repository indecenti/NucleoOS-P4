#!/bin/bash
# test.sh — the Biblioteca app on the PC before the board (Git Bash; builds and runs in WSL):
# app.c and the rest built natively with ASan/UBSan against test/native_host.c, driven by the
# scripts in test/*.script over the ZIM fixtures (docs/ZIM_READER_PLAN.md §1b, $FIXTURES), with
# screenshots as PNG in ports/zim/_out/bib_shots/<script>/.
#
#   bash ports/zim/biblioteca/test.sh [SCRIPT...]      (default: every script)
# Environment: FIXTURES (WSL path, default /mnt/d/zimtest/fixtures), BIB_LANG (default it).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
Z="$(cd "$here/.." && pwd)"
bash "$Z/fetch.sh" >/dev/null
G="$Z/_out/bib_gen"
mkdir -p "$G" "$Z/_out/bib_shots"
python "$(cygpath -m "$here/gen_icons.py")" "$(cygpath -m "$G/icons.h")" >/dev/null
python "$(cygpath -m "$here/gen_fontdata.py")" "$(cygpath -m "$G/fontdata.c")" >/dev/null
wsl_path() { local p; p="$(cygpath -m "$1")"; echo "/mnt/$(echo "${p:0:1}" | tr 'A-Z' 'a-z')${p:2}"; }
scripts=()
for s in "$@"; do scripts+=("$s"); done
MSYS_NO_PATHCONV=1 wsl.exe -d "${DISTRO:-Ubuntu-24.04}" -- env FIXTURES="${FIXTURES:-/mnt/d/zimtest/fixtures}" \
    BIB_LANG="${BIB_LANG:-it}" bash "$(wsl_path "$here/test/run_native.sh")" "${scripts[@]}"
# PPM -> PNG (Pillow on the Windows side)
python - "$(cygpath -m "$Z/_out/bib_shots")" <<'EOF'
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
