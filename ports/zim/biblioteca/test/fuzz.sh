#!/bin/bash
# fuzz.sh — the libFuzzer run of test/fuzz_img.c (the picture decoders and the SVG rewriting of img.c):
#   bash ports/zim/biblioteca/test/fuzz.sh [SECONDS]     (Git Bash; builds and runs in WSL with clang)
# Any crash, leak or timeout fails; findings stay in ports/zim/_out/bib_fuzz.
set -euo pipefail
T="$(cd "$(dirname "$0")" && pwd)"
B="$(cd "$T/.." && pwd)"
Z="$(cd "$B/.." && pwd)"
bash "$Z/fetch.sh" >/dev/null
O="$Z/_out/bib_fuzz"
mkdir -p "$O/seeds" "$O/corpus"
python "$(cygpath -m "$T/make_seeds.py")" "$(cygpath -m "$O/seeds")" >/dev/null
wsl_path() { local p; p="$(cygpath -m "$1")"; echo "/mnt/$(echo "${p:0:1}" | tr 'A-Z' 'a-z')${p:2}"; }
cat > "$O/run.sh" <<EOF
#!/bin/bash
set -u
Z=$(wsl_path "$Z"); B=\$Z/biblioteca; S=\$Z/_src; W=\$S/libwebp-1.6.0; O=\$Z/_out/bib_fuzz
BIN=/tmp/bib_fuzz_img
if [ ! -x \$BIN ] || [ \$(stat -c %Y \$B/img.c) -gt \$(stat -c %Y \$BIN) ] || [ \$(stat -c %Y \$B/test/fuzz_img.c) -gt \$(stat -c %Y \$BIN) ] \
   || [ \$(stat -c %Y \$S/nanosvgrast_nv.h) -gt \$(stat -c %Y \$BIN) ]; then
  clang -O1 -g -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -w \\
    -DLODEPNG_NO_COMPILE_ENCODER -DLODEPNG_NO_COMPILE_DISK -DLODEPNG_NO_COMPILE_ANCILLARY_CHUNKS -DLODEPNG_NO_COMPILE_CPP \\
    -I\$B -I\$Z/nvzim -I\$S -I\$W -I\$W/src -o \$BIN \$B/test/fuzz_img.c \$B/img.c \$S/lodepng.c \\
    \$W/src/dec/*.c \$W/src/dsp/{alpha_processing,cpu,dec,filters,rescaler,upsampling,yuv}*.c \$W/src/dsp/lossless.c \\
    \$W/src/dsp/lossless_{avx2,mips_dsp_r2,msa,neon,sse2,sse41}.c \\
    \$W/src/utils/{bit_reader_utils,color_cache_utils,filters_utils,huffman_utils,palette,quant_levels_dec_utils}.c \\
    \$W/src/utils/{random_utils,rescaler_utils,thread_utils,utils}.c -lm || exit 2
fi
cd \$O && \$BIN -max_total_time=\${1:-60} -timeout=10 -rss_limit_mb=2048 -max_len=65536 -print_final_stats=1 \\
  -artifact_prefix=\$O/ \$O/corpus \$O/seeds > \$O/log.txt 2>&1
grep -E "^#[0-9]+ +DONE|stat::number_of_executed_units|SUMMARY" \$O/log.txt
for f in \$O/crash-* \$O/leak-* \$O/timeout-* \$O/oom-*; do [ -e "\$f" ] && { echo "FUZZ FAIL: \$f (\$O/log.txt)"; exit 1; }; done
echo "FUZZ OK"
EOF
MSYS_NO_PATHCONV=1 wsl.exe -d "${DISTRO:-Ubuntu-24.04}" -- bash "$(wsl_path "$O/run.sh")" "${1:-60}"
