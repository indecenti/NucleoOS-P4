idf_component_register(
    SRCS
        "nv_ota.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        nv_fwup      # signed manifests, verified staging (shared with recovery/)
        nv_kernel
        nv_hal
        esp_http_client
        app_update
        esp_partition
        fatfs
        json
        esp-tls
        mbedtls
)
