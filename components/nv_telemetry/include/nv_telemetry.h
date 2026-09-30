// nv_telemetry — opt-in, anonymous usage statistics (one report a day).
//
// Nothing is recorded or sent until the owner accepts (setup wizard, or Settings > Security). A
// report is one HTTPS GET to https://nucleoos.indexhub.it/t/1?<counts> at most once per calendar
// day, carrying no device identifier: "first report this month / ever" flags count active devices
// without telling two of them apart. The server logs no IP address (server/stats/README.md), the
// public notice is <store>/privacy.html. What a report holds: firmware version, UI language, store
// region, reset reasons and crashes since the last report, whether an update landed or was rolled
// back, launches and minutes of system and store apps (never side-loaded ones), store installs /
// updates / uninstalls, hardware flags (PSRAM MB, SD, gamepad, keyboard), lowest free SRAM.
//
// Counters live in RAM and are mirrored to /sdcard/nucleos/tele.bin (a reboot is itself an event
// worth counting); without an SD card they only cover the current run.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NV_TELEMETRY_UNASKED = 0,   // never asked: nothing is recorded (the setup wizard asks)
    NV_TELEMETRY_YES     = 1,
    NV_TELEMETRY_NO      = 2,
} nv_telemetry_consent_t;

// Boot, after nv_config + the SD card and BEFORE nv_ui_start() (it reads "last_ver", which the UI
// then updates): records this boot's reset reason and a landed / rolled-back update.
void nv_telemetry_init(void);
// After Wi-Fi and the clock are up: a 10-minute check that sends the day's report when due.
void nv_telemetry_start(void);

nv_telemetry_consent_t nv_telemetry_consent(void);
// The owner's answer. NO also forgets every counter collected so far.
void nv_telemetry_set_consent(bool yes);

// SystemUI: an app came to the foreground / left it. `countable` = a system app or one installed
// from the store (its id is public); anything else is never recorded.
void nv_telemetry_app_open(const char *id, bool countable);
void nv_telemetry_app_close(void);

typedef enum { NV_TL_STORE_INSTALL, NV_TL_STORE_UPDATE, NV_TL_STORE_UNINSTALL } nv_tl_store_t;
void nv_telemetry_store(nv_tl_store_t what);

// The public notice (privacy.html next to the store catalog), for a QR code.
#define NV_TELEMETRY_PRIVACY_URL "https://indecenti.github.io/nucleoos-p4-store/privacy.html"

#ifdef __cplusplus
}
#endif
