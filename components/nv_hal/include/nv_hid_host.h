// nv_hid_host — USB HID host: keyboard, mouse and gamepads on the OTG-HS Type-C (directly or
// behind a hub). Keyboard keys inject into the focused IME field exactly like the on-screen
// keyboard (nv_ime_inject_*), delivered on the LVGL thread; the mouse drives an LVGL pointer indev with an on-screen cursor, so
// it clicks/scrolls the whole UI; gamepads become nv_pad players. Requires host mode ("usbhost"
// config) — same bus as nv_usb_audio, which owns usb_host_install; call this AFTER
// nv_usb_audio_init().
//
// Keyboard and mouse use the boot protocol (every real one supports it), US or Italian keymap.
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
// invoked on the LVGL thread (from an lv_timer). `key` uses the nv_ime_remote_key_t values
// (ENTER=0, ESC, BACKSPACE, DELETE, TAB, LEFT, RIGHT, UP, DOWN).
typedef void (*nv_hid_host_text_cb)(const char *utf8);
typedef void (*nv_hid_host_key_cb)(int key);
void nv_hid_host_set_sink(nv_hid_host_text_cb text, nv_hid_host_key_cb key);

// Physical keyboard layout (letters are QWERTY in both; symbols, accents and AltGr differ).
typedef enum { NV_HID_KBD_US = 0, NV_HID_KBD_IT = 1 } nv_hid_kbd_layout_t;
void nv_hid_host_set_layout(nv_hid_kbd_layout_t layout);
nv_hid_kbd_layout_t nv_hid_host_get_layout(void);

// Every key press, auto-repeat and release, on the LVGL thread, BEFORE the IME: `usage` is the
// HID usage, `mods` the boot-report modifier byte (0x01/0x10 Ctrl, 0x02/0x20 Shift, 0x04 Alt,
// 0x40 AltGr, 0x08/0x80 Win). Modifiers also arrive as their own keys (usages 0xE0..0xE7), so
// Win alone or letting go of Alt can be acted on. Return true to consume it (shortcuts, focus navigation). Keys
// are queued by the report path and delivered by an LVGL timer, so a busy UI never drops them;
// held keys auto-repeat (500 ms, then every 33 ms). Alt / Win chords never type text.
typedef bool (*nv_hid_host_kbd_hook_cb)(uint8_t usage, uint8_t mods, bool pressed, bool repeat);
void nv_hid_host_set_kbd_hook(nv_hid_host_kbd_hook_cb hook);
// Synthetic key (tests / remote control): press + release of `usage` with `mods`, through the same
// queue, layout, hook and IME path as a physical keyboard. False when the queue is full.
bool nv_hid_host_inject_key(uint8_t usage, uint8_t mods);
// UTF-8 text a key types under the active layout with these modifiers (AltGr, Shift), or NULL.
const char *nv_hid_host_key_text(uint8_t usage, uint8_t mods);

// Mouse right button (USB / Bluetooth), on the LVGL thread with the pointer position: context
// menus. Not called while a fullscreen app has captured the mouse.
void nv_hid_host_set_rclick_cb(void (*cb)(int x, int y));
// Mouse wheel, on the LVGL thread: lines to scroll at the pointer (positive = wheel up / towards
// the top), already scaled and signed by the Mouse settings.
void nv_hid_host_set_wheel_cb(void (*cb)(int x, int y, int lines));
// The mouse's LVGL pointer indev (lv_indev_t *), NULL before the first mouse: tells a mouse click
// from a finger (double-click to open on the desktop, single tap on touch).
void *nv_hid_host_mouse_indev(void);

// Mouse preferences (Settings > Mouse), applied live: pointer speed in percent (100 = 1:1), wheel
// lines per detent, inverted wheel, left-handed (left / right buttons swapped). Games reading the
// raw mouse (nv_hid_host_mouse_take) get the buttons swapped too, nothing else.
void nv_hid_host_set_mouse_prefs(int speed_pct, int wheel_lines, bool invert_wheel, bool left_handed);

// Presence (USB or Bluetooth). Every change also publishes NV_EV_INPUT_DEVICES (nv_event_bus),
// synchronously from the HID / Bluetooth task: subscribers that touch LVGL must lv_async_call.
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
