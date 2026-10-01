#!/bin/bash
# build_linux.sh — the Lua App engine (apps/luaapp/app.wasm) on Linux/WSL, same steps and flags as
# build.sh (Git Bash on Windows) without the riscv32 AOT (build.sh / LOCAL_AGENT_TODO.md does that).
# Needs: wasi-sdk 27 in ports/_src (ports/micropython/build.sh fetches it), Python 3 + Pillow,
# ports/_src/lua-5.4.9/src and ports/_src/miniz-3.1.2 (ports/fetch.sh, or git clones of the tags).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
WASI="${WASI_SDK:-$root/ports/_src/wasi-sdk-27.0-x86_64-linux}"
CLANG="$WASI/bin/clang"
SYSROOT="$WASI/share/wasi-sysroot"
BUILTINS="$WASI/lib/clang/20/lib/wasm32-unknown-wasip1/libclang_rt.builtins.a"
LUA="$root/ports/_src/lua-5.4.9/src"
GEN="$here/gen"
mkdir -p "$GEN"
# the generated headers, exactly as build.sh's gen()
sed -n '/^gen() {/,/^}/p' "$here/build.sh" | sed '1d;$d' | sed "s#\$here#$here#g; s#\$GEN#$GEN#g; s#^ *python #python3 #" > "$GEN/gen.sh"
bash "$GEN/gen.sh"
srcs=()
for f in "$LUA"/*.c; do case "$(basename "$f")" in lua.c|luac.c|lobject.c) ;; *) srcs+=("$f") ;; esac; done
python3 - "$LUA/lobject.c" "$GEN/lobject.c" <<'PY'
import sys
s = open(sys.argv[1]).read()
a = 'if (buff[strspn(buff, "-0123456789")] == '
assert s.count(a) == 1, "lobject.c patch does not apply"
s = "int nv_lua51_numbers;\n" + s.replace(a, "if (!nv_lua51_numbers && " + a[4:], 1)
open(sys.argv[2], "w").write(s)
PY
srcs+=("$GEN/lobject.c")
out="$root/apps/luaapp/app.wasm"
"$CLANG" --target=wasm32-wasip1 "--sysroot=$SYSROOT" -O2 -nodefaultlibs -mexec-model=reactor \
    -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS -DLUA_COMPAT_5_3 -Wno-deprecated-declarations \
    -I"$root/ports/common" -I"$root/ports/lua/shim" -I"$root/ports/lua" -I"$LUA" -I"$here" \
    -I"$root/sdk/include" -I"$root/ports/_src/miniz-3.1.2" -I"$root/ports/miniz" -include nv_lua_port.h \
    -Wl,--export=run -Wl,-z,stack-size=262144 -Wl,--strip-all -o "$out" \
    "${srcs[@]}" "$root/ports/lua/nv_lua_glue.c" "$here/luaapp.c" "$here/gfx.c" \
    -lc -lwasi-emulated-signal -lwasi-emulated-process-clocks "$BUILTINS"
n=$(stat -c %s "$out")
[ "$n" -le 2097152 ] || { echo "app.wasm $n bytes > 2 MB device cap" >&2; exit 1; }
echo "  luaapp/app.wasm  $n bytes"
