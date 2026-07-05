idf_component_register(
    SRCS
        "nv_ui.cpp"
        "nv_gesture.cpp"
        "nv_notify.cpp"
        "nv_ime.cpp"
        "nv_keyboard_layouts.c"
        "generated/nv_icons.c"
    INCLUDE_DIRS "include"
    REQUIRES nv_kernel nv_i18n nv_fonts nv_theme nv_hal nv_usb lvgl esp_lvgl_port esp_timer esp_app_format esp_driver_ppa
)
