// nv_wifi — Wi-Fi service for NucleoOS Anima.
//
// The ESP32-P4 has NO native radio: Wi-Fi runs on the on-board ESP32-C6 over esp-hosted
// (SDIO) via esp_wifi_remote. This module presents ONE backend-agnostic API to the UI and
// picks its implementation at compile time:
//   * REAL  — when <esp_wifi_remote.h> is available (component added + C6 slave flashed),
//             it drives esp_wifi_* for genuine scan/connect.
//   * SIM   — otherwise, a self-contained simulated provider so the whole Network UI is
//             fully usable (scan results, connect flow, saved creds, status) with no radio.
// nv_wifi_has_radio() tells the UI which one is live.
//
// Threading: the radio/timer callbacks mutate internal state only (never LVGL). The UI polls
// nv_wifi_get_state() + nv_wifi_scan_generation() from an LVGL timer and copies the AP list
// under lock via nv_wifi_copy_aps() — no cross-thread LVGL access.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NV_WIFI_DISABLED = 0,  // radio off
    NV_WIFI_IDLE,          // on, not connected
    NV_WIFI_SCANNING,      // scan in progress
    NV_WIFI_CONNECTING,    // association/DHCP in progress
    NV_WIFI_CONNECTED,     // associated + IP
    NV_WIFI_FAILED,        // last connect attempt failed (wrong password / no AP)
} nv_wifi_state_t;

// Security of an AP (mapped from the driver's authmode). Ordinal roughly tracks strength.
typedef enum {
    NV_WIFI_AUTH_OPEN = 0,
    NV_WIFI_AUTH_WEP,
    NV_WIFI_AUTH_WPA,
    NV_WIFI_AUTH_WPA2,
    NV_WIFI_AUTH_WPA3,
    NV_WIFI_AUTH_WPA2_ENT,   // enterprise (802.1X) — UI treats as needing more than a PSK
} nv_wifi_auth_t;

// 802.11 generation of an AP / link. The on-board ESP32-C6 is a 2.4 GHz Wi-Fi 6 radio, so it
// sees LEGACY / WIFI4 (11n) / WIFI6 (11ax); WIFI5 (11ac, 5 GHz-only) appears only as metadata.
typedef enum {
    NV_WIFI_GEN_LEGACY = 0,  // 802.11 b/g — no badge
    NV_WIFI_GEN_WIFI4,       // 802.11n
    NV_WIFI_GEN_WIFI5,       // 802.11ac
    NV_WIFI_GEN_WIFI6,       // 802.11ax
} nv_wifi_gen_t;

typedef enum { NV_WIFI_BAND_24 = 0, NV_WIFI_BAND_5 } nv_wifi_band_t;

typedef struct {
    char    ssid[33];
    int8_t  rssi;     // dBm (e.g. -55)
    bool    secured;  // needs a password (== auth != NV_WIFI_AUTH_OPEN)
    bool    saved;    // we hold credentials for it
    uint8_t auth;     // nv_wifi_auth_t
    uint8_t gen;      // nv_wifi_gen_t (best-effort from the scan's PHY flags)
} nv_wifi_ap_t;

// Details of the active association (valid only while NV_WIFI_CONNECTED).
typedef struct {
    char    ssid[33];
    char    ip[16];
    char    gateway[16];   // "" if unknown (sim backend never fills these)
    char    netmask[16];
    char    dns[16];       // primary DNS
    char    mac[18];       // station MAC "AA:BB:CC:DD:EE:FF"
    int8_t  rssi;
    uint8_t auth;     // nv_wifi_auth_t
    uint8_t gen;      // nv_wifi_gen_t (negotiated PHY)
    uint8_t band;     // nv_wifi_band_t
    uint8_t channel;  // primary channel
} nv_wifi_link_t;

// Load saved credentials; radio stays OFF (lazy — brought up on first enable).
void nv_wifi_init(void);
// false => the simulated backend is active (no C6/esp-hosted available).
bool nv_wifi_has_radio(void);

// Power the radio on/off. Enabling kicks an initial scan.
void nv_wifi_set_enabled(bool on);
bool nv_wifi_is_enabled(void);
nv_wifi_state_t nv_wifi_get_state(void);

// Trigger an async scan. Results land asynchronously; watch nv_wifi_scan_generation().
void nv_wifi_start_scan(void);
// Increments whenever the AP list changes — cheap change-detection for the UI poll.
uint32_t nv_wifi_scan_generation(void);
// Snapshot the current AP list into `buf` (up to `max`). Returns the count copied.
int nv_wifi_copy_aps(nv_wifi_ap_t *buf, int max);

// Connect. `pass` may be NULL/"" for an open network or one with saved credentials.
void nv_wifi_connect(const char *ssid, const char *pass);
void nv_wifi_disconnect(void);
// If connected, fill the connected SSID / IP / RSSI (any out-param may be NULL). Returns
// true when connected.
bool nv_wifi_get_connected(char *ssid_out, size_t ssid_n,
                           char *ip_out, size_t ip_n, int8_t *rssi_out);
// Superset of the above: fills the full link (auth/gen/band/channel). Returns true when
// connected; `out` is left untouched otherwise.
bool nv_wifi_get_link(nv_wifi_link_t *out);

// Short human labels for the enums above. auth: "Open"/"WEP"/"WPA"/"WPA2"/"WPA3"/"WPA2-E".
// gen: "" for LEGACY, else "Wi-Fi 4"/"Wi-Fi 5"/"Wi-Fi 6". Static storage — never free.
const char *nv_wifi_auth_label(uint8_t auth);
const char *nv_wifi_gen_label(uint8_t gen);

bool nv_wifi_is_saved(const char *ssid);
void nv_wifi_forget(const char *ssid);

#ifdef __cplusplus
}
#endif
