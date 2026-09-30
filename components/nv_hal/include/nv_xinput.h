// nv_xinput — USB host driver for Xbox-protocol controllers (vendor class, not HID).
//
// Handles, by interface descriptor (never by VID list):
//   - Xbox 360 wired and X-input clones (8BitDo etc.)   class FF / sub 5D / proto 01
//   - Xbox 360 Wireless Receiver, up to 4 pads            class FF / sub 5D / proto 81
//   - Xbox One / Series wired (GIP protocol)              class FF / sub 47 / proto D0
//   - original Xbox (via adapter)                         class 58 / sub 42
// Every connected controller becomes a standard nv_pad slot (NV_PAD_SRC_XINPUT) with rumble.
// Interfaces of other classes are never claimed, so it coexists with nv_hid_host, nv_usb_audio
// and nv_usb_storage on the same bus.
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Start the driver (idempotent). Call after the USB host library is being installed (same place
// as nv_hid_host_init / nv_usb_storage_init): the client task retries registration until it is up.
bool nv_xinput_init(void);

// Controllers currently driven by this module (diagnostics).
int nv_xinput_count(void);

#ifdef __cplusplus
}
#endif
