// nv_hid_host — USB HID host (keyboard -> IME, mouse -> LVGL pointer, gamepads -> nv_pad).
// See nv_hid_host.h.
#include "nv_hid_host.h"
#include "nv_hid_gamepad.h"

#include "nv_event_bus.h"
#include "nv_log.h"
#include "nv_mem_attr.h"   // NV_PSRAM_BSS: gamepad layouts are cold data

#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"
#include "usb/hid_usage_mouse.h"

#include "esp_lvgl_port.h"   // lvgl_port_lock — IME injection + indev setup off the LVGL thread
#include "lvgl.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static const char *TAG = "usb_hid";

namespace {

volatile bool s_kb_present = false;
volatile bool s_mouse_present = false;
// Keyboards / mice from other transports (Bluetooth LE HID), counted per source.
volatile int s_ext_kb = 0, s_ext_mouse = 0;

// Keyboard sinks (wired by app_main -> nv_ime; see header). NULL until registered.
nv_hid_host_text_cb s_text_sink = nullptr;
nv_hid_host_key_cb  s_key_sink = nullptr;

// Mirrors nv_ime_remote_key_t (nv_ime.h) — kept numeric here to avoid the nv_ui dependency.
enum { RK_ENTER = 0, RK_ESC, RK_BACKSPACE, RK_DELETE, RK_TAB, RK_LEFT, RK_RIGHT, RK_UP, RK_DOWN,
       RK_HOME, RK_END };

// ---------------------------------------------------------------- mouse -> LVGL pointer

volatile int  s_mx = 512, s_my = 300;     // cursor position (panel coords)
volatile bool s_mleft = false;
volatile uint8_t s_mbuttons = 0;          // HID button bits (games read all three)
lv_indev_t   *s_indev = nullptr;
lv_obj_t     *s_cursor = nullptr;
// Relative motion for a full-screen app that captured the mouse (nv_hid_host_mouse_take). While
// captured the reports stop moving/clicking the LVGL pointer and the cursor dot is hidden.
volatile int32_t s_acc_dx = 0, s_acc_dy = 0, s_acc_wheel = 0;
volatile int32_t s_ui_wheel = 0;          // wheel detents for the UI (scrolls what's under the pointer)
volatile bool    s_rclick = false;        // right button went down (delivered by the input pump)
void (*s_rclick_cb)(int x, int y) = nullptr;
volatile bool    s_captured = false;

void mouse_read_cb(lv_indev_t *, lv_indev_data_t *data) {
    data->point.x = (int32_t)s_mx;
    data->point.y = (int32_t)s_my;
    data->state = s_mleft ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    // LVGL 9.5 scrolls the scrollable object under a pointer by enc_diff (or sends ROTARY to an
    // editable one). HID wheel up is positive; LVGL scrolls content down for positive diffs.
    data->enc_diff = (int16_t)-__atomic_exchange_n(&s_ui_wheel, 0, __ATOMIC_RELAXED);
}

// Arrow cursor, drawn once into PSRAM from this mask: '#' outline, '.' fill, ' ' transparent.
// The hot spot is the tip (0,0), which is where LVGL puts the object's top-left corner.
constexpr int kCurW = 12, kCurH = 19;
constexpr const char *kCursor[kCurH] = {
    "#           ", "##          ", "#.#         ", "#..#        ", "#...#       ", "#....#      ",
    "#.....#     ", "#......#    ", "#.......#   ", "#........#  ", "#.........# ", "#......#####",
    "#...#..#    ", "#..# #..#   ", "#.#  #..#   ", "##    #..#  ", "#     #..#  ", "       #..# ",
    "        ##  ",
};
NV_PSRAM_BSS uint32_t s_cur_px[kCurW * kCurH];
lv_image_dsc_t s_cur_dsc;

const lv_image_dsc_t *cursor_image(void) {
    if (!s_cur_dsc.data) {
        for (int y = 0; y < kCurH; y++)
            for (int x = 0; x < kCurW; x++) {
                const char c = kCursor[y][x];
                s_cur_px[y * kCurW + x] = c == '#' ? 0xFF000000u : c == '.' ? 0xFFFFFFFFu : 0;
            }
        s_cur_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        s_cur_dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
        s_cur_dsc.header.w = kCurW;
        s_cur_dsc.header.h = kCurH;
        s_cur_dsc.header.stride = kCurW * 4;
        s_cur_dsc.data_size = sizeof s_cur_px;
        s_cur_dsc.data = (const uint8_t *)s_cur_px;
    }
    return &s_cur_dsc;
}

// Create the pointer indev + cursor dot once, on the LVGL thread (caller holds the port lock).
void mouse_indev_setup_locked(void) {
    if (s_indev) return;
    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, mouse_read_cb);

    s_cursor = lv_image_create(lv_layer_sys());   // top layer: above every app/screen
    lv_image_set_src(s_cursor, cursor_image());   // black-outlined white arrow, readable on any theme
    lv_obj_clear_flag(s_cursor, LV_OBJ_FLAG_CLICKABLE);
    lv_indev_set_cursor(s_indev, s_cursor);       // LVGL keeps the dot glued to the pointer
    if (s_captured) lv_obj_add_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);
}

