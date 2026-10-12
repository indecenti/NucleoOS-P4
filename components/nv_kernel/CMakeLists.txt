idf_component_register(
    SRCS
        "nv_log.cpp"
        "nv_event_bus.cpp"
        "nv_service_mgr.cpp"
        "nv_memory_broker.cpp"
        "nv_config.cpp"
        "nv_config_secure.cpp"
        "nv_seclog.cpp"
        "nv_sealed.cpp"
        "nv_time.cpp"
        "nv_crash.cpp"
        "nv_irqwatch.cpp"
        "nv_bgwork.cpp"
        "nv_clipboard.cpp"
        "nv_net_policy.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer nvs_flash esp_netif lwip espcoredump
    PRIV_REQUIRES spi_flash esp_app_format efuse mbedtls esp_partition esp_rom
)

# Every esp_restart() goes through nv_crash.cpp's wrapper, which notes who asked (RTC memory) so the
# next boot can say why it was a software reset (nv_crash_last_restart, GET /api/crash "restart").
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_restart")
