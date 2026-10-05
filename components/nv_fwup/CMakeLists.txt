# nv_fwup: firmware update core (signed manifests, verified install/backup, the `assets` journal and
# last-known-good copy, the update policy) shared by NucleoOS and the recovery app. ESP-IDF only - no
# NucleoOS components - so recovery/ stays small.
#
# Release keys (tools/ota_sign.py): ota_signing_pub.pem signs releases, ota_signing_pub_backup.pem is
# the offline backup that makes a key change possible. A TEST build passes -DNV_FWUP_PUBKEY=<pem>:
# that one key replaces both, so a test image is never trusted by a production device and a
# production image never by the test board. tools/ota_sign.py refuses to sign an image that does
# not embed the production key.
if(NV_FWUP_PUBKEY)
    message(WARNING "nv_fwup: TEST BUILD - trusts only ${NV_FWUP_PUBKEY}, not the release keys")
    set(_keydir "${CMAKE_CURRENT_BINARY_DIR}/testkey")
    configure_file("${NV_FWUP_PUBKEY}" "${_keydir}/ota_signing_pub.pem" COPYONLY)
    configure_file("${NV_FWUP_PUBKEY}" "${_keydir}/ota_signing_pub_backup.pem" COPYONLY)
    set(_keys "${_keydir}/ota_signing_pub.pem" "${_keydir}/ota_signing_pub_backup.pem")
else()
    set(_keys ota_signing_pub.pem ota_signing_pub_backup.pem)
endif()

idf_component_register(
    SRCS
        "nv_fwup.cpp"
        "nv_fwup_safety.cpp"
        "nv_fwup_policy.cpp"
        "nv_fwup_codec.cpp"
        "nv_ota_manifest.cpp"
    INCLUDE_DIRS "include"
    EMBED_TXTFILES ${_keys}
    REQUIRES
        app_update
        bootloader_support
        esp_partition
        json
        mbedtls
        vfs
)
if(NV_FWUP_PUBKEY)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC NV_FWUP_TEST_KEY=1)
endif()
