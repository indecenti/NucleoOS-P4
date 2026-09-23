idf_component_register(
    SRCS "nv_wasm.cpp" "nv_wasm_wasi.c"
    INCLUDE_DIRS "include"
    REQUIRES nv_kernel nv_hal nv_i18n nv_tts wamr pthread json fatfs esp_timer esp_hw_support
             lwip esp_netif vfs
)

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
