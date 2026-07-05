idf_component_register(
    SRCS "nv_usb.c" "nv_usb_vendor.c" "nv_usb_frame.c" "nv_usb_hid.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "tusb" "."
    PRIV_REQUIRES usb esp_driver_jpeg esp_timer nv_kernel
)

# TinyUSB compiles against our tusb_config.h, and the descriptor callbacks live inside
# the tinyusb archive so its usbd.c resolves them (same pattern as the esp-iot-solution
# usb_extend_screen example).
idf_build_get_property(build_components BUILD_COMPONENTS)
if(tinyusb IN_LIST build_components)
    set(tinyusb_name tinyusb) # local component
else()
    set(tinyusb_name espressif__tinyusb) # managed component
endif()

idf_component_get_property(tusb_lib ${tinyusb_name} COMPONENT_LIB)
target_include_directories(${tusb_lib} PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/tusb"
    "${CMAKE_CURRENT_LIST_DIR}/include")
target_sources(${tusb_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/tusb/usb_descriptors.c")

cmake_policy(SET CMP0079 NEW)
target_link_libraries(${tusb_lib} PRIVATE ${COMPONENT_LIB})
