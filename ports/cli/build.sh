#!/bin/bash
# build.sh — build the small command-line programs (WASI ports) for the NucleoOS Terminal:
#
#   berry    Berry 1.1.0                  apps/berry/{app.wasm,app.aot,icon.z}
#   wren     Wren 0.4.0 (VM + own REPL)   apps/wren/...
#   tcl      Jim Tcl 0.84 (jimsh)         apps/tcl/...
#   pforth   pForth 2.0.1                 apps/pforth/...
#   bc       bc 7.1.0 (Gavin Howard)      apps/bc/...
#   figlet   FIGlet 2.2.5 (fonts inside)  apps/figlet/...
#   jq       jq 1.8.2 + Oniguruma 6.9.10  apps/jq/...
#   scheme   TinyScheme 1.42              apps/scheme/...
#
#   bash ports/cli/build.sh [berry|wren|...]      (Git Bash on Windows; default: all)
#
# Same toolchain and conventions as ports/build.sh (clang wasm32-wasip1 + wasi-sdk sysroot,
# riscv32 AOT via wamrc in WSL, Terminal-badge icon via ports/make_icon.py). Sources come from
# ports/cli/fetch.sh (pinned, patched). tcl, bc and jq also run their upstream configure step in
# WSL (tcl/jim_generate.sh, bc/bc_generate.sh, jq/jq_generate.sh; needs gcc, make and the Linux
# wasi-sdk in /opt) once per fresh source tree; pforth builds its dictionary under /root/nvhost.
# Interpreters that recover from errors with setjmp/longjmp (berry, bc, Jim's lsort) use the
# host's nv_try_call/nv_throw instead (ports/common/nv_sjlj.h; see each <id>/*.patch).
# The manifests and guides in apps/<id>/ are hand-written; this script only regenerates the
# binaries. Test on the PC first: bash ports/cli/test.sh.
set -euo pipefail
here="$(cygpath -m "$(cd "$(dirname "$0")" && pwd)")"      # D:/.../ports/cli
ports="$(cygpath -m "$(cd "$here/.." && pwd)")"
root="$(cygpath -m "$(cd "$ports/.." && pwd)")"
bash "$here/fetch.sh" >/dev/null

CLANG="${CLANG:-/c/Program Files/LLVM/bin/clang.exe}"
WASI="${WASI:-D:/esp/wasi-sdk-34}"
SYSROOT="$WASI/wasi-sysroot-34.0"
BUILTINS="$WASI/libclang_rt-34.0/wasm32-unknown-wasip1/libclang_rt.builtins.a"
WAMRC="${WAMRC:-/root/wamrc-build/wamrc}"
DISTRO="${DISTRO:-Ubuntu-24.04}"
S="$here/_src"
G="$here/_src/gen"      # generated sources (tables, embedded files)
mkdir -p "$G"

CFLAGS=(--target=wasm32-wasip1 "--sysroot=$SYSROOT" -O2 -nodefaultlibs
        -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS -Wno-deprecated-declarations
        -I"$ports/common")
LDFLAGS=(-Wl,-z,stack-size=262144 -Wl,--strip-all)
LIBS=(-lc -lwasi-emulated-signal -lwasi-emulated-process-clocks -lm)

wsl_path() { echo "/mnt/$(echo "${1:0:1}" | tr 'A-Z' 'a-z')${1:2}"; }   # D:/x -> /mnt/d/x

aot() {
    local wasm="$1" out="${1%.wasm}.aot"
    MSYS_NO_PATHCONV=1 wsl.exe -d "$DISTRO" -- "$WAMRC" --target=riscv32 --target-abi=ilp32f --cpu=generic-rv32 \
        --cpu-features=+m,+a,+c,+f --enable-multi-thread -o "$(wsl_path "$out")" "$(wsl_path "$wasm")" \
        >/dev/null
    local n; n=$(stat -c %s "$out")
    [ "$n" -le 4194304 ] || { echo "$out: $n bytes > 4 MB device cap" >&2; exit 1; }
    echo "  $(basename "$(dirname "$out")")/app.aot  $n bytes"
}