void mouse_report(const uint8_t *d, size_t len) {
    if (len < 3) return;
    // Boot report: [0]=buttons, [1]=dx, [2]=dy (int8).
    const int8_t dx = (int8_t)d[1], dy = (int8_t)d[2];
    __atomic_fetch_add(&s_acc_dx, dx, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_acc_dy, dy, __ATOMIC_RELAXED);
    if (len >= 4) __atomic_fetch_add(&s_acc_wheel, (int8_t)d[3], __ATOMIC_RELAXED);
    s_mbuttons = (uint8_t)(d[0] & 0x07);
    if (s_captured) { s_mleft = false; return; }  // the app owns the mouse: the UI pointer stays put
    if (len >= 4 && d[3]) __atomic_fetch_add(&s_ui_wheel, (int8_t)d[3], __ATOMIC_RELAXED);
    static uint8_t prev_btn = 0;
    if ((d[0] & 0x02) && !(prev_btn & 0x02)) {        // right button pressed: context action
        s_rclick = true;
    }
    prev_btn = d[0];
    int x = s_mx + dx, y = s_my + dy;
    const int W = LV_HOR_RES ? LV_HOR_RES : 1024, H = LV_VER_RES ? LV_VER_RES : 600;
    if (x < 0) x = 0; else if (x >= W) x = W - 1;
    if (y < 0) y = 0; else if (y >= H) y = H - 1;
    s_mx = x;
    s_my = y;
    s_mleft = (d[0] & 0x01) != 0;
}

// ---------------------------------------------------------------- keyboard -> IME

// The HID task (or the NimBLE host task for BLE keyboards) only diffs boot reports into press /
// release events and queues them; an LVGL timer drains the queue on the LVGL thread, applies the
// layout, auto-repeat and the shortcut hook, then feeds the IME. No LVGL lock is ever taken on
// the report path, so a busy UI no longer drops keys.

// Boot-report modifier bits.
constexpr uint8_t kModCtrl = 0x11, kModShift = 0x22, kModLAlt = 0x04, kModAltGr = 0x40, kModGui = 0x88;

// Printable keys of one layout: UTF-8 for plain / Shift / AltGr / AltGr+Shift (nullptr = none).
// Letters a..z are shared by both layouts (QWERTY) and handled apart.
struct KeyMap { uint8_t usage; const char *plain, *shifted, *altgr, *altgr_shift; };

constexpr KeyMap kMapUs[] = {
    {0x1E, "1", "!"},  {0x1F, "2", "@"},  {0x20, "3", "#"},  {0x21, "4", "$"},  {0x22, "5", "%"},
    {0x23, "6", "^"},  {0x24, "7", "&"},  {0x25, "8", "*"},  {0x26, "9", "("},  {0x27, "0", ")"},
    {0x2D, "-", "_"},  {0x2E, "=", "+"},  {0x2F, "[", "{"},  {0x30, "]", "}"},  {0x31, "\\", "|"},
    {0x32, "\\", "|"}, {0x33, ";", ":"},  {0x34, "'", "\""}, {0x35, "`", "~"},  {0x36, ",", "<"},
    {0x37, ".", ">"},  {0x38, "/", "?"},  {0x64, "\\", "|"},
};

// Italian (ISO). AltGr+ì = ~ and AltGr+' = ` follow the Linux layout (Windows has none).
constexpr KeyMap kMapIt[] = {
    {0x1E, "1", "!"},  {0x1F, "2", "\""}, {0x20, "3", "\xC2\xA3"}, {0x21, "4", "$"},  {0x22, "5", "%"},
    {0x23, "6", "&"},  {0x24, "7", "/"},  {0x25, "8", "("},  {0x26, "9", ")"},  {0x27, "0", "="},
    {0x2D, "'", "?", "`"},
    {0x2E, "\xC3\xAC", "^", "~"},                              // ì
    {0x2F, "\xC3\xA8", "\xC3\xA9", "[", "{"},                  // è é
    {0x30, "+", "*", "]", "}"},
    {0x31, "\xC3\xB9", "\xC2\xA7"},                            // ù §
    {0x32, "\xC3\xB9", "\xC2\xA7"},
    {0x33, "\xC3\xB2", "\xC3\xA7", "@"},                       // ò ç
    {0x34, "\xC3\xA0", "\xC2\xB0", "#"},                       // à °
    {0x35, "\\", "|"},
    {0x36, ",", ";"},  {0x37, ".", ":"},  {0x38, "-", "_"},  {0x64, "<", ">"},
};

volatile nv_hid_kbd_layout_t s_layout = NV_HID_KBD_US;

// Numeric keypad (NumLock assumed on: boot keyboards keep it on by default).
const char *keypad_text(uint8_t u) {
    static const char *const kPad[] = {"/", "*", "-", "+", nullptr, "1", "2", "3", "4", "5",
                                       "6", "7", "8", "9", "0", "."};
    return (u >= 0x54 && u <= 0x63) ? kPad[u - 0x54] : nullptr;
}

