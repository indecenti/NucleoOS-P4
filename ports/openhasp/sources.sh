# sources.sh — sourced by build.sh (wasm) and test.sh (native harness), inside WSL. Needs $root,
# $here and $S set. Defines the upstream paths, the openHASP configuration (DEFS), the include
# path (INCS), gen_ttf and compile_all (parallel, cached; fills OBJS).

OPENHASP_SHA=e896407c5ecc81299b90ab0e0e9ba0d41dddc1ba
OH="$S/openHASP-$OPENHASP_SHA"
LV="$S/lvgl-0af8c7a950157234f96256aec8ec3b761f2d76d8"
PNG="$S/lv_lib_png-d42282b8673cffdc9010289e8ef149bcb6ec013f"
AJ="$S/ArduinoJson-6.21.5"
FT="$S/freetype-7290cbd5a582c642538a50ea7bed19a29595d311"

# openHASP as its "PC" target, like upstream's user_setups/linux/linux_headless.ini, plus the
# ESP32-S3 font setup (FreeType + openhasp.ttf, the 4 Roboto Condensed "all" bitmap fonts).
# The panel: 1024x600 RGB565 (LV_COLOR_DEPTH 16 in include/lv_conf_v7.h, no byte swap: the canvas
# is little-endian RGB565 like lv_color_t).
DEFS=(
    -DHASP_TARGET_PC=1 -DPOSIX -DNUCLEO -DHASP_USE_NULL_DRIVER=1
    -DHASP_MODEL=NucleoOS -D'PIOENV="nucleoos"'
    -DHASP_VER_MAJ=0 -DHASP_VER_MIN=7 -DHASP_VER_REV=0.1
    -DTFT_WIDTH=1024 -DTFT_HEIGHT=600
    -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -DLV_COMP_CONF_INCLUDE_SIMPLE -DLV_SYMBOL_DEF_H
    -DLODEPNG_NO_COMPILE_ALLOCATORS -DLV_PNG_USE_LV_FILESYSTEM=1 -DLV_USE_FILESYSTEM=1 -DLV_USE_FS_IF=1
    -DARDUINOJSON_DECODE_UNICODE=1 -DARDUINOJSON_ENABLE_COMMENTS=1
    -DHASP_NUM_PAGES=12 -DHASP_USE_SPIFFS=0 -DHASP_USE_LITTLEFS=0 -DHASP_USE_EEPROM=0 -DHASP_USE_GPIO=0
    -DHASP_USE_CONFIG=1 -DHASP_USE_DEBUG=1 -DHASP_USE_CONSOLE=0 -DHASP_USE_MQTT=1
    -DHASP_USE_PNGDECODE=1 -DHASP_USE_BMPDECODE=0 -DHASP_USE_GIFDECODE=0 -DHASP_USE_JPGDECODE=0
    -DHASP_USE_QRCODE=0 -DMQTT_MAX_PACKET_SIZE=8192
    -DHASP_ATTRIBUTE_FAST_MEM= -DIRAM_ATTR= -DPROGMEM=
    -DHASP_USE_FREETYPE=1 -DLV_USE_FREETYPE=1 -DLV_FREETYPE_SBIT_CACHE=1 -DLV_FREETYPE_CACHE_SIZE=1
    -DLV_USE_FT_CACHE_MANAGER=1 -DLVGL_FREETYPE_MAX_FACES=16 -DLVGL_FREETYPE_MAX_SIZES=16
    -DLVGL_FREETYPE_MAX_BYTES=2048 -DLVGL_FREETYPE_MAX_BYTES_PSRAM=262144
    -DLV_MEM_CUSTOM=1 -DLV_VDB_SIZE=204800 -DLV_IMG_CACHE_DEF_SIZE_PSRAM=12
    -D'LV_ASSERT_HANDLER=__builtin_trap();'
    -DHASP_PC_LOG_LEVEL=4   # LOG_LEVEL_WARNING: upstream's PC build prints every level to stdout
)

INCS=(
    -I"$here/shim" -I"$FT/devel" -I"$FT/include" -I"$OH/include" -I"$OH/src"
    -I"$OH/lib/lv_fs_if" -I"$OH/lib/lv_lib_freetype" -I"$LV" -I"$LV/src" -I"$PNG" -I"$AJ/src" -I"$root/sdk/include"
)

