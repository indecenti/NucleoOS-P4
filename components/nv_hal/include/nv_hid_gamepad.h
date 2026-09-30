// nv_hid_gamepad — generic HID gamepad / joystick report parser, shared by the USB HID host and the
// Bluetooth LE HID host (a HOGP Report Map is the same descriptor format).
//
// Gamepads don't have a boot protocol: every model describes its own input report in its HID
// report descriptor. This parses the descriptor once (on connect) and records every axis, hat
// switch and button of the first Joystick / Gamepad / Multi-axis application collection, ordered
// the way SDL enumerates them (see nv_hid_raw_t in nv_pad.h), so the SDL_GameControllerDB
// mappings apply unchanged. Each input report then decodes into an nv_hid_raw_t. Plain C with no
// IDF dependency: tested on the PC (tools/hidpad_test).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nv_pad.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t off;          // bit offset in the report (after the report ID byte, if any)
    uint8_t  size;         // bits; 0 = field absent
    int32_t  lmin, lmax;   // logical range
} nv_hid_field_t;

#define NV_HID_MAX_BUTTONS 32

typedef struct {
    uint8_t        report_id;                     // 0 = the device doesn't use report IDs
    uint8_t        n_axes, n_hats, n_buttons;
    bool           sim_triggers;                  // Simulation Accelerator / Brake present (Xbox BLE)
    nv_hid_field_t axis[NV_HID_MAX_AXES];         // sorted by Linux ABS code
    uint8_t        axis_code[NV_HID_MAX_AXES];    // that code (diagnostics)
    nv_hid_field_t hat[NV_HID_MAX_HATS];
    nv_hid_field_t dpad[4];                       // Desktop D-pad usages (up, down, right, left) -> hat 0
    nv_hid_field_t btn[NV_HID_MAX_BUTTONS];       // by usage rank
} nv_hid_pad_layout_t;

// True when the descriptor has a Joystick / Gamepad / Multi-axis application collection with some
// way to steer (two axes, a hat or a D-pad) and at least one button.
bool nv_hid_pad_parse(const uint8_t *desc, size_t len, nv_hid_pad_layout_t *out);

// One input report (starting with the report ID byte when the layout has one) -> raw state.
// False when the report isn't the one the layout describes (another report ID, too short): keep
// the old state.
bool nv_hid_pad_decode(const nv_hid_pad_layout_t *l, const uint8_t *report, size_t len, nv_hid_raw_t *out);

// Standard mapping for a parsed pad (bus NV_PAD_BUS_USB / NV_PAD_BUS_BT): see nv_pad_map_lookup.
static inline bool nv_hid_pad_map(uint8_t bus, uint16_t vid, uint16_t pid, const nv_hid_pad_layout_t *l,
                                  nv_pad_map_t *out) {
    return nv_pad_map_lookup(bus, vid, pid, l->axis_code, l->n_axes, l->n_hats, l->n_buttons, out);
}

#ifdef __cplusplus
}
#endif
