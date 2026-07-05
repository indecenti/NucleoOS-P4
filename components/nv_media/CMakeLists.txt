idf_component_register(
    SRCS "nv_media.c" "nv_mp3dec.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES nv_hal nv_kernel esp_audio_codec
)

# minimp3 unit gets -O2: the component default (-Os) left the decoder under realtime; decode
# speed IS the feature here. Quiet minimp3's benign warnings so -Werror doesn't trip.
set_source_files_properties(nv_mp3dec.c PROPERTIES COMPILE_OPTIONS
    "-O2;-Wno-unused-function;-Wno-maybe-uninitialized")
