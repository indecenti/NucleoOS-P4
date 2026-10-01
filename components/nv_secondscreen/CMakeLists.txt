idf_component_register(
    SRCS "ss_main.cpp" "ss_engine.cpp" "ss_usb.cpp" "ss_net.cpp" "ss_tls.cpp" "ss_cast.cpp"
         "ss_vnc.cpp" "ss_des.c" "ss_icons.c" "ss_assets.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES nv_kernel
    PRIV_REQUIRES nv_hal nv_usb lvgl esp_lvgl_port esp_lcd esp_driver_jpeg esp_driver_ppa esp_mm
                  esp_timer lwip mbedtls mdns esp_netif json esp_app_format esp_rom
)

# NucleoCast page + PC script: raw-deflated at build time, inflated once at run time (ss_assets.cpp).
foreach(asset "web/cast.html" "../../tools/nucleocast/nucleocast.py")
    get_filename_component(name "${asset}" NAME)
    set(out "${CMAKE_CURRENT_BINARY_DIR}/${name}.z")
    add_custom_command(OUTPUT "${out}"
        COMMAND ${PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/pack_asset.py" "${CMAKE_CURRENT_SOURCE_DIR}/${asset}" "${out}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${asset}" "${CMAKE_CURRENT_SOURCE_DIR}/pack_asset.py" VERBATIM)
    add_custom_target(ss_pack_${name} DEPENDS "${out}")
    add_dependencies(${COMPONENT_LIB} ss_pack_${name})
    target_add_binary_data(${COMPONENT_LIB} "${out}" BINARY)
endforeach()

# Labels/URLs/JSON are built with bounded snprintf on purpose (truncation = the intended clamp).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
