idf_component_register(
    SRCS
        "nv_ota.cpp"
        "nv_ota_manifest.cpp"
    INCLUDE_DIRS "include"
    EMBED_TXTFILES ota_signing_pub.pem   # the release key's public half (tools/ota_sign.py)
    REQUIRES
        nv_kernel
        nv_hal
        esp_http_client
        app_update
        esp_partition
        json
        esp-tls
        mbedtls
)
