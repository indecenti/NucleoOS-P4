idf_component_register(
    SRCS "app_main.cpp"
    INCLUDE_DIRS "."
    REQUIRES nv_kernel nv_i18n nv_theme nv_hal nv_ui nv_apps nv_appstore nv_keydeck nv_mqtt nv_wasm nv_web nv_auth nv_usb nv_camera nv_tts nv_bt nv_telemetry
             esp_hw_support esp_lvgl_port
)
