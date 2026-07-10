idf_component_register(
    SRCS "nv_wasm.cpp"
    INCLUDE_DIRS "include"
    REQUIRES nv_kernel nv_hal nv_i18n nv_tts wamr pthread json fatfs esp_timer esp_hw_support
             lwip esp_netif
)