// Text for a key under the current layout, or nullptr (not printable / no mapping).
const char *usage_to_text(uint8_t u, bool shift, bool altgr) {
    if (u >= 0x04 && u <= 0x1D) {                       // a..z (AltGr+letter: nothing mapped)
        if (altgr) return nullptr;
        static const char lower[] = "a\0b\0c\0d\0e\0f\0g\0h\0i\0j\0k\0l\0m\0n\0o\0p\0q\0r\0s\0t\0u\0v\0w\0x\0y\0z";
        static const char upper[] = "A\0B\0C\0D\0E\0F\0G\0H\0I\0J\0K\0L\0M\0N\0O\0P\0Q\0R\0S\0T\0U\0V\0W\0X\0Y\0Z";
        return (shift ? upper : lower) + 2 * (u - 0x04);
    }
    if (u == 0x2C) return " ";
    if (const char *k = keypad_text(u)) return altgr ? nullptr : k;
    const bool it = s_layout == NV_HID_KBD_IT;
    const KeyMap *m = it ? kMapIt : kMapUs;
    const size_t n = it ? sizeof kMapIt / sizeof *kMapIt : sizeof kMapUs / sizeof *kMapUs;
    for (size_t i = 0; i < n; i++) {
        if (m[i].usage != u) continue;
        if (altgr) return shift ? m[i].altgr_shift : m[i].altgr;
        return shift ? m[i].shifted : m[i].plain;
    }
    return nullptr;
}

// Non-printable usages -> IME special keys. -1 = unhandled.
int usage_to_ime_key(uint8_t u) {
    switch (u) {
        case 0x28: case 0x58: return RK_ENTER;   // main Enter, keypad Enter
        case 0x29: return RK_ESC;
        case 0x2A: return RK_BACKSPACE;
        case 0x2B: return RK_TAB;
        case 0x4C: return RK_DELETE;
        case 0x4F: return RK_RIGHT;
        case 0x50: return RK_LEFT;
        case 0x51: return RK_DOWN;
        case 0x52: return RK_UP;
        case 0x4A: return RK_HOME;
        case 0x4D: return RK_END;
        default:   return -1;
    }
}

uint8_t s_prev_keys[6] = {0};   // also the held-key snapshot for games (nv_hid_host_keys_down)
volatile uint8_t s_mods = 0;    // modifier byte of the last report (nv_hid_host_kbd_state)

nv_hid_host_kbd_hook_cb s_kbd_hook = nullptr;

struct KeyEv { uint8_t usage, mods, pressed; };
QueueHandle_t s_kq = nullptr;          // report path -> LVGL thread
lv_timer_t   *s_kpump = nullptr;       // drains s_kq; created on the LVGL thread
volatile uint8_t s_rep_usage = 0;      // key auto-repeating now (0 = none)
uint32_t s_rep_next = 0;               // lv_tick of the next repeat
constexpr uint32_t kRepDelayMs = 500, kRepRateMs = 33;

