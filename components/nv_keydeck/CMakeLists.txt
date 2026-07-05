idf_component_register(
    SRCS
        "nv_keydeck.cpp"
        "nv_cpu_load.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES nv_kernel nv_hal nv_ui lvgl esp_lvgl_port esp_timer lwip esp_netif
)
