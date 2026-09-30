// nv_bt — Bluetooth LE: device discovery + HID over GATT host (game controllers, keyboards, mice).
//
// The ESP32-P4 has no radio: the on-board ESP32-C6 runs the BLE controller and esp_hosted carries
// HCI over the same SDIO link as Wi-Fi (C6 firmware reports "HCI over SDIO, BLE only"). The
// NimBLE host runs here on the P4. BLE only: Bluetooth Classic pads (DualShock 4, DualSense,
// Switch Pro, Joy-Con) can't connect wirelessly — they work over USB.
//
// A scan lists every BLE advertiser nearby (phones, laptops, watches, tags... not only HID), but
// only HID devices can be connected. They are paired (bonded, keys in NVS) and then reconnect by
// themselves when switched on while Bluetooth is enabled. A connected pad is an nv_pad slot
// (NV_PAD_SRC_BLE), its HID Report Map parsed by nv_hid_gamepad and mapped with nv_pad_map_*;
// keyboards and mice are switched to the boot protocol and feed nv_hid_host (IME + pointer).
//
// All calls are thread-safe and non-blocking (work runs on the NimBLE host task); poll
// nv_bt_status / nv_bt_scan_results / nv_bt_paired from UI timers.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NV_BT_OFF = 0,        // disabled by the user (or never enabled)
    NV_BT_STARTING,       // bringing up the host / controller
    NV_BT_READY,          // idle (reconnecting to paired pads in the background)
    NV_BT_SCANNING,       // discovery (nv_bt_scan_start)
    NV_BT_CONNECTING,     // connecting / pairing / reading the HID service
    NV_BT_ERROR,          // controller didn't answer (see nv_bt_status.error)
} nv_bt_state_t;

typedef struct {
    nv_bt_state_t state;
    uint8_t  n_connected;          // BLE HID devices connected and set up now (pads, keyboards, mice)
    uint8_t  n_paired;
    char     error[64];            // last error, "" = none
    char     busy_name[32];        // device being connected / paired
} nv_bt_status_t;

typedef struct {
    uint8_t  addr[6];              // little-endian as NimBLE stores it
    uint8_t  addr_type;            // BLE_ADDR_* (0 public, 1 random)
    int8_t   rssi;
    uint16_t appearance;           // GAP appearance: 0x03C4 gamepad, 0x03C3 joystick, 0x03C1 keyboard, 0x03C2 mouse, 0 = unknown
    bool     hid;                  // advertises the HID service (0x1812)
    bool     paired;
    char     name[32];             // "" = the device advertises no name
    uint16_t company;              // manufacturer data company ID (0x004C Apple, 0x0006 Microsoft...), 0xFFFF = none
    uint8_t  mfg_type;             // first manufacturer data byte after the ID (Apple: message type), 0 = none
    bool     connectable;          // connectable advertising seen (ADV_IND / ADV_DIRECT_IND)
} nv_bt_device_t;

// What a device is, from its appearance (Bluetooth Assigned Numbers, category = appearance >> 6),
// its services and its manufacturer data. Values are stable (UI + /api/bt).
typedef enum {
    NV_BT_KIND_UNKNOWN = 0,
    NV_BT_KIND_GAMEPAD,            // gamepad / joystick
    NV_BT_KIND_KEYBOARD,
    NV_BT_KIND_MOUSE,              // mouse / touchpad / pen
    NV_BT_KIND_HID,                // other HID (remote, barcode reader, unknown HID)
    NV_BT_KIND_PHONE,
    NV_BT_KIND_COMPUTER,           // computer / tablet
    NV_BT_KIND_WATCH,              // watch / wearable / fitness band
    NV_BT_KIND_AUDIO,              // earbuds, headphones, speakers, hearing aids
    NV_BT_KIND_TV,                 // TV / display / media player
    NV_BT_KIND_TAG,                // tracker tag / beacon / keyring
    NV_BT_KIND_SENSOR,             // health / fitness / environment sensors, lights, appliances
    NV_BT_KIND_COUNT
} nv_bt_kind_t;

nv_bt_kind_t nv_bt_kind(const nv_bt_device_t *d);
// "gamepad", "keyboard"... ("unknown"), for JSON / logs.
const char  *nv_bt_kind_id(nv_bt_kind_t k);
// A device this host can connect to (HID, or a connectable device of unknown kind: some pads
// advertise neither the HID service nor an appearance).
bool         nv_bt_can_connect(const nv_bt_device_t *d);
// Well-known manufacturer names ("Apple", "Microsoft"...), NULL when not in the short table.
const char  *nv_bt_company_name(uint16_t company);

// Start once at boot: brings Bluetooth up if the user left it on (nv_config "bt_on"). Idempotent.
void nv_bt_init(void);

// Turn Bluetooth on / off (persisted). Off disconnects every pad and stops the host.
void nv_bt_set_enabled(bool on);
bool nv_bt_is_enabled(void);
void nv_bt_status(nv_bt_status_t *out);

// Discovery of every nearby BLE device, up to `seconds` (<= 60). Results accumulate (at most 48);
// call nv_bt_scan_results while scanning. Order: HID devices (pads, keyboards, mice) first, then
// named devices, then by signal strength.
bool nv_bt_scan_start(int seconds);
void nv_bt_scan_stop(void);
int  nv_bt_scan_results(nv_bt_device_t *out, int max);

// This board's own BLE address (valid while the host runs). False when Bluetooth is off.
bool nv_bt_own_addr(uint8_t addr[6]);

// Connect + pair a scanned device (async: watch nv_bt_status / nv_pad_count).
bool nv_bt_connect(const uint8_t addr[6], uint8_t addr_type);
// Paired devices (bond store). `connected` tells which are online. `appearance` is set for a
// device connected now (what it turned out to be) or seen by the last scan.
int  nv_bt_paired(nv_bt_device_t *out, bool *connected, int max);
// Disconnect + delete the bond.
bool nv_bt_forget(const uint8_t addr[6], uint8_t addr_type);

// "aa:bb:cc:dd:ee:ff" (MSB first, as phones show it) <-> NimBLE little-endian bytes.
void nv_bt_addr_str(const uint8_t addr[6], char out[18]);
bool nv_bt_addr_parse(const char *s, uint8_t addr[6]);

#ifdef __cplusplus
}
#endif
