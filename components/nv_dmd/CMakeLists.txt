idf_component_register(
    SRCS
        "nv_dmd.c"
        "dmd_core.c"
    INCLUDE_DIRS "include" "."
    REQUIRES nv_kernel
    PRIV_REQUIRES lwip esp_timer log esp_rom esp_driver_usb_serial_jtag
)
