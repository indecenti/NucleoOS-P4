idf_component_register(
    SRCS "nucleo_anima.c" "anima_solve.c" "anima_text.c" "nucleo_anima_bench.c" "nucleo_anima_l1.c"
         "nucleo_anima_online.c" "nucleo_anima_learn.c" "nucleo_anima_profile.c"
         "nucleo_anima_translate.c" "nucleo_anima_hdc.c" "nucleo_anima_facet.c"
         "nucleo_arb.c" "nucleo_arb_plat_esp.c" "nucleo_setup_shim.c" "nucleo_anima_lan.c"
         "nucleo_anima_conv.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES esp_timer
    # online tier: HTTPS fetch + JSON parse + the live Wi-Fi IP (nv_hal/nv_wifi via shim).
    # mdns: LAN teacher discovery (nucleomind on the phone, _anima._tcp).
    PRIV_REQUIRES esp_http_client mbedtls json esp_partition nv_hal mdns
)

# Bounded snprintf into fixed buffers on purpose (entity/slug/url/reply); truncation is safe.
# misleading-indentation: the engine packs two statements per line in many spots by style.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation -Wno-error=misleading-indentation)
