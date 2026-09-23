// nv_hid_gamepad — generic USB HID gamepad / joystick support for nv_hid_host (private to nv_hal).
//
// Gamepads don't have a boot protocol: every model describes its own input report in its HID
// report descriptor. This parses the descriptor once (on connect) for what a game needs — the
// left stick (X / Y), the hat switch or D-pad usages, and the buttons — and then decodes each
// input report into directions + button bits. Plain C with no IDF dependency, so it is tested on
// the PC (tools/hidpad_test). XInput pads (Xbox) aren't HID and aren't covered.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t off;          // bit offset in the report (after the report ID byte, if any)
    uint8_t  size;         // bits; 0 = field absent
    int32_t  lmin, lmax;   // logical range
} nv_hid_field_t;

#define NV_HID_PAD_BUTTONS 16

typedef struct {
    uint8_t        report_id;          // 0 = the device doesn't use report IDs
    nv_hid_field_t x, y;               // left stick
    nv_hid_field_t hat;                // hat switch (8- or 4-way)
    nv_hid_field_t dpad[4];            // Generic Desktop D-pad usages: up, down, right, left
    nv_hid_field_t btn[NV_HID_PAD_BUTTONS];   // button 1..16
    uint8_t        n_buttons;          // highest button number found (<= NV_HID_PAD_BUTTONS)
} nv_hid_pad_layout_t;

// Direction bits reported by nv_hid_pad_decode().
enum { NV_PAD_UP = 1, NV_PAD_DOWN = 2, NV_PAD_LEFT = 4, NV_PAD_RIGHT = 8 };

// True when the descriptor has a Joystick or Gamepad application collection with some way to
// steer (stick, hat or D-pad) and at least one button; `out` then describes its input report.
bool nv_hid_pad_parse(const uint8_t *desc, size_t len, nv_hid_pad_layout_t *out);

// One input report -> directions (NV_PAD_*) and buttons (bit i = button i + 1). False when the
// report isn't the one the layout describes (another report ID, too short): keep the old state.
bool nv_hid_pad_decode(const nv_hid_pad_layout_t *l, const uint8_t *report, size_t len,
                       uint8_t *dirs, uint32_t *buttons);

#ifdef __cplusplus
}
#endif