// Created on first use by whichever task gets there first (USB HID task, NimBLE host task).
QueueHandle_t kq(void) {
    QueueHandle_t q = __atomic_load_n(&s_kq, __ATOMIC_ACQUIRE);
    if (q) return q;
    QueueHandle_t nq = xQueueCreate(32, sizeof(KeyEv));
    if (!nq) return nullptr;
    QueueHandle_t expected = nullptr;
    if (!__atomic_compare_exchange_n(&s_kq, &expected, nq, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        vQueueDelete(nq);
        return expected;
    }
    return nq;
}

bool is_modifier(uint8_t u) { return u >= 0xE0 && u <= 0xE7; }

// One key press (or auto-repeat) on the LVGL thread: shortcut hook first, then text or an IME key.
void key_dispatch(uint8_t u, uint8_t mods, bool pressed, bool repeat) {
    if (s_kbd_hook && s_kbd_hook(u, mods, pressed, repeat)) return;
    if (!pressed || !s_text_sink || !s_key_sink) return;
    const bool ctrl_alt = (mods & kModCtrl) && (mods & kModLAlt);    // Windows: Ctrl+Alt = AltGr
    const bool altgr = (mods & kModAltGr) || ctrl_alt;
    // Alt / Win chords are shortcuts, never text (the hook had its chance above).
    if ((mods & kModGui) || ((mods & kModLAlt) && !ctrl_alt)) return;
    const bool shift = (mods & kModShift) != 0;
    // Ctrl+letter -> the control character (^C = 0x03): the IME hands it to the field's key
    // hook (terminal shortcuts) and never inserts it.
    if ((mods & kModCtrl) && !altgr && u >= 0x04 && u <= 0x1D) {
        const char s[2] = {(char)(1 + (u - 0x04)), 0};
        s_text_sink(s);
        return;
    }
    if (const char *t = usage_to_text(u, shift, altgr)) { s_text_sink(t); return; }
    const int k = usage_to_ime_key(u);
    if (k >= 0) s_key_sink(k);
}

void kbd_pump_cb(lv_timer_t *) {
    const uint32_t now = lv_tick_get();
    if (s_rclick) {                                     // mouse right button -> UI, LVGL thread
        s_rclick = false;
        if (s_rclick_cb) s_rclick_cb(s_mx, s_my);
    }
    KeyEv ev;
    while (s_kq && xQueueReceive(s_kq, &ev, 0) == pdTRUE) {
        if (ev.pressed) {
            key_dispatch(ev.usage, ev.mods, true, false);
            if (!is_modifier(ev.usage)) {               // modifiers never auto-repeat
                s_rep_usage = ev.usage;
                s_rep_next = now + kRepDelayMs;
            }
        } else {
            if (ev.usage == s_rep_usage) s_rep_usage = 0;
            key_dispatch(ev.usage, ev.mods, false, false);
        }
    }
    const uint8_t r = s_rep_usage;
    if (r && (int32_t)(now - s_rep_next) >= 0) {
        key_dispatch(r, s_mods, true, true);      // live modifiers: Shift let go mid-repeat counts
        s_rep_next = now + kRepRateMs;
    }
}

// The pump timer, on the LVGL thread. Caller holds the port lock.
void kbd_pump_setup_locked(void) {
    if (!s_kpump) s_kpump = lv_timer_create(kbd_pump_cb, 10, nullptr);
}

void kbd_post(uint8_t usage, uint8_t mods, bool pressed) {
    QueueHandle_t q = kq();
    const KeyEv ev = {usage, mods, (uint8_t)pressed};
    if (!q || xQueueSend(q, &ev, 0) != pdTRUE) {
        static bool warned = false;
        if (!warned) { warned = true; NV_LOGW(TAG, "key queue full: key dropped"); }
    }
}

// Forget held keys (disconnect): release events so nothing stays down or auto-repeats.
void kbd_release_all(void) {
    for (uint8_t &p : s_prev_keys) {
        if (p && !is_modifier(p)) kbd_post(p, 0, false);
        p = 0;
    }
    for (int b = 0; b < 8; b++)
        if (s_mods & (1u << b)) kbd_post((uint8_t)(0xE0 + b), 0, false);
    s_mods = 0;
    s_rep_usage = 0;
}

void keyboard_report(const uint8_t *d, size_t len) {
    if (len < 8) return;
    // Boot report: [0]=modifiers, [1]=reserved, [2..7]=up to 6 pressed usages. 0x01 in the key
    // slots = phantom state (too many keys): keep the previous snapshot.
    if (d[2] == 0x01) return;
    const uint8_t mods = d[0], old_mods = s_mods;
    s_mods = mods;
    // Modifiers as keys too (usages 0xE0..0xE7: LCtrl LShift LAlt LWin RCtrl RShift AltGr RWin), so a
    // shortcut can act on Win alone or on letting go of Alt (Alt+Tab). Presses first, releases last.
    for (int b = 0; b < 8; b++)
        if ((mods & ~old_mods) & (1u << b)) kbd_post((uint8_t)(0xE0 + b), mods, true);
    for (int i = 0; i < 6; i++) {                       // released: held before, not now
        const uint8_t u = s_prev_keys[i];
        if (!u) continue;
        bool still = false;
        for (int j = 2; j < 8; j++) if (d[j] == u) { still = true; break; }
        if (!still) kbd_post(u, mods, false);
    }
    for (int i = 2; i < 8; i++) {                       // pressed: new in this report
        const uint8_t u = d[i];
        if (!u) continue;
        bool was = false;
        for (uint8_t p : s_prev_keys) if (p == u) { was = true; break; }
        if (!was) kbd_post(u, mods, true);
    }
    for (int b = 0; b < 8; b++)
        if ((old_mods & ~mods) & (1u << b)) kbd_post((uint8_t)(0xE0 + b), mods, false);
    memcpy(s_prev_keys, d + 2, 6);
    // The pump missed at connect (LVGL lock busy): try again without waiting.
    if (!s_kpump && lvgl_port_lock(0)) { kbd_pump_setup_locked(); lvgl_port_unlock(); }
}

// ---------------------------------------------------------------- presence -> event bus

void devices_changed(void) {
    const bool mouse = s_mouse_present || s_ext_mouse > 0;
    // A cursor without a mouse is a stray dot: hide it until a mouse is back.
    if (s_cursor && lvgl_port_lock(200)) {
        if (mouse && !s_captured) lv_obj_clear_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);
        lvgl_port_unlock();
    }
    nv_event_publish(NV_EV_INPUT_DEVICES, nullptr);
}

// ---------------------------------------------------------------- gamepads -> nv_pad

// Most pads are generic HID: the report descriptor is parsed on connect, each input report
// decodes into raw axes / hats / buttons and the SDL_GameControllerDB mapping (nv_pad_map_*)
// turns those into the standard layout. Nintendo Switch pads (Pro Controller, NSO pads, 8BitDo in
// Switch mode) need a vendor handshake over USB and report in their own 0x30 format; they get a
// native decoder (protocol after SDL's hidapi Switch driver, zlib license). DualShock 4 /
// DualSense are generic HID for input and get their output report for rumble + light bar.
enum PadKind : uint8_t { PAD_GENERIC, PAD_SWITCH, PAD_DS4, PAD_DS5 };

// One entry per connected HID gamepad. The HID task writes; `h` / `slot` change only on connect /
// disconnect.
struct Pad {
    hid_host_device_handle_t h;
    int                      slot;       // nv_pad slot, -1 = free entry
    PadKind                  kind;
    uint8_t                  player;     // light bar colour
    nv_hid_pad_layout_t      layout;
    nv_pad_map_t             map;
};
NV_PSRAM_BSS Pad s_pads[NV_PAD_MAX];
bool s_pads_init = false;

