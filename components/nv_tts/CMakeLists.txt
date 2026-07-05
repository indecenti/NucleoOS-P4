idf_component_register(
    SRCS "nv_tts.c" "nucleo_tts_plan.c" "nucleo_tts_index.c"
    INCLUDE_DIRS "include"
    REQUIRES nv_hal nv_kernel
)
# Fixed-size snprintf buffers (truncation intentional / bounded by slug length).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
