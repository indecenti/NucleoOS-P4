// nv_hid_host — USB HID host: keyboard, mouse and gamepads on the OTG-HS Type-C (directly or
// behind a hub). Keyboard keys inject into the focused IME field exactly like the on-screen
// keyboard (nv_ime_inject_*); the mouse drives an LVGL pointer indev with an on-screen cursor, so
// it clicks/scrolls the whole UI; gamepads become nv_pad players. Requires host mode ("usbhost"
// config) — same bus as nv_usb_audio, which owns usb_host_install; call this AFTER
// nv_usb_audio_init().
//
// Keyboard and mouse use the boot protocol (every real one supports it), US keymap for now.
// Gamepads / joysticks are generic HID: the report descriptor is parsed on connect
// (nv_hid_gamepad.c) and mapped to the standard layout with the SDL_GameControllerDB mappings
// (nv_pad.c); Switch pads get their USB handshake, DualShock 4 / DualSense rumble + light bar.
// XInput pads (Xbox) aren't HID: nv_xinput.cpp handles them.
#pragma once

#include <stdbool.h>
#include <stddef.h>
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
// Keyboard for games that need real keys: out[0] = modifier byte (boot report bits), out[1..] =
// the usages held now. Returns how many usages (0..6), -1 without a keyboard.
int nv_hid_host_kbd_state(uint8_t out[7]);
// Mouse motion since the previous call (raw counts, unclamped) + buttons held now. False without
// a mouse (the counters are still reset).
bool nv_hid_host_mouse_take(int32_t *dx, int32_t *dy, int32_t *wheel, uint8_t *buttons);
// A full-screen app owns the mouse: pointer hidden and frozen, no UI clicks. Off again on exit.
void nv_hid_host_mouse_capture(bool on);

// Keyboards / mice on another transport (Bluetooth LE HID, boot protocol): announce them, then feed
// boot reports (keyboard 8 bytes: modifiers, reserved, 6 usages; mouse: buttons, dx, dy[, wheel]).
// They type into the IME and drive the pointer exactly like USB ones. Safe from any task.
void nv_hid_host_ext_keyboard(bool connected);
void nv_hid_host_ext_mouse(bool connected);
void nv_hid_host_ext_keyboard_report(const uint8_t *r, size_t len);
void nv_hid_host_ext_mouse_report(const uint8_t *r, size_t len);

// Gamepads are published through nv_pad (nv_pad.h), together with XInput and Bluetooth pads.

#ifdef __cplusplus
}
#endif
