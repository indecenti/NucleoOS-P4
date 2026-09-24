idf_component_register(
    SRCS "ss_main.cpp" "ss_engine.cpp" "ss_usb.cpp" "ss_net.cpp" "ss_tls.cpp" "ss_cast.cpp"
         "ss_vnc.cpp" "ss_des.c" "ss_icons.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    EMBED_TXTFILES "web/cast.html" "../../tools/nucleocast/nucleocast.py"
    REQUIRES nv_kernel
    PRIV_REQUIRES nv_hal nv_usb lvgl esp_lvgl_port esp_lcd esp_driver_jpeg esp_driver_ppa esp_mm
                  esp_timer lwip mbedtls mdns esp_netif json esp_app_format esp_rom
)

# Labels/URLs/JSON are built with bounded snprintf on purpose (truncation = the intended clamp).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
