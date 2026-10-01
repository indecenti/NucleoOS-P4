idf_component_register(
    SRCS
        "nv_ui.cpp"
        "nv_gesture.cpp"
        "nv_notify.cpp"
        "nv_open.cpp"
        "nv_ime.cpp"
        "nv_ui_focus.cpp"
        "nv_ui_classic.cpp"
        "nv_ui_filesearch.cpp"
        "nv_keyboard_layouts.c"
        "generated/nv_icons.c"
        "generated/nv_logo.c"
    INCLUDE_DIRS "include"
    REQUIRES nv_kernel nv_i18n nv_fonts nv_theme nv_hal nv_usb nv_ota nv_auth lvgl esp_lvgl_port esp_timer esp_app_format esp_driver_ppa esp_driver_jpeg
    PRIV_REQUIRES nv_secondscreen nv_telemetry
)
