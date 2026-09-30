idf_component_register(
    SRCS "nv_telemetry.cpp"
    INCLUDE_DIRS "include"
    REQUIRES nv_kernel
    PRIV_REQUIRES nv_hal nv_i18n esp_http_client esp-tls mbedtls app_update esp_app_format esp_timer
)
