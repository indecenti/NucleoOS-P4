# nv_wake: the always-on wake word for ANIMA (see include/nv_wake.h). The ESP-SR backend is opt-in
# (CONFIG_NV_WAKE_ESP_SR, Kconfig) so the default build needs no extra component.
set(srcs "nv_wake.c" "wake_core.c")
set(reqs nv_hal nv_kernel esp_timer)
if(CONFIG_NV_WAKE_ESP_SR)
    list(APPEND srcs "wake_esp_sr.c")
    list(APPEND reqs espressif__esp-sr)
endif()
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    PRIV_REQUIRES ${reqs}
)
