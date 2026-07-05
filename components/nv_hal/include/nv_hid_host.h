// nv_hid_host — USB HID host: keyboard + mouse on the OTG-HS Type-C (directly or behind a hub).
// Keyboard keys inject into the focused IME field exactly like the on-screen keyboard
// (nv_ime_inject_*); the mouse drives an LVGL pointer indev with an on-screen cursor, so it
// clicks/scrolls the whole UI. Requires host mode ("usbhost" config) — same bus as nv_usb_audio,
// which owns usb_host_install; call this AFTER nv_usb_audio_init().
//
// Boot-protocol only (every real keyboard/mouse supports it). US keymap for now.
#pragma once

#include <stdbool.h>

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

#ifdef __cplusplus
}
#endif
