idf_component_register(
    SRCS "nv_appstore.cpp"
    INCLUDE_DIRS "include"
    REQUIRES nv_kernel nv_hal nv_wasm nv_i18n esp_http_client esp-tls mbedtls json fatfs
)
