#!/bin/bash
# fuzz_cat.sh — the libFuzzer run of test/fuzz_cat.c (cat.c, the Kiwix catalog parser), seeded with the
# real entries of test/opds_ita.xml:
#   bash ports/zim/biblioteca/test/fuzz_cat.sh [SECONDS]     (Git Bash; builds and runs in WSL with clang)
# Any crash, leak or timeout fails; findings stay in ports/zim/_out/bib_fuzz_cat.
set -euo pipefail
T="$(cd "$(dirname "$0")" && pwd)"
Z="$(cd "$T/../.." && pwd)"
O="$Z/_out/bib_fuzz_cat"
mkdir -p "$O/seeds" "$O/corpus"
cp "$T/opds_ita.xml" "$O/seeds/"
wsl_path() { local p; p="$(cygpath -m "$1")"; echo "/mnt/$(echo "${p:0:1}" | tr 'A-Z' 'a-z')${p:2}"; }
cat > "$O/run.sh" <<EOF
#!/bin/bash
set -u
Z=$(wsl_path "$Z"); B=\$Z/biblioteca; O=\$Z/_out/bib_fuzz_cat
BIN=/tmp/bib_fuzz_cat
clang -O1 -g -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -w -I\$B -I\$Z/nvzim \\
  -o \$BIN \$B/test/fuzz_cat.c \$B/cat.c || exit 2
cd \$O && \$BIN -max_total_time=\${1:-60} -timeout=10 -rss_limit_mb=2048 -max_len=65536 -print_final_stats=1 \\
  -artifact_prefix=\$O/ \$O/corpus \$O/seeds > \$O/log.txt 2>&1
grep -E "^#[0-9]+ +DONE|stat::number_of_executed_units|SUMMARY" \$O/log.txt
for f in \$O/crash-* \$O/leak-* \$O/timeout-* \$O/oom-*; do [ -e "\$f" ] && { echo "FUZZ FAIL: \$f (\$O/log.txt)"; exit 1; }; done
echo "FUZZ OK"
EOF
MSYS_NO_PATHCONV=1 wsl.exe -d "${DISTRO:-Ubuntu-24.04}" -- bash "$(wsl_path "$O/run.sh")" "${1:-60}"
