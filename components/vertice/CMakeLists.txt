# Vertice — the NucleoOS 3D engine. core/ is the rasteriser (derived from Jet by CubeCoders, MIT:
# core/LICENSE-Jet), vertice.cpp the engine (scene handles, dual-core bands, WASM-facing API) and
# vertice_alloc.c its PSRAM heap.
file(GLOB VX_CORE_SRCS "${CMAKE_CURRENT_LIST_DIR}/core/*.cpp")

idf_component_register(
    SRCS "vertice.cpp" "vertice_alloc.c" ${VX_CORE_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "." "core"      # "." = JetConfig.hpp, which the core headers include
    PRIV_REQUIRES esp_timer heap freertos log nv_kernel
    LDFRAGMENTS "linker.lf"           # static tables -> PSRAM (.bss), see linker.lf
)

# The rasteriser is pure CPU work over PSRAM: -O2 like the other hot paths (LVGL, WAMR).
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
# The core keeps Jet's code style; its warnings are not ours to fail the build on.
set_source_files_properties(${VX_CORE_SRCS} PROPERTIES COMPILE_OPTIONS "-w")

# Every new/delete/malloc/free reference inside this library -> vertice_alloc.c (PSRAM, metered).
# Applied to the archive after it is built, so the rest of the firmware is unaffected;
# vertice_alloc.syms lists the riscv32 (size_t = unsigned int) manglings.
add_custom_command(TARGET ${COMPONENT_LIB} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} --redefine-syms=${CMAKE_CURRENT_LIST_DIR}/vertice_alloc.syms
            $<TARGET_FILE:${COMPONENT_LIB}>
    VERBATIM)
