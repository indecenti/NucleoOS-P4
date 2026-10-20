idf_component_register(
    SRCS "nv_dl.cpp" "nv_dl_core.c"
    INCLUDE_DIRS "include"
    REQUIRES nv_kernel nv_hal nv_ota esp_http_client esp-tls mbedtls lwip esp_timer esp_app_format
)
