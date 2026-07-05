idf_component_register(
    SRCS "nv_camera.c" "nv_mp4.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        nv_hal
        nv_kernel
        esp_driver_cam
        esp_driver_isp
        esp_driver_ppa
        esp_driver_jpeg
        esp_driver_i2c
        esp_mm
        esp_hw_support
        esp_h264
)
