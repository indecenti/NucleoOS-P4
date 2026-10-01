// nv_ota — firmware updater for NucleoOS Anima, flash layout v2 (recovery + system, docs/OTA.md).
//
// Flow: nv_ota_check(manifest_url) fetches a small JSON manifest describing the latest build; if it
// is newer and signed, the state becomes AVAILABLE. nv_ota_update() downloads the image to the SD
// card (nvupd/), verifies it, saves a rollback copy of the running system, and arms the recovery app;
// the state becomes SUCCESS and nv_ota_reboot() restarts into recovery, which installs and verifies
// the image and boots it. Updates therefore need a microSD card; without one the state is FAILED with
// a clear reason. All network work runs on worker tasks. Poll nv_ota_generation() for changes.
//
// Manifest JSON:  {"version":"1.2.0","url":"https://host/nucleos-anima.bin","notes":"...",
//                  "size":3786976,"sha256":"<64 hex>","sig":"<DER hex>"}
// A newer version is announced and installed only when "sig" is a valid ECDSA P-256 signature by the
// release key (public half: ota_signing_pub.pem, embedded) over nv_ota_manifest::message(), and the
// bytes written to the slot hash to "sha256"/"size" (else the slot is dropped before the boot
// pointer moves; recovery checks it all again). Release side: tools/ota_sign.py + tools/dist.py. nv_ota_install_sd()
// holds an image on the card to the same rule: the signed manifest must sit beside it as <name>.json.
#pragma once
#include <stdbool.h>
#include <stddef.h>
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

// Where updates come from unless Settings says otherwise: the GitHub Pages distribution repo
// (indecenti/nucleoos-p4-store, published by tools/dist.py). A local server for tests is still
// just a URL typed in Settings → Update, e.g. http://<PC-IP>:8080/manifest.json.
#define NV_OTA_DEFAULT_URL "https://indecenti.github.io/nucleoos-p4-store/ota/v2/manifest.json"
// Layout-v1 (dual 4.5 MB slots) channel: its images must never reach a v2 board and vice versa.
#define NV_OTA_LEGACY_URL  "https://indecenti.github.io/nucleoos-p4-store/ota/manifest.json"

// Arm the 60 s survival gate for a fresh image and read what recovery reported. Call once at boot,
// after the SD card is mounted.
void nv_ota_init(void);

// False on a layout-v1 board (dual slots): it cannot install v2 images - one reinstall from the web
// flasher (or USB) moves it to layout v2. The UI should say so.
bool nv_ota_layout_ok(void);

// A check/download/install worker is running (new requests are ignored until it ends).
bool nv_ota_busy(void);

// A one-line, user-facing summary of what recovery did before this boot ("NucleoOS updated to X",
// a failed install, a rollback). True once, then false.
bool nv_ota_take_boot_notice(char *out, size_t n);

// The manifest URL in use: nv_config "ota_url", or NV_OTA_DEFAULT_URL when unset or empty.
void nv_ota_get_url(char *out, size_t n);

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
// "/sdcard/nucleos-anima.bin". Needs the release's signed manifest beside it ("nucleos-anima.json")
// and a version newer than the running one; otherwise refused like a bad download.
void nv_ota_install_sd(const char *path);
void nv_ota_reboot(void);                      // restart into the freshly written slot

// Hands-free boot updater: on a background task, wait for the network, fetch `manifest_url`, and
// if it offers a different version download + flash + reboot into it automatically. Verbose serial
// logging. No-op on NULL/"" URL. Call once at boot after the Wi-Fi service is initialized.
void nv_ota_boot_autoupdate(const char *manifest_url);

// While running: re-read the manifest (nv_ota_get_url) 15 min after boot, then every 6 h. A newer
// version only flips the state to AVAILABLE, the same as a manual check, so Settings → Update
// offers it and the SystemUI posts a notice. Nothing is installed behind the user's back.
// Quiet on failure (no Wi-Fi is not news). Skips a tick while a check/download is running.
void nv_ota_watch_start(void);

#ifdef __cplusplus
}
#endif
