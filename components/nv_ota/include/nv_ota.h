// nv_ota — Wi-Fi firmware updater (dual-OTA) for NucleoOS Anima.
//
// Flow: nv_ota_check(manifest_url) fetches a small JSON manifest describing the latest build;
// if its version differs from the running firmware the state becomes AVAILABLE (with the .bin
// URL remembered). nv_ota_update() then streams that image into the inactive OTA slot with live
// progress; on success the state is SUCCESS and nv_ota_reboot() boots the new slot. All network
// work runs on a worker task — never blocks the UI thread. Poll nv_ota_generation() for changes.
//
// Manifest JSON:  {"version":"0.3.0","url":"https://host/nucleos-anima.bin","notes":"..."}
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NV_OTA_IDLE = 0,     // nothing in progress
    NV_OTA_CHECKING,     // fetching the manifest
    NV_OTA_UPTODATE,     // manifest fetched; already on the latest version
    NV_OTA_AVAILABLE,    // a newer version exists (see nv_ota_available_version())
    NV_OTA_DOWNLOADING,  // streaming the image (see nv_ota_progress())
    NV_OTA_SUCCESS,      // image written + validated; reboot to apply
    NV_OTA_FAILED,       // check/download failed (see nv_ota_message())
} nv_ota_state_t;

void nv_ota_init(void);   // mark the running image valid (cancels rollback); call once at boot

nv_ota_state_t nv_ota_state(void);
int  nv_ota_progress(void);                 // 0..100 during DOWNLOADING
uint32_t nv_ota_generation(void);           // bumps on any state/progress change (UI poll)
const char *nv_ota_running_version(void);   // running firmware version (app descriptor)
const char *nv_ota_available_version(void); // version offered by the last successful check ("" if none)
const char *nv_ota_message(void);           // human status / error / "notes" line

// Kick off (async). No-op while a check/download is already running.
void nv_ota_check(const char *manifest_url);  // NULL/"" -> fail (UI supplies the URL)
void nv_ota_update(void);                      // download (staged on SD when present) + apply
// Flash a firmware image already on the SD card (offline update; no server). NULL/"" ->
// "/sdcard/nucleos-anima.bin".
void nv_ota_install_sd(const char *path);
void nv_ota_reboot(void);                      // restart into the freshly written slot

// Hands-free boot updater: on a background task, wait for the network, fetch `manifest_url`, and
// if it offers a different version download + flash + reboot into it automatically. Verbose serial
// logging. No-op on NULL/"" URL. Call once at boot after the Wi-Fi service is initialized.
void nv_ota_boot_autoupdate(const char *manifest_url);

#ifdef __cplusplus
}
#endif