Pad *pad_for(hid_host_device_handle_t h) {
    for (Pad &p : s_pads) if (p.slot >= 0 && p.h == h) return &p;
    return nullptr;
}

constexpr uint16_t kVidNintendo = 0x057e, kVidSony = 0x054c;

bool is_switch(uint16_t vid, uint16_t pid) {
    // Pro Controller, Joy-Con grip, NSO SNES / N64 / Genesis — all speak the Pro protocol on USB.
    return vid == kVidNintendo && (pid == 0x2009 || pid == 0x200e || pid == 0x2017 || pid == 0x2019 || pid == 0x201e);
}

PadKind sony_kind(uint16_t vid, uint16_t pid) {
    if (vid != kVidSony) return PAD_GENERIC;
    if (pid == 0x05c4 || pid == 0x09cc || pid == 0x0ba0) return PAD_DS4;   // DS4 v1 / v2 / wireless adapter
    if (pid == 0x0ce6 || pid == 0x0df2) return PAD_DS5;                    // DualSense / Edge
    return PAD_GENERIC;
}

// Output report through a SET_REPORT control request (the class driver has no interrupt OUT
// path). `r` starts with the report ID.
bool send_output(hid_host_device_handle_t h, uint8_t *r, size_t n) {
    return hid_class_request_set_report(h, HID_REPORT_TYPE_OUTPUT, r[0], r, n) == ESP_OK;
}

// ---- Switch: handshake then "full report" mode (0x30), done off the HID task (it waits for replies
// the HID task delivers).
struct SwitchJob { hid_host_device_handle_t h; };

void switch_packet(hid_host_device_handle_t h, const uint8_t *d, size_t n) {
    if (!pad_for(h)) return;                    // unplugged mid-handshake: the handle is gone
    uint8_t buf[64] = {};
    memcpy(buf, d, n < sizeof buf ? n : sizeof buf);
    send_output(h, buf, sizeof buf);
    vTaskDelay(pdMS_TO_TICKS(60));   // SDL waits ~30 ms for each ack; no ack reading needed
}

void switch_setup_task(void *arg) {
    SwitchJob job = *(SwitchJob *)arg;
    free(arg);
    static const uint8_t kHandshake[] = { 0x80, 0x02 }, kHighSpeed[] = { 0x80, 0x03 }, kForceUsb[] = { 0x80, 0x04 };
    switch_packet(job.h, kHandshake, sizeof kHandshake);
    switch_packet(job.h, kHighSpeed, sizeof kHighSpeed);
    switch_packet(job.h, kHandshake, sizeof kHandshake);
    switch_packet(job.h, kForceUsb, sizeof kForceUsb);
    // Subcommand 0x03 (input report mode) = 0x30, with the neutral rumble frame.
    static const uint8_t kMode[] = { 0x01, 0x00, 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40, 0x03, 0x30 };
    switch_packet(job.h, kMode, sizeof kMode);
    // Player 1 LED (subcommand 0x30), cosmetic.
    static const uint8_t kLed[] = { 0x01, 0x01, 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40, 0x30, 0x01 };
    switch_packet(job.h, kLed, sizeof kLed);
    NV_LOGI(TAG, "Switch controller: USB handshake sent");
    vTaskDelete(nullptr);
}

// 12-bit stick value (centre ~2048, travel ~±1600 on factory calibrations) -> -32768..32767.
int16_t switch_axis(int raw, bool invert) {
    int v = (raw - 2048) * 32767 / 1600;
    if (invert) v = -v;
    if (v < -32768) v = -32768;
    if (v > 32767) v = 32767;
    return (int16_t)v;
}

void switch_report(Pad *p, const uint8_t *d, size_t len) {
    if (len < 12 || d[0] != 0x30) return;
    const uint8_t r = d[3], s = d[4], l = d[5];
    nv_pad_input_t in = {};
    // Positional, like the standard layout: Nintendo B (south) is A, A (east) is B, Y (west) is X.
    if (r & 0x04) in.buttons |= NV_PADB_A;
    if (r & 0x08) in.buttons |= NV_PADB_B;
    if (r & 0x01) in.buttons |= NV_PADB_X;
    if (r & 0x02) in.buttons |= NV_PADB_Y;
    if (r & 0x40) in.buttons |= NV_PADB_RB;
    if (r & 0x80) in.axis[NV_PADA_RT] = 32767;
    if (s & 0x01) in.buttons |= NV_PADB_BACK;
    if (s & 0x02) in.buttons |= NV_PADB_START;
    if (s & 0x04) in.buttons |= NV_PADB_RSTICK;
    if (s & 0x08) in.buttons |= NV_PADB_LSTICK;
    if (s & 0x10) in.buttons |= NV_PADB_GUIDE;
    if (s & 0x20) in.buttons |= NV_PADB_MISC;
    if (l & 0x01) in.buttons |= NV_PADB_DOWN;
    if (l & 0x02) in.buttons |= NV_PADB_UP;
    if (l & 0x04) in.buttons |= NV_PADB_RIGHT;
    if (l & 0x08) in.buttons |= NV_PADB_LEFT;
    if (l & 0x40) in.buttons |= NV_PADB_LB;
    if (l & 0x80) in.axis[NV_PADA_LT] = 32767;
    in.axis[NV_PADA_LX] = switch_axis(d[6] | (d[7] & 0x0f) << 8, false);
    in.axis[NV_PADA_LY] = switch_axis((d[7] >> 4) | d[8] << 4, true);    // Switch Y is up
    in.axis[NV_PADA_RX] = switch_axis(d[9] | (d[10] & 0x0f) << 8, false);
    in.axis[NV_PADA_RY] = switch_axis((d[10] >> 4) | d[11] << 4, true);
    nv_pad_update(p->slot, &in);
}

