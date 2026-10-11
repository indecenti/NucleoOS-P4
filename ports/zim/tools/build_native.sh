#!/bin/bash
# build_native.sh — zimdump (nvzim on the PC) with gcc in WSL: ports/zim/_out/native/zimdump.
#   bash ports/zim/tools/build_native.sh        (from Git Bash it re-runs itself in WSL)
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
case "$(uname -s)" in
MINGW*|MSYS*)
    bash "$here/../fetch.sh" >/dev/null
    p="$(cygpath -m "$here/build_native.sh")"
    exec env MSYS_NO_PATHCONV=1 wsl.exe -d "${DISTRO:-Ubuntu-24.04}" -- bash "/mnt/$(echo "${p:0:1}" | tr 'A-Z' 'a-z')${p:2}" ;;
esac
Z="$(cd "$here/.." && pwd)"
S="$Z/_src"
O="$Z/_out/native"
X="$S/xz-embedded-20240322"
mkdir -p "$O"
gcc -O2 -g -Wall -Wextra -Wno-unused-parameter -DZSTD_DISABLE_ASM -DZSTD_LEGACY_SUPPORT=0 -DXZ_USE_CRC64 \
    -I"$Z/nvzim" -I"$S/zstd-1.5.7/lib" -I"$X/linux/include/linux" -I"$X/userspace" \
    -o "$O/zimdump" "$Z/tools/zimdump.c" "$Z/nvzim/nvzim.c" "$Z/nvzim/nvzim_md5.c" \
    "$S"/zstd-1.5.7/lib/common/*.c "$S"/zstd-1.5.7/lib/decompress/*.c \
    "$X"/linux/lib/xz/xz_crc32.c "$X"/linux/lib/xz/xz_crc64.c "$X"/linux/lib/xz/xz_dec_stream.c \
    "$X"/linux/lib/xz/xz_dec_lzma2.c 2>&1 | grep -v -e 'zstd-1.5.7/lib' -e 'xz-embedded' || true
ls -l "$O/zimdump"
