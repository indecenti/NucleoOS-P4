// nv_bt — Bluetooth LE for game controllers (HID over GATT host).
//
// The ESP32-P4 has no radio: the on-board ESP32-C6 runs the BLE controller and esp_hosted carries
// HCI over the same SDIO link as Wi-Fi (C6 firmware reports "HCI over SDIO, BLE only"). The
// NimBLE host runs here on the P4. BLE only: Bluetooth Classic pads (DualShock 4, DualSense,
// Switch Pro, Joy-Con) can't connect wirelessly — they work over USB.
//
// Pads found by a scan are paired (bonded, keys in NVS) and then reconnect by themselves when
// switched on while Bluetooth is enabled. Each connected pad is an nv_pad slot (NV_PAD_SRC_BLE),
// its HID Report Map parsed by nv_hid_gamepad and mapped with nv_pad_map_*.
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
    NV_BT_SCANNING,       // discovery for new pads (nv_bt_scan_start)
    NV_BT_CONNECTING,     // connecting / pairing / reading the HID service
    NV_BT_ERROR,          // controller didn't answer (see nv_bt_status.error)
} nv_bt_state_t;

typedef struct {
    nv_bt_state_t state;
    uint8_t  n_connected;          // BLE pads connected now
    uint8_t  n_paired;
    char     error[64];            // last error, "" = none
    char     busy_name[32];        // device being connected / paired
} nv_bt_status_t;

typedef struct {
    uint8_t  addr[6];              // little-endian as NimBLE stores it
    uint8_t  addr_type;            // BLE_ADDR_* (0 public, 1 random)
    int8_t   rssi;
    uint16_t appearance;           // 0x03C4 gamepad, 0x03C3 joystick, 0x03C1 keyboard, 0x03C2 mouse, 0 = unknown
    bool     hid;                  // advertises the HID service (0x1812)
    bool     paired;
    char     name[32];
} nv_bt_device_t;

// Start once at boot: brings Bluetooth up if the user left it on (nv_config "bt_on"). Idempotent.
void nv_bt_init(void);

// Turn Bluetooth on / off (persisted). Off disconnects every pad and stops the host.
void nv_bt_set_enabled(bool on);
bool nv_bt_is_enabled(void);
void nv_bt_status(nv_bt_status_t *out);

// Discovery for new pads (HID devices), up to `seconds` (<= 60). Results accumulate, strongest
// first; call nv_bt_scan_results while scanning.
bool nv_bt_scan_start(int seconds);
void nv_bt_scan_stop(void);
int  nv_bt_scan_results(nv_bt_device_t *out, int max);

// Connect + pair a scanned device (async: watch nv_bt_status / nv_pad_count).
bool nv_bt_connect(const uint8_t addr[6], uint8_t addr_type);
// Paired devices (bond store). `connected` tells which are online.
int  nv_bt_paired(nv_bt_device_t *out, bool *connected, int max);
// Disconnect + delete the bond.
bool nv_bt_forget(const uint8_t addr[6], uint8_t addr_type);

// "aa:bb:cc:dd:ee:ff" (MSB first, as phones show it) <-> NimBLE little-endian bytes.
void nv_bt_addr_str(const uint8_t addr[6], char out[18]);
bool nv_bt_addr_parse(const char *s, uint8_t addr[6]);

#ifdef __cplusplus
}
#endif
