idf_component_register(
    SRCS "nv_wasm.cpp" "nv_wasm_wasi.c" "nv_wasm_w4.cpp" "w4/w4_framebuffer.c" "w4/w4_apu.c"
    INCLUDE_DIRS "include"
    REQUIRES nv_kernel nv_hal nv_i18n nv_tts wamr pthread json fatfs esp_timer esp_hw_support
             lwip esp_netif vfs
)

# WASM-4 rasterizer + APU are upstream sources kept verbatim (w4/LICENSE): silence the
# pedantic warnings they were never written against instead of diverging from upstream.
set_source_files_properties("w4/w4_framebuffer.c" "w4/w4_apu.c" PROPERTIES COMPILE_OPTIONS
    "-Wno-sign-compare;-Wno-double-promotion;-Wno-unused-function;-Wno-missing-field-initializers")

# WASI on FATFS (see nv_wasm_wasi.h): WAMR's esp-idf platform only implements the *at() calls
# and fdopendir for LittleFS and leaves ENOSYS stubs otherwise, and its nanosleep stub never
# sleeps. Redirect every reference to them to nv_wasm_wasi.c; the originals stay reachable as
# __real_*. "-u" pulls that object in even though nothing references the __wrap_ symbols by name.
if(CONFIG_WAMR_ENABLE_LIBC_WASI)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u __wrap_openat")
    foreach(sym openat fstatat mkdirat unlinkat renameat fdopendir closedir nanosleep)
        target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=${sym}")
    endforeach()
endif()
