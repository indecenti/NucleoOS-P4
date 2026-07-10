idf_component_register(
    SRCS "nv_web.cpp"
    INCLUDE_DIRS "include"
    REQUIRES nv_kernel nv_hal nv_wasm nv_anima nv_apps nv_media nv_vplayer nv_tts nv_ui nv_sysmon esp_lvgl_port esp_http_server esp_netif esp_timer esp_app_format esp_system mdns fatfs
)
