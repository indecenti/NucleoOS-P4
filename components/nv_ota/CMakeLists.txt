idf_component_register(
    SRCS
        "nv_ota.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        nv_fwup      # signed manifests, verified staging, journal + safety copy (shared with recovery/)
        nv_kernel
        nv_hal
        nv_i18n      # boot notices (update result, safe mode)
        esp_http_client
        app_update
        esp_partition
        esp_hw_support
        fatfs
        json
        esp-tls
        mbedtls
        lwip
)
# The recovery app built alongside (top-level CMakeLists.txt, ExternalProject recovery_app, which this
# library depends on): installed into the `recovery` slot by a confirmed system (nv_ota sync_recovery).
set(_rec_bin "${CMAKE_BINARY_DIR}/recovery/nucleo-recovery.bin")
set_source_files_properties("${_rec_bin}" PROPERTIES GENERATED TRUE)   # made by recovery_app at build time
target_add_binary_data(${COMPONENT_LIB} "${_rec_bin}" BINARY)

# Test builds only: an image that fails on purpose (boot | late | ui | net), see nv_ota.cpp inject_fault.
if(NV_OTA_FAULT)
    message(WARNING "nv_ota: TEST BUILD with injected fault '${NV_OTA_FAULT}'")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "NV_OTA_FAULT=\"${NV_OTA_FAULT}\"")
endif()