# "lang|source|extra flags"
sources() {
    local f
    for f in $(find "$LV/src" -name '*.c' | sort); do echo "c|$f|"; done
    echo "c|$PNG/lodepng.c|"
    echo "c|$PNG/lv_png.c|"
    # FreeType modules of openHASP's build (fvanroie/freetype library.json + devel/ftmodule.h:
    # TrueType, SFNT, smooth rasterizer, cache)
    for f in base/ftsystem.c base/ftinit.c base/ftdebug.c base/ftbase.c base/ftbbox.c base/ftglyph.c base/ftbitmap.c \
             truetype/truetype.c sfnt/sfnt.c smooth/smooth.c cache/ftcache.c; do
        echo "c|$FT/src/$f|-DFT2_BUILD_LIBRARY"
    done
    echo "c|$OH/lib/lv_fs_if/lv_fs_if.c|"
    echo "c|$OH/lib/lv_fs_if/lv_fs_pc.c|"
    echo "c|$OH/lib/lv_lib_freetype/lv_freetype.c|"
    echo "c|$OH/lib/lv_lib_freetype/lv_fs_freetype.c|"
    echo "c|$OH/src/font/unscii_8_icon.c|"
    echo "c|$OH/src/hasp/lv_theme_hasp.c|"
    for f in "$OH"/src/font/all/*.cpp; do echo "cxx|$f|"; done
    echo "cxx|$OH/src/font/hasp_font_loader.cpp|"
    for f in "$OH"/src/hasp/*.cpp; do echo "cxx|$f|"; done
    for f in hasp_config.cpp hasp_debug.cpp hasp_gui.cpp hasp_oobe.cpp main.cpp; do echo "cxx|$OH/src/$f|"; done
    for f in nv_hasp_device.cpp nv_hasp_mqtt.cpp nv_hasp_main.cpp nv_hasp_tz.cpp; do echo "cxx|$here/shim/$f|"; done
}

gen_ttf() {   # out.c: data/openhasp.ttf as a C array
    local out="$1" ttf="$OH/data/openhasp.ttf"
    [ -f "$out" ] && [ "$out" -nt "$ttf" ] && return 0
    python3 - "$ttf" "$out" <<'PY'
import sys
b = open(sys.argv[1], "rb").read()
with open(sys.argv[2], "w") as f:
    f.write("/* generated from openHASP data/openhasp.ttf (Roboto Condensed + Material Design Icons) */\n")
    f.write("#include <stddef.h>\n#include <stdint.h>\n")
    f.write("const uint8_t openhasp_ttf[%d] = {\n" % len(b))
    for i in range(0, len(b), 24):
        f.write(",".join(str(x) for x in b[i:i + 24]) + ",\n")
    f.write("};\nconst size_t openhasp_ttf_len = %d;\n" % len(b))
PY
}

# compile_all objdir cc cxx cflags_var cxxflags_var [extra sources...] -> OBJS
# Objects are rebuilt when the source is newer or the flags changed (then the cache is dropped).
compile_all() {
    local objdir="$1" cc="$2" cxx="$3"
    local -n _cf="$4" _xf="$5"
    shift 5
    mkdir -p "$objdir"
    # response files: one argument per line, double quotes escaped (clang/gcc unquote them)
    printf '%s\n' "${_cf[@]}" | sed 's/"/\\"/g' > "$objdir/c.rsp.new"
    printf '%s\n' "${_xf[@]}" | sed 's/"/\\"/g' > "$objdir/cxx.rsp.new"
    if ! cmp -s "$objdir/c.rsp.new" "$objdir/c.rsp" || ! cmp -s "$objdir/cxx.rsp.new" "$objdir/cxx.rsp"; then
        find "$objdir" -name '*.o' -delete
    fi
    # header changes are not tracked: a re-patched openHASP tree or a changed shim header drops
    # the cache too
    for h in "$OH/.nv_patched" "$here"/shim/*.h "$here"/shim/wasi/*.h; do
        if [ "$h" -nt "$objdir/c.rsp" ]; then find "$objdir" -name '*.o' -delete; break; fi
    done
    mv "$objdir/c.rsp.new" "$objdir/c.rsp"
    mv "$objdir/cxx.rsp.new" "$objdir/cxx.rsp"

    OBJS=()
    local jobs="$objdir/jobs.txt" line lang src extra o
    : > "$jobs"
    {
        sources
        for f in "$@"; do echo "c|$f|"; done
    } | while IFS='|' read -r lang src extra; do
        o="$objdir/$(echo "${src#$root/}" | sed 's|^ports/_src/openhasp/||; s|[/.]|_|g').o"
        echo "$o" >> "$objdir/objs.txt.tmp"
        if [ ! -f "$o" ] || [ "$src" -nt "$o" ]; then
            if [ "$lang" = c ]; then echo "$cc @$objdir/c.rsp $extra -c $src -o $o"
            else echo "$cxx @$objdir/cxx.rsp $extra -c $src -o $o"; fi >> "$jobs"
        fi
    done
    mapfile -t OBJS < "$objdir/objs.txt.tmp"
    rm -f "$objdir/objs.txt.tmp"
    local n; n=$(wc -l < "$jobs")
    if [ "$n" -gt 0 ]; then
        echo "  compiling $n files"
        if ! xargs -P "$(nproc)" -I{} -d '\n' sh -c '{} 2>>'"$objdir"'/errors.log || exit 255' < "$jobs"; then
            grep -m 30 -B2 -A4 "error" "$objdir/errors.log" || tail -40 "$objdir/errors.log"
            rm -f "$objdir/errors.log"
            return 1
        fi
        rm -f "$objdir/errors.log"
    fi
}
