// nv_backup — settings durability: mirror the whole NVS (config + Wi-Fi credentials) to the SD
// card so preferences survive an NVS wipe, a low-pages auto-erase, a factory reflash, or moving
// the card to another unit. Firmware OTA already leaves NVS untouched; this adds a second, SD
// copy as belt-and-suspenders.
//
// Behavior wired by nv_backup_init():
//   * restore — if NVS looks empty at boot AND an SD backup exists, import it before the UI reads
//     any preference (so theme/language/brightness come back automatically);
//   * auto-backup — every NV_EV_SETTINGS_CHANGED re-exports the NVS to SD, debounced.
// Backup file: /sdcard/nucleos/settings.nvb (compact binary of every NVS entry).
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Restore-if-empty + arm auto-backup. Call after nv_config_init() and nv_sd_mount(), BEFORE the
// i18n/theme init that read config.
void nv_backup_init(void);

bool nv_backup_export(void);     // dump NVS -> SD now; true on success
bool nv_backup_import(void);     // SD -> NVS now (caller usually reboots to apply); true on success
bool nv_backup_available(void);  // an SD backup file exists
bool nv_backup_delete(void);     // remove the SD backup (factory reset: stop the auto-restore)

#ifdef __cplusplus
}
#endif
