# NucleoOS Anima — top-level project (ESP-IDF)
cmake_minimum_required(VERSION 3.16)
# WAMR (WebAssembly Micro Runtime) ships its ESP-IDF component inside the cloned reference tree;
# register it as an extra component dir so nv_wasm can REQUIRE `wamr`.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/reference/wasm-micro-runtime/build-scripts/esp-idf/wamr")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Demote GCC's stringop-truncation from error to warning project-wide (must be set BEFORE project()
# so it reaches every component's compile). Every nv_* site that trips it uses the safe
# `strncpy(dst, src, sizeof dst - 1); dst[sizeof dst - 1] = 0;` idiom (explicit NUL), so the
# diagnostic is a false positive. Matches the repo's existing per-component -Wno-error demotions.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=stringop-truncation" APPEND)
# Same rationale for misleading-indentation: a purely stylistic diagnostic the repo already demotes
# per-component (nv_anima). A forced clean rebuild surfaces it in several files; demote globally.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=misleading-indentation" APPEND)

# VERSION feeds the app descriptor (esp_app_get_description()->version) the OTA updater compares
# against and the About page shows. Bump on each release.
project(nucleos-anima VERSION 1.1.81)

# Relocate LVGL's 192 KB builtin heap pool (a static .bss array) out of scarce internal SRAM into
# PSRAM. LVGL reads CONFIG_LV_ATTRIBUTE_LARGE_RAM_ARRAY as the section attribute for that pool but
# exposes no Kconfig knob in this version, so inject it for the LVGL component only. EXT_RAM_BSS_ATTR
# is reachable in LVGL TUs via lv_conf_kconfig.h (includes esp_attr.h) and needs
# CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y (already set). The panel draw/render buffers are
# allocated separately with DMA caps by esp_lvgl_port; this pool only backs widget/object/style
# allocations, which are safe in cached PSRAM. Frees ~192 KB internal SRAM.
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_compile_definitions(${lvgl_lib} PRIVATE "CONFIG_LV_ATTRIBUTE_LARGE_RAM_ARRAY=EXT_RAM_BSS_ATTR")
# LVGL is the UI's hot path — every frame is software-rendered (blend, fill, glyph and image
# draw loops) — yet the project builds at -Os. -O2 on this one component speeds rendering for a
# modest flash cost (the OTA slot has headroom); target options land after the global -Os, so they
# win. -O2 also enables deeper flow analysis: keep its extra diagnostics as warnings in third-party
# code rather than failing the build.
target_compile_options(${lvgl_lib} PRIVATE -O2 -Wno-error=maybe-uninitialized -Wno-error=array-bounds
                       -Wno-error=stringop-overflow)

# Same for the WASM engine: WAMR's fast interpreter runs every app that ships no app.aot, and
# nv_wasm's host side holds the ABI v2 gfx primitives (rect/line/circle/tri/blit/text loops) every
# game frame goes through. nv_wasm is our own code, but -O2's deeper analysis still only warns.
foreach(comp wamr nv_wasm)
    idf_component_get_property(comp_lib ${comp} COMPONENT_LIB)
    target_compile_options(${comp_lib} PRIVATE -O2 -Wno-error=maybe-uninitialized
                           -Wno-error=array-bounds -Wno-error=stringop-overflow)
endforeach()
