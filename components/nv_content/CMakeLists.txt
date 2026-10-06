idf_component_register(
    SRCS "nv_content.cpp" "nv_content_plan.cpp"
    INCLUDE_DIRS "include"
    REQUIRES nv_appstore nv_kernel nv_hal nv_i18n nv_ota esp_timer
)
