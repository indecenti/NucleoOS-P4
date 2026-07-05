idf_component_register(
    SRCS "nv_sysmon.c"
    INCLUDE_DIRS "include"
    REQUIRES nv_kernel nv_hal freertos esp_timer esp_hw_support heap
)
