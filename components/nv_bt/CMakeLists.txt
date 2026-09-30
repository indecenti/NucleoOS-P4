idf_component_register(
    SRCS "nv_bt.c"
    INCLUDE_DIRS "include"
    LDFRAGMENTS "linker.lf"           # NimBLE host + nv_bt statics -> PSRAM (.bss), see linker.lf
    PRIV_REQUIRES
        bt
        esp_hosted
        nv_kernel
        nv_hal
        esp_timer
        heap
)