// ---- DualShock 4 / DualSense rumble + light bar (player colour).
const uint8_t kPlayerRgb[NV_PAD_MAX][3] = { {0, 0, 64}, {64, 0, 0}, {0, 64, 0}, {64, 0, 64} };

bool sony_output(Pad *p, uint16_t low, uint16_t high) {
    if (p->kind == PAD_DS4) {
        uint8_t r[32] = { 0x05, 0x03 };                 // rumble + light bar valid
        r[4] = (uint8_t)(high >> 8);                     // right (small) motor
        r[5] = (uint8_t)(low >> 8);                      // left (big) motor
        memcpy(r + 6, kPlayerRgb[p->player], 3);
        return send_output(p->h, r, sizeof r);
    }
    uint8_t r[48] = { 0x02, 0x03, 0x14 };               // compatible rumble + haptics; light bar + player LEDs
    r[3] = (uint8_t)(high >> 8);
    r[4] = (uint8_t)(low >> 8);
    static const uint8_t kLeds[NV_PAD_MAX] = { 0x04, 0x0a, 0x15, 0x1b };
    r[44] = kLeds[p->player];
    memcpy(r + 45, kPlayerRgb[p->player], 3);
    return send_output(p->h, r, sizeof r);
}

bool sony_rumble(void *ctx, uint16_t low, uint16_t high) {
    Pad *p = (Pad *)ctx;
    return p->slot >= 0 && sony_output(p, low, high);
}

// A HID interface without a boot protocol: a gamepad if its report descriptor says so (or a
// Switch pad, whose descriptor only lists vendor reports).
bool gamepad_connect(hid_host_device_handle_t h) {
    if (!s_pads_init) {
        for (Pad &p : s_pads) p.slot = -1;
        s_pads_init = true;
    }
    Pad *e = nullptr;
    for (Pad &p : s_pads) if (p.slot < 0) { e = &p; break; }
    if (!e) return false;

    hid_host_dev_info_t dev = {};
    hid_host_get_device_info(h, &dev);
    nv_pad_info_t info = {};
    info.source = NV_PAD_SRC_USB_HID;
    info.battery = 255;
    info.vid = dev.VID;
    info.pid = dev.PID;
    size_t k = 0;
    for (; k < sizeof info.name - 1 && dev.iProduct[k]; k++)
        info.name[k] = dev.iProduct[k] < 0x80 ? (char)dev.iProduct[k] : '?';
    info.name[k] = 0;
    if (!info.name[0]) snprintf(info.name, sizeof info.name, "USB gamepad %04x:%04x", info.vid, info.pid);

    e->kind = PAD_GENERIC;
    if (is_switch(info.vid, info.pid)) {
        e->kind = PAD_SWITCH;
        info.mapped = 1;
    } else {
        size_t len = 0;
        const uint8_t *desc = hid_host_get_report_descriptor(h, &len);
        if (!desc || !len || !nv_hid_pad_parse(desc, len, &e->layout)) return false;
        info.mapped = nv_hid_pad_map(NV_PAD_BUS_USB, info.vid, info.pid, &e->layout, &e->map);
        e->kind = sony_kind(info.vid, info.pid);
    }
    e->h = h;
    e->slot = nv_pad_attach(&info);
    if (e->slot < 0) return false;
    e->player = (uint8_t)(nv_pad_count() - 1) % NV_PAD_MAX;
    if (e->kind == PAD_GENERIC)
        NV_LOGI(TAG, "USB gamepad: %d axes, %d hats, %d buttons%s", e->layout.n_axes, e->layout.n_hats,
                e->layout.n_buttons, e->layout.report_id ? " (report ID)" : "");
    return true;
}

// After hid_host_device_start: the vendor setup that needs the interface running.
void gamepad_started(hid_host_device_handle_t h) {
    Pad *p = pad_for(h);
    if (!p) return;
    if (p->kind == PAD_SWITCH) {
        SwitchJob *job = (SwitchJob *)malloc(sizeof *job);
        if (!job) return;
        job->h = h;
        // Self-deleting -> internal-RAM stack (the PSRAM-stack rule excludes self-deleters).
        if (xTaskCreate(switch_setup_task, "pad_switch", 3072, job, 4, nullptr) != pdPASS) free(job);
    } else if (p->kind == PAD_DS4 || p->kind == PAD_DS5) {
        if (sony_output(p, 0, 0)) nv_pad_set_rumble(p->slot, sony_rumble, p);
        else NV_LOGW(TAG, "PlayStation pad: output report refused (no rumble / light bar)");
    }
}

