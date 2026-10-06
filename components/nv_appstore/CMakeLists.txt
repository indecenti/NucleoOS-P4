idf_component_register(
    SRCS "nv_appstore.cpp" "nv_store_pkg.cpp" "nv_store_tree.cpp"
    INCLUDE_DIRS "include"
    EMBED_TXTFILES store_signing_pub.pem          # store key's public half (tools/store_sign.py)
                   store_signing_backup_pub.pem   # the offline backup key (store_sign.py keygen-backup)
    REQUIRES nv_kernel nv_hal nv_wasm nv_i18n nv_telemetry esp_http_client esp-tls mbedtls json fatfs
)
