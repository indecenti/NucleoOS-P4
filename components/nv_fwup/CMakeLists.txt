# nv_fwup: firmware update core (signed manifests, verified install/backup) shared by NucleoOS and
# the recovery app. ESP-IDF only - no NucleoOS components - so recovery/ stays small.
idf_component_register(
    SRCS
        "nv_fwup.cpp"
        "nv_ota_manifest.cpp"
    INCLUDE_DIRS "include"
    EMBED_TXTFILES ota_signing_pub.pem   # the release key's public half (tools/ota_sign.py)
    REQUIRES
        app_update
        esp_partition
        json
        mbedtls
        vfs
)
