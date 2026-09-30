idf_component_register(
    SRCS
        "nv_mqtt.cpp"
        "nv_ha_proto.c"
        "nv_mqtt_topic.c"
    INCLUDE_DIRS "include" "."
    REQUIRES nv_kernel
    PRIV_REQUIRES nv_hal nv_ui nv_i18n nv_tts nv_sysmon nv_ota mqtt mdns lvgl esp_lvgl_port
                  esp_timer esp_netif lwip efuse esp_ringbuf
)
