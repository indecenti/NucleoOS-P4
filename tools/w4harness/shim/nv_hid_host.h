// Harness shim of components/nv_hal/include/nv_hid_host.h (the parts the WASM-4 host uses).
// w4run.cpp implements them to simulate a USB keyboard / mouse (--keyboard, --mouse); controllers
// (--pads) come through the nv_pad.h shim.
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
#ifdef __cplusplus
}
#endif