void gamepad_report(hid_host_device_handle_t h, const uint8_t *d, size_t len) {
    Pad *p = pad_for(h);
    if (!p) return;
    if (p->kind == PAD_SWITCH) { switch_report(p, d, len); return; }
    nv_hid_raw_t raw;
    if (!nv_hid_pad_decode(&p->layout, d, len, &raw)) return;
    nv_pad_input_t in;
    nv_pad_map_apply(&p->map, &raw, &in);
    nv_pad_update(p->slot, &in);
}

void gamepad_gone(hid_host_device_handle_t h) {
    if (Pad *p = pad_for(h)) {
        const int slot = p->slot;
        p->slot = -1;
        nv_pad_detach(slot);
    }
}

// ---------------------------------------------------------------- HID host plumbing

void iface_event_cb(hid_host_device_handle_t h, const hid_host_interface_event_t event, void *) {
    switch (event) {
        case HID_HOST_INTERFACE_EVENT_INPUT_REPORT: {
            uint8_t data[64];
            size_t len = 0;
            if (hid_host_device_get_raw_input_report_data(h, data, sizeof data, &len) != ESP_OK) break;
            hid_host_dev_params_t p;
            if (hid_host_device_get_params(h, &p) != ESP_OK) break;
            if (p.proto == HID_PROTOCOL_KEYBOARD)   keyboard_report(data, len);
            else if (p.proto == HID_PROTOCOL_MOUSE) mouse_report(data, len);
            else                                    gamepad_report(h, data, len);
            break;
        }
        case HID_HOST_INTERFACE_EVENT_DISCONNECTED: {
            hid_host_dev_params_t p;
            if (hid_host_device_get_params(h, &p) == ESP_OK) {
                if (p.proto == HID_PROTOCOL_KEYBOARD) {
                    s_kb_present = false;
                    kbd_release_all();                            // no stuck keys in a running game
                    NV_LOGI(TAG, "keyboard disconnected");
                    devices_changed();
                }
                if (p.proto == HID_PROTOCOL_MOUSE) {
                    s_mouse_present = false;
                    s_mleft = false;
                    s_mbuttons = 0;
                    NV_LOGI(TAG, "mouse disconnected");
                    devices_changed();
                }
            }
            gamepad_gone(h);
            hid_host_device_close(h);
            break;
        }
        default:
            break;
    }
}

void device_event_cb(hid_host_device_handle_t h, const hid_host_driver_event_t event, void *) {
    if (event != HID_HOST_DRIVER_EVENT_CONNECTED) return;
    hid_host_dev_params_t p;
    if (hid_host_device_get_params(h, &p) != ESP_OK) return;

    hid_host_device_config_t cfg = {};
    cfg.callback = iface_event_cb;
    cfg.callback_arg = nullptr;
    if (hid_host_device_open(h, &cfg) != ESP_OK) { NV_LOGW(TAG, "device open failed"); return; }

    // Boot protocol: fixed report layout, supported by every real keyboard/mouse.
    if (p.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
        hid_class_request_set_protocol(h, HID_REPORT_PROTOCOL_BOOT);
        if (p.proto == HID_PROTOCOL_KEYBOARD) hid_class_request_set_idle(h, 0, 0);
    }
    const bool pad = p.proto == HID_PROTOCOL_NONE && gamepad_connect(h);
    if (hid_host_device_start(h) != ESP_OK) {
        NV_LOGW(TAG, "device start failed");
        gamepad_gone(h);
        hid_host_device_close(h);
        return;
    }

    if (p.proto == HID_PROTOCOL_KEYBOARD) {
        s_kb_present = true;
        memset(s_prev_keys, 0, sizeof s_prev_keys);
        if (lvgl_port_lock(1000)) { kbd_pump_setup_locked(); lvgl_port_unlock(); }
        NV_LOGI(TAG, "USB keyboard connected (types into the focused field)");
        devices_changed();
    } else if (p.proto == HID_PROTOCOL_MOUSE) {
        s_mouse_present = true;
        if (lvgl_port_lock(1000)) { mouse_indev_setup_locked(); kbd_pump_setup_locked(); lvgl_port_unlock(); }
        NV_LOGI(TAG, "USB mouse connected (pointer + click)");
        devices_changed();
    } else if (pad) {
        gamepad_started(h);
    } else {
        NV_LOGI(TAG, "HID device connected (proto %d) — no handler", (int)p.proto);
    }
}

void hid_init_task(void *) {
    // nv_usb_audio owns usb_host_install; give it a moment, then retry a few times.
    hid_host_driver_config_t drv = {};
    drv.create_background_task = true;
    drv.task_priority = 5;
    drv.stack_size = 4096;
    drv.core_id = 0;
    drv.callback = device_event_cb;
    drv.callback_arg = nullptr;
    for (int i = 0; i < 5; i++) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        const esp_err_t err = hid_host_install(&drv);
        if (err == ESP_OK) {
            NV_LOGI(TAG, "HID host ready (keyboard/mouse hot-plug)");
            vTaskDelete(nullptr);
        }
        NV_LOGW(TAG, "hid_host_install: %s (attempt %d)", esp_err_to_name(err), i + 1);
    }
    NV_LOGE(TAG, "HID host unavailable");
    vTaskDelete(nullptr);
}

}  // namespace

bool nv_hid_host_init(void) {
    static bool s_started = false;
    if (s_started) return true;
    // Self-deleting task -> internal-RAM stack (the PSRAM-stack rule excludes self-deleters).
    if (xTaskCreate(hid_init_task, "hid_init", 3072, nullptr, 3, nullptr) != pdPASS) return false;
    s_started = true;
    return true;
}

