// Harness shim of components/nv_hal/include/nv_hid_host.h (the parts the WASM-4 host uses).
// w4run.cpp implements them to simulate a USB keyboard / mouse / gamepads (--keyboard, --mouse,
// --pads).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
bool nv_hid_host_keyboard_present(void);
bool nv_hid_host_mouse_present(void);
int  nv_hid_host_keys_down(uint8_t usages[6]);
bool nv_hid_host_mouse_state(int *x, int *y, uint8_t *buttons);
#define NV_HID_MAX_PADS 4
enum { NV_HID_DIR_UP = 1, NV_HID_DIR_DOWN = 2, NV_HID_DIR_LEFT = 4, NV_HID_DIR_RIGHT = 8 };
int  nv_hid_host_gamepad_count(void);
bool nv_hid_host_gamepad_state(int index, uint8_t *dirs, uint32_t *buttons);
#ifdef __cplusplus
}
#endif
