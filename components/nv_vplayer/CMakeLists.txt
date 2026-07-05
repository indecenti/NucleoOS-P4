idf_component_register(
    SRCS "nv_vplayer.c" "nv_mpeg1_impl.c"
    INCLUDE_DIRS "include"
    REQUIRES nv_hal nv_kernel esp_driver_jpeg esp_driver_ppa esp_mm esp_h264 esp_timer esp_audio_codec
)

# pl_mpeg is the hot path (IDCT/motion-comp/VLC). Build it at -O3 (aggressive: unrolling + more
# inlining of the tight per-block loops) while the rest of the project stays -Os. Same trick the
# music player uses for minimp3.
set_source_files_properties(nv_mpeg1_impl.c PROPERTIES COMPILE_OPTIONS
    "-O3;-Wno-error=incompatible-pointer-types")   # bitbank2 fork uses int* where int32_t* (both 32-bit here) — benign
