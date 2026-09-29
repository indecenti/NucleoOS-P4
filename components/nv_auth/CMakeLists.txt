idf_component_register(
    SRCS "nv_auth.cpp" "nv_auth_core.cpp"
    INCLUDE_DIRS "include"
    REQUIRES nv_kernel
    PRIV_REQUIRES mbedtls esp_timer esp_hw_support
)
