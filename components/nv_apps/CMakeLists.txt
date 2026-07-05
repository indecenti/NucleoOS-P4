idf_component_register(
    SRCS
        "nv_apps.cpp"
        "anima_app.cpp"
        "settings_app.cpp"
        "files_app.cpp"
        "diagnostics_app.cpp"
        "calculator_app.cpp"
        "gallery_app.cpp"
        "gallery_jpeg_hw.cpp"
        "gallery_thumb_cache.cpp"
        "notes_app.cpp"
        "apps_app.cpp"
        "secondscreen_app.cpp"
        "tasks_app.cpp"
        "camera_app.cpp"
        "terminal_app.cpp"
        "music_app.cpp"
        "video_app.cpp"
        "recorder_app.cpp"
        "sysmon_app.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES nv_ui nv_kernel nv_i18n nv_fonts nv_theme nv_hal nv_ota lvgl esp_hw_support
             app_update nvs_flash esp_timer spi_flash esp_app_format esp_driver_i2c nv_wasm
             nv_usb nv_camera nv_media nv_vplayer esp_driver_jpeg esp_driver_ppa esp_mm esp_lcd
             esp_lvgl_port nv_anima json nv_sysmon
)
