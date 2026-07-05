idf_component_register(
    SRCS
        "nv_ota.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        nv_kernel
        nv_hal
        esp_https_ota
        esp_http_client
        app_update
        esp_partition
        json
        esp-tls
        mbedtls
)
