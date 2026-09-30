#!/bin/bash
# Build svmhost (PC test host for the ScummVM port) inside WSL, against the same libiwasm.a as
# ports/host/build.sh (build that first: it configures WAMR like the firmware).
set -e
here="$(cd "$(dirname "$0")" && pwd)"
wamr="$(cd "$here/../../../reference/wasm-micro-runtime" && pwd)"
lib=/root/nvhost-lib2
[ -f "$lib/libiwasm.a" ] || bash "$here/../../host/build.sh"
gcc -O2 -I"$wamr/core/iwasm/include" -o /root/svmhost "$here/svmhost.c" "$lib/libiwasm.a" -lm -lpthread -ldl
echo "OK /root/svmhost"
