idf_component_register(
    SRCS "recovery.cpp" "rec_disp.cpp" "rec_assets.c"
    INCLUDE_DIRS "."
    REQUIRES
        nv_fwup
        app_update
        bootloader_support
        esp_driver_ledc
        esp_driver_sdmmc
        esp_lcd
        esp_mm
        esp_partition
        fatfs
        sdmmc
)