finish() {   # id label bg fg
    local dir="$root/apps/$1" n
    n=$(stat -c %s "$dir/app.wasm")
    [ "$n" -le 2097152 ] || { echo "$1: app.wasm $n bytes > 2 MB device cap" >&2; exit 1; }
    echo "  $1/app.wasm  $n bytes"
    aot "$dir/app.wasm"
    python "$ports/make_icon.py" "$dir/icon.z" "$2" "$3" "$4"
}

build_berry() {
    local B="$S/berry-1.1.0" srcs=()
    # Constant tables (be_const_*.h) generated from the sources by Berry's own coc tool.
    # The sources include them as "../generate/...", so they go in the source tree.
    rm -rf "$B/generate"; mkdir -p "$B/generate"
    python "$B/tools/coc/coc" -o "$B/generate" "$B/src" "$B/default" -c "$here/berry/berry_conf.h" 2>/dev/null
    for f in "$B"/src/*.c "$B"/default/*.c; do srcs+=("$f"); done
    "$CLANG" "${CFLAGS[@]}" -std=c99 -I"$here/berry" -I"$B/src" -I"$B/default" \
        -include "$here/berry/nv_berry_port.h" "${LDFLAGS[@]}" -o "$root/apps/berry/app.wasm" \
        "${srcs[@]}" "$here/berry/nv_berry_shim.c" "${LIBS[@]}" "$BUILTINS"
    finish berry "Berry" "#5b2a86" "#f4c20d"
}

build_wren() {
    local W="$S/wren-0.4.0/src"
    "$CLANG" "${CFLAGS[@]}" -std=c99 -I"$W/include" -I"$W/vm" -I"$W/optional" "${LDFLAGS[@]}" \
        -o "$root/apps/wren/app.wasm" "$W"/vm/*.c "$W"/optional/*.c "$here/wren/nv_wren_main.c" \
        "${LIBS[@]}" "$BUILTINS"
    finish wren "Wren" "#0b3d5c" "#7fdbff"
}

JIM_TCL_EXTS=(binary ensemble glob jsonencode nshelper oo stdlib tclcompat tree initjimsh)

build_tcl() {
    local J="$S/jimtcl-0.84" srcs=()
    # configure + Tcl-to-C generation run in WSL (tcl/jim_generate.sh), the compile here
    [ -f "$J/_initjimsh.c" ] || MSYS_NO_PATHCONV=1 wsl.exe -d "$DISTRO" -- \
        bash "$(wsl_path "$here/tcl/jim_generate.sh")" "$(wsl_path "$J")"
    for f in jim-subcmd jim-interactive jim-format jim utf8 jimregexp jimiocompat jsmn/jsmn jim-nosignal \
             jim-aio jim-array jim-clock jim-file jim-interp jim-json jim-namespace jim-pack jim-package \
             jim-readdir jim-regexp jim-tclprefix _load-static-exts jimsh; do srcs+=("$J/$f.c"); done
    for e in "${JIM_TCL_EXTS[@]}"; do srcs+=("$J/_$e.c"); done
    "$CLANG" "${CFLAGS[@]}" -I"$J" -DUSE_UTF8 -D_GNU_SOURCE -include "$here/tcl/nv_jim_port.h" \
        -Wno-unused-command-line-argument "${LDFLAGS[@]}" -o "$root/apps/tcl/app.wasm" "${srcs[@]}" \
        "$here/tcl/nv_jim_shim.c" "${LIBS[@]}" "$BUILTINS"
    finish tcl "Tcl" "#7a1f1f" "#ffd479"
}

build_pforth() {
    local P="$S/pforth-2.0.1" C="$S/pforth-2.0.1/csrc" D="$G/pforth" srcs=()
    for f in pf_cglue pf_clib pf_core pf_inner pf_io pf_io_none pf_main pf_mem pf_save pf_text \
             pf_words pfcompil pfcustom stdio/pf_io_stdio stdio/pf_fileio_stdio; do srcs+=("$C/$f.c"); done
    srcs+=("$here/pforth/nv_pforth_shim.c")
    local PF=(-DPF_SUPPORT_FP -D_DEFAULT_SOURCE -D_GNU_SOURCE -fsigned-char -Wno-parentheses -I"$C")
    # 1) a dictionary-less pforth builds the dictionary from fth/system.fth, then saves it as C
    #    (pfdicdat.h): run on the PC under nvhost, so the cells are the device's 32-bit ones.
    #    No history.fth: its line editor needs raw keys and ANSI cursor moves (the Terminal gives
    #    whole lines and drops escape sequences), and it echoes what was typed.
    rm -rf "$D"; mkdir -p "$D"; cp -r "$P/fth" "$D/fth"
    sed -i "/include? HISTORY history.fth/d" "$D/fth/loadp4th.fth"
    "$CLANG" "${CFLAGS[@]}" "${PF[@]}" "${LDFLAGS[@]}" -o "$D/dicapp.wasm" "${srcs[@]}" "${LIBS[@]}" "$BUILTINS"
    cat > "$D/mkdic.sh" <<'SH'
set -e
cd "$1"
/root/nvhost --dir=/::fth dicapp.wasm -i system.fth >/dev/null
echo 'include savedicd.fth SDAD BYE' | /root/nvhost --dir=/::fth dicapp.wasm -d pforth.dic >/dev/null
SH
    MSYS_NO_PATHCONV=1 wsl.exe -d "$DISTRO" -- bash "$(wsl_path "$D")/mkdic.sh" "$(wsl_path "$D")"
    # 2) the standalone interpreter with the dictionary compiled in
    "$CLANG" "${CFLAGS[@]}" "${PF[@]}" -DPF_STATIC_DIC -I"$D/fth" "${LDFLAGS[@]}" \
        -o "$root/apps/pforth/app.wasm" "${srcs[@]}" "${LIBS[@]}" "$BUILTINS"
    finish pforth "Forth" "#3b2f2f" "#ff9f43"
}

# The 15 fonts of the release whose headers explicitly allow redistribution/modification.
FIGLET_FONTS=(standard big block bubble digital lean mini script shadow slant small smscript
              smshadow smslant term)

build_figlet() {
    local F="$S/figlet-2.2.5" O="$G/figlet" names=()
    rm -rf "$O"; mkdir -p "$O"
    for n in "${FIGLET_FONTS[@]}"; do names+=("$n.flf"); done
    python "$here/figlet/embed_fonts.py" "$F/fonts" "$O/figlet_fonts.h" "${names[@]}"
    local FF=(-DDEFAULTFONTDIR=\"/figlet-fonts\" -DDEFAULTFONTFILE=\"standard\" -Wno-unused-command-line-argument
              -Wno-deprecated-non-prototype -include unistd.h)
    "$CLANG" "${CFLAGS[@]}" "${FF[@]}" -Dmain=figlet_main '-Dstat(p,s)=nv_fig_stat(p,s)' -c -o "$O/figlet.o" "$F/figlet.c"
    "$CLANG" "${CFLAGS[@]}" "${FF[@]}" -Dfopen=nv_fig_fopen -c -o "$O/zipio.o" "$F/zipio.c"
    for f in crc inflate utf8; do "$CLANG" "${CFLAGS[@]}" "${FF[@]}" -c -o "$O/$f.o" "$F/$f.c"; done
    "$CLANG" "${CFLAGS[@]}" "${FF[@]}" -I"$O" -c -o "$O/nv_figlet_main.o" "$here/figlet/nv_figlet_main.c"
    "$CLANG" "${CFLAGS[@]}" "${LDFLAGS[@]}" -o "$root/apps/figlet/app.wasm" "$O"/*.o "${LIBS[@]}" "$BUILTINS"
    finish figlet "FIG" "#1d3557" "#f1faee"
}

build_jq() {
    local Q="$S/jq-1.8.2" O="$S/onig-6.9.10/src" srcs=() defs
    [ -f "$O/config.h" ] || MSYS_NO_PATHCONV=1 wsl.exe -d "$DISTRO" -- \
        bash "$(wsl_path "$here/jq/jq_generate.sh")" "$(wsl_path "$Q")" "$(wsl_path "$S/onig-6.9.10")"
    # configure's results are the Makefile's DEFS line (-DHAVE_... -DUSE_DECNUM=1 ...)
    eval "defs=($(sed -n 's/^DEFS = //p' "$Q/Makefile"))"
    for f in builtin bytecode compile execute jq_test jv jv_alloc jv_aux jv_dtoa jv_file jv_parse \
             jv_print jv_unicode linker locfile util jv_dtoa_tsd lexer parser main; do srcs+=("$Q/src/$f.c"); done
    srcs+=("$Q/vendor/decNumber/decContext.c" "$Q/vendor/decNumber/decNumber.c")
    # Oniguruma 6.9.10 (regex: test, match, capture, sub, gsub, scan, splits): the library
    # sources of its src/Makefile.am minus the POSIX/GNU API wrappers
    for f in regparse regcomp regexec regenc regerror regext regsyntax regtrav regversion st \
             unicode unicode_unfold_key unicode_fold1_key unicode_fold2_key unicode_fold3_key ascii utf8 \
             utf16_be utf16_le utf32_be utf32_le euc_jp euc_jp_prop sjis sjis_prop iso8859_1 iso8859_2 \
             iso8859_3 iso8859_4 iso8859_5 iso8859_6 iso8859_7 iso8859_8 iso8859_9 iso8859_10 \
             iso8859_11 iso8859_13 iso8859_14 iso8859_15 iso8859_16 euc_tw euc_kr big5 gb18030 koi8_r \
             cp1251 onig_init; do srcs+=("$O/$f.c"); done
    "$CLANG" "${CFLAGS[@]}" "${defs[@]}" -DHAVE_LIBONIG=1 -I"$Q" -I"$Q/src" -I"$Q/vendor" -I"$O" \
        -Wno-unused-command-line-argument \
        "${LDFLAGS[@]}" -o "$root/apps/jq/app.wasm" "${srcs[@]}" "${LIBS[@]}" "$BUILTINS"
    finish jq "jq" "#2d2d2d" "#c8e64c"
}

build_bc() {
    local B="$S/bc-7.1.0" srcs=() flags
    [ -f "$B/nv_cflags.txt" ] || MSYS_NO_PATHCONV=1 wsl.exe -d "$DISTRO" -- \
        bash "$(wsl_path "$here/bc/bc_generate.sh")" "$(wsl_path "$B")"
    read -r -a flags < "$B/nv_cflags.txt"     # no quoting in it: plain -D/-I words
    for f in args bc bc_lex bc_parse data file lang lex main num opt parse program rand read vector \
             vm; do srcs+=("$B/src/$f.c"); done
    srcs+=("$B/gen/lib.c" "$B/gen/lib2.c" "$B/gen/bc_help.c")
    # shim/setjmp.h first: sigsetjmp() never returns twice, errors unwind via nv_throw (bc.patch)
    "$CLANG" "${CFLAGS[@]}" -I"$here/bc/shim" -I"$B/include" "${flags[@]/#-I.\/include\//-I$B/include}" \
        "${LDFLAGS[@]}" -o "$root/apps/bc/app.wasm" "${srcs[@]}" "${LIBS[@]}" "$BUILTINS"
    finish bc "bc" "#0f5132" "#d1e7dd"
}

build_scheme() {
    local T="$S/tinyscheme-1.42" O="$G/scheme"
    rm -rf "$O"; mkdir -p "$O"
    python "$here/scheme/embed_init.py" "$T/init.scm" "$O/scheme_init.h"
    # Library mode (no STANDALONE main, no dynamic loading); math, string ports, error hook on.
    "$CLANG" "${CFLAGS[@]}" -DSTANDALONE=0 -DUSE_DL=0 -DUSE_MATH=1 -DUSE_STRLWR=1 -DUSE_INTERFACE=1 \
        -DUSE_ERROR_HOOK=1 -DUSE_STRING_PORTS=1 -DUSE_ASCII_NAMES=1 -DUSE_CHAR_CLASSIFIERS=1 \
        -DCELL_SEGSIZE=10000 -DCELL_NSEGMENT=30 \
        -Wno-unused-command-line-argument -I"$T" -I"$O" "${LDFLAGS[@]}" -o "$root/apps/scheme/app.wasm" \
        "$T/scheme.c" "$here/scheme/nv_scheme_main.c" "${LIBS[@]}" "$BUILTINS"
    finish scheme "SCM" "#4b0082" "#e0b0ff"
}

targets=("$@")
[ ${#targets[@]} -gt 0 ] || targets=(berry wren tcl pforth scheme bc figlet jq)
for t in "${targets[@]}"; do
    echo "== $t"
    mkdir -p "$root/apps/$t"
    "build_$t"
done
echo "done. Test: bash ports/cli/test.sh"
