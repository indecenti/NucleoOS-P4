#!/bin/bash
# fetch.sh — download the pinned upstream sources of the openHASP app into ports/_src (not
# committed) and apply ports/openhasp/patches/*.patch to the extracted openHASP tree.
#
#   openHASP   HASwitchPlate/openHASP master @ e896407 (0.7.0.1, MIT)
#   lvgl       HASwitchPlate/lvgl release/v7 @ 0af8c7a (LVGL 7.11 fork openHASP builds with, MIT)
#   lv_lib_png lvgl/lv_lib_png release/v7 @ d42282b (lodepng, zlib; PNG images from L:/)
#   ArduinoJson v6.21.5 (MIT; platformio.ini: bblanchon/ArduinoJson@^6.21.5)
#   freetype   fvanroie/freetype @ 7290cbd (FreeType 2.10.4, FTL; openHASP's lib/freetype submodule)
#
# Everything is pinned to a commit/tag and checked by sha256.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
src="$here/../_src"
mkdir -p "$src"
cd "$src"

get() {   # url file sha256
    if [ ! -f "$2" ]; then
        echo "fetch $1"
        curl -sSfL -o "$2.part" "$1"
        mv "$2.part" "$2"
    fi
    echo "$3  $2" | sha256sum -c --quiet -
}

OPENHASP_SHA=e896407c5ecc81299b90ab0e0e9ba0d41dddc1ba
LVGL_SHA=0af8c7a950157234f96256aec8ec3b761f2d76d8
LVPNG_SHA=d42282b8673cffdc9010289e8ef149bcb6ec013f
FREETYPE_SHA=7290cbd5a582c642538a50ea7bed19a29595d311
AJSON_VER=6.21.5

get "https://github.com/HASwitchPlate/openHASP/archive/$OPENHASP_SHA.tar.gz" "openHASP-$OPENHASP_SHA.tar.gz" \
    ebc55f97b207481adb1b7cc31cb0c5ffdd4a5ee650038cc525d3eb8b5f33e1fb
get "https://github.com/HASwitchPlate/lvgl/archive/$LVGL_SHA.tar.gz" "lvgl-hasp-$LVGL_SHA.tar.gz" \
    c9c23cb943dd317da4481f4ff692db0e5ac343051facb4cf228c275db47215e4
get "https://github.com/lvgl/lv_lib_png/archive/$LVPNG_SHA.tar.gz" "lv_lib_png-$LVPNG_SHA.tar.gz" \
    868a5bbbaa6d84c6765a46f182b4e4264d56603b7b0c32105480fc9ad7fbeb79
get "https://github.com/bblanchon/ArduinoJson/archive/refs/tags/v$AJSON_VER.tar.gz" "ArduinoJson-$AJSON_VER.tar.gz" \
    27cd4891596fa1a70e35851231958ea2b0719a37d0af110e9e085eb5500be0c3
get "https://github.com/fvanroie/freetype/archive/$FREETYPE_SHA.tar.gz" "freetype-hasp-$FREETYPE_SHA.tar.gz" \
    bdacac4c4f0079bd9a2d8e06e9db24a7c62ffd84039c8564329bd835a40d8807

# PC test assets (ports/openhasp/test.sh): the "Widgets Demo" pages.jsonl of the openHASP docs and
# the logo its img object loads (HASwitchPlate/openHASP-docs @ 2f23d1f, MIT).
DOCS_SHA=2f23d1f30111c2ea7aa32ee2e5a3d5d1605206f4
get "https://raw.githubusercontent.com/HASwitchPlate/openHASP-docs/$DOCS_SHA/docs/examples/widgets.md" \
    "openhasp-docs-widgets-$DOCS_SHA.md" c8c78b15a033574c1a4ca2011a7d471b10b81077e92abea448b6aa3c577ec22a
get "https://raw.githubusercontent.com/HASwitchPlate/openHASP-docs/$DOCS_SHA/docs/assets/images/logo-medium.png" \
    "openhasp-docs-logo-medium-$DOCS_SHA.png" 6c6b2c274cb706647aed39b1f484a6f86ad848791a5d466b2f8dc67012088450

mkdir -p openhasp
cd openhasp
# The openHASP tree is (re)extracted and patched whenever a patch is newer than the stamp, so the
# patches in ports/openhasp/patches are the only changes to upstream code.
stamp="openHASP-$OPENHASP_SHA/.nv_patched"
newest=$(ls -t "$here"/patches/*.patch 2>/dev/null | head -1)
if [ ! -f "$stamp" ] || { [ -n "$newest" ] && [ "$newest" -nt "$stamp" ]; }; then
    rm -rf "openHASP-$OPENHASP_SHA"
    tar xzf "../openHASP-$OPENHASP_SHA.tar.gz"
    for p in "$here"/patches/*.patch; do
        [ -f "$p" ] || continue
        patch -s -p1 -d "openHASP-$OPENHASP_SHA" < "$p"
    done
    touch "$stamp"
fi
[ -d "lvgl-$LVGL_SHA" ] || tar xzf "../lvgl-hasp-$LVGL_SHA.tar.gz"
[ -d "lv_lib_png-$LVPNG_SHA" ] || tar xzf "../lv_lib_png-$LVPNG_SHA.tar.gz"
[ -d "ArduinoJson-$AJSON_VER" ] || tar xzf "../ArduinoJson-$AJSON_VER.tar.gz"
[ -d "freetype-$FREETYPE_SHA" ] || tar xzf "../freetype-hasp-$FREETYPE_SHA.tar.gz"
echo "sources ready in $src/openhasp"