void nv_hid_host_set_sink(nv_hid_host_text_cb text, nv_hid_host_key_cb key) {
    s_text_sink = text;
    s_key_sink = key;
}

void nv_hid_host_set_kbd_hook(nv_hid_host_kbd_hook_cb hook) { s_kbd_hook = hook; }
void nv_hid_host_set_rclick_cb(void (*cb)(int x, int y)) { s_rclick_cb = cb; }
void *nv_hid_host_mouse_indev(void) { return s_indev; }

const char *nv_hid_host_key_text(uint8_t usage, uint8_t mods) {
    const bool ctrl_alt = (mods & kModCtrl) && (mods & kModLAlt);
    return usage_to_text(usage, (mods & kModShift) != 0, (mods & kModAltGr) || ctrl_alt);
}

bool nv_hid_host_inject_key(uint8_t usage, uint8_t mods) {
    QueueHandle_t q = kq();
    if (!q || !usage || uxQueueSpacesAvailable(q) < 2) return false;
    kbd_post(usage, mods, true);
    kbd_post(usage, mods, false);   // same drain: no auto-repeat starts
    if (!s_kpump && lvgl_port_lock(1000)) { kbd_pump_setup_locked(); lvgl_port_unlock(); }
    return true;
}

void nv_hid_host_set_layout(nv_hid_kbd_layout_t layout) { s_layout = layout; }
nv_hid_kbd_layout_t nv_hid_host_get_layout(void) { return s_layout; }

// Keyboards / mice from other transports (Bluetooth LE HID in boot protocol) use the same paths as
// USB ones: boot reports, the key queue and the LVGL pointer. Presence is counted per source.

void nv_hid_host_ext_keyboard(bool connected) {
    s_ext_kb += connected ? 1 : -1;
    if (s_ext_kb < 0) s_ext_kb = 0;
    if (connected && lvgl_port_lock(1000)) { kbd_pump_setup_locked(); lvgl_port_unlock(); }
    if (!connected) kbd_release_all();                  // no stuck keys
    devices_changed();
}

void nv_hid_host_ext_mouse(bool connected) {
    s_ext_mouse += connected ? 1 : -1;
    if (s_ext_mouse < 0) s_ext_mouse = 0;
    if (connected && lvgl_port_lock(1000)) { mouse_indev_setup_locked(); kbd_pump_setup_locked(); lvgl_port_unlock(); }
    if (!connected) { s_mleft = false; s_mbuttons = 0; }
    devices_changed();
}

void nv_hid_host_ext_keyboard_report(const uint8_t *r, size_t len) { keyboard_report(r, len); }
void nv_hid_host_ext_mouse_report(const uint8_t *r, size_t len)    { mouse_report(r, len); }

bool nv_hid_host_keyboard_present(void) { return s_kb_present || s_ext_kb > 0; }
bool nv_hid_host_mouse_present(void)    { return s_mouse_present || s_ext_mouse > 0; }

// Written by the HID task, read by a game loop: a torn read costs at most one frame of one key.
int nv_hid_host_keys_down(uint8_t usages[6]) {
    if (!s_kb_present && s_ext_kb <= 0) return 0;
    int n = 0;
    for (int i = 0; i < 6; i++) if (s_prev_keys[i]) usages[n++] = s_prev_keys[i];
    return n;
}

int nv_hid_host_kbd_state(uint8_t out[7]) {
    if (!s_kb_present && s_ext_kb <= 0) return -1;
    out[0] = s_mods;
    int n = 0;
    for (int i = 0; i < 6; i++) if (s_prev_keys[i]) out[1 + n++] = s_prev_keys[i];
    return n;
}

bool nv_hid_host_mouse_take(int32_t *dx, int32_t *dy, int32_t *wheel, uint8_t *buttons) {
    const int32_t x = __atomic_exchange_n(&s_acc_dx, 0, __ATOMIC_RELAXED);
    const int32_t y = __atomic_exchange_n(&s_acc_dy, 0, __ATOMIC_RELAXED);
    const int32_t w = __atomic_exchange_n(&s_acc_wheel, 0, __ATOMIC_RELAXED);
    if (!s_mouse_present && s_ext_mouse <= 0) return false;
    if (dx) *dx = x;
    if (dy) *dy = y;
    if (wheel) *wheel = w;
    if (buttons) *buttons = s_mbuttons;
    return true;
}

void nv_hid_host_mouse_capture(bool on) {
    if (s_captured == on) return;
    s_captured = on;
    s_mleft = false;
    __atomic_store_n(&s_acc_dx, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_acc_dy, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_acc_wheel, 0, __ATOMIC_RELAXED);
    if (s_cursor && lvgl_port_lock(200)) {
        if (on) lv_obj_add_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);
        lvgl_port_unlock();
    }
}

bool nv_hid_host_mouse_state(int *x, int *y, uint8_t *buttons) {
    if (!s_mouse_present && s_ext_mouse <= 0) return false;
    if (x) *x = s_mx;
    if (y) *y = s_my;
    if (buttons) *buttons = s_mbuttons;
    return true;
}
