// nv_hid_host — USB HID host: keyboard, mouse and gamepads on the OTG-HS Type-C (directly or
// behind a hub). Keyboard keys inject into the focused IME field exactly like the on-screen
// keyboard (nv_ime_inject_*); the mouse drives an LVGL pointer indev with an on-screen cursor, so
// it clicks/scrolls the whole UI; gamepads are read by games only. Requires host mode ("usbhost"
// config) — same bus as nv_usb_audio, which owns usb_host_install; call this AFTER
// nv_usb_audio_init().
//
// Keyboard and mouse use the boot protocol (every real one supports it), US keymap for now.
// Gamepads / joysticks are generic HID: the report descriptor is parsed on connect
// (nv_hid_gamepad.c) for the left stick, the hat switch / D-pad and the buttons. XInput pads
// (Xbox) aren't HID and stay unsupported.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Install the HID class driver (waits briefly for the USB host stack). Idempotent.
bool nv_hid_host_init(void);

// Keyboard sink — nv_hal cannot depend on nv_ui (cycle), so the IME wiring is injected:
// app_main registers adapters that call nv_ime_inject_text / nv_ime_inject_key. Both are
// invoked under lvgl_port_lock already. `key` uses the nv_ime_remote_key_t values
// (ENTER=0, ESC, BACKSPACE, DELETE, TAB, LEFT, RIGHT, UP, DOWN).
typedef void (*nv_hid_host_text_cb)(const char *utf8);
typedef void (*nv_hid_host_key_cb)(int key);
void nv_hid_host_set_sink(nv_hid_host_text_cb text, nv_hid_host_key_cb key);

bool nv_hid_host_keyboard_present(void);
bool nv_hid_host_mouse_present(void);

// Raw state for full-screen games, which need held keys rather than the IME's key presses.
// Keyboard: HID usages currently held (boot report, up to 6) -> count, 0 without a keyboard.
int nv_hid_host_keys_down(uint8_t usages[6]);
// Mouse: pointer position (panel coords, same as the LVGL cursor) and HID button bits
// (1 = left, 2 = right, 4 = middle). False without a mouse.
bool nv_hid_host_mouse_state(int *x, int *y, uint8_t *buttons);

// Gamepads, up to NV_HID_MAX_PADS, numbered in connection order (a disconnect closes the gap).
// Directions: NV_HID_DIR_* bits (stick past ~40%, hat switch or D-pad); buttons: bit i = HID
// button i + 1. False when there is no such gamepad.
#define NV_HID_MAX_PADS 4
enum { NV_HID_DIR_UP = 1, NV_HID_DIR_DOWN = 2, NV_HID_DIR_LEFT = 4, NV_HID_DIR_RIGHT = 8 };
int  nv_hid_host_gamepad_count(void);
bool nv_hid_host_gamepad_state(int index, uint8_t *dirs, uint32_t *buttons);

#ifdef __cplusplus
}
#endif
