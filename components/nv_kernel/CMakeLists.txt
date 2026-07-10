idf_component_register(
    SRCS
        "nv_log.cpp"
        "nv_event_bus.cpp"
        "nv_service_mgr.cpp"
        "nv_memory_broker.cpp"
        "nv_config.cpp"
        "nv_time.cpp"
        "nv_crash.cpp"
        "nv_bgwork.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer nvs_flash esp_netif lwip espcoredump
)
