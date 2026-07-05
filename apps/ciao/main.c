// ciao — NucleoOS Anima demo app written in plain C against the SDK (host ABI v1).
// Build:  .\sdk\build_app.ps1 -AppDir apps\ciao
// Deploy: copy manifest.json + app.wasm to /sdcard/apps/ciao/
#include "nucleo_sdk.h"

NV_EXPORT("run")
void run(void) {
    nv_print("Ciao! App C compilata con clang + NucleoOS SDK.");

    char lang[8];
    nv_lang(lang, sizeof lang);
    nv_printf("lingua UI: %s", lang);
    nv_printf("uptime: %d ms, epoch: %d s", nv_millis(), (int32_t)nv_time_unix());
    nv_printf("numero fortunato: %u", (uint32_t)nv_rand() % 100u);

    nv_log(NV_LOG_INFO, "demo SDK: giro completato");
    nv_toast(NV_TOAST_OK, "SDK C funziona!");
}
