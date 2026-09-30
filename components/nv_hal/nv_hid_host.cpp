// nv_hid_host — USB HID host (keyboard -> IME, mouse -> LVGL pointer, gamepads -> nv_pad).
// See nv_hid_host.h.
#include "nv_hid_host.h"
#include "nv_hid_gamepad.h"

#include "nv_log.h"
#include "nv_mem_attr.h"   // NV_PSRAM_BSS: gamepad layouts are cold data

#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"
#include "usb/hid_usage_mouse.h"

#include "esp_lvgl_port.h"   // lvgl_port_lock — IME injection + indev setup off the LVGL thread
#include "lvgl.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static const char *TAG = "usb_hid";

namespace {

volatile bool s_kb_present = false;
volatile bool s_mouse_present = false;

// Keyboard sinks (wired by app_main -> nv_ime; see header). NULL until registered.
nv_hid_host_text_cb s_text_sink = nullptr;
nv_hid_host_key_cb  s_key_sink = nullptr;

// Mirrors nv_ime_remote_key_t (nv_ime.h) — kept numeric here to avoid the nv_ui dependency.
enum { RK_ENTER = 0, RK_ESC, RK_BACKSPACE, RK_DELETE, RK_TAB, RK_LEFT, RK_RIGHT, RK_UP, RK_DOWN };

// ---------------------------------------------------------------- mouse -> LVGL pointer

volatile int  s_mx = 512, s_my = 300;     // cursor position (panel coords)
volatile bool s_mleft = false;
volatile uint8_t s_mbuttons = 0;          // HID button bits (games read all three)
lv_indev_t   *s_indev = nullptr;
lv_obj_t     *s_cursor = nullptr;

void mouse_read_cb(lv_indev_t *, lv_indev_data_t *data) {
    data->point.x = (int32_t)s_mx;
    data->point.y = (int32_t)s_my;
    data->state = s_mleft ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

// Create the pointer indev + cursor dot once, on the LVGL thread (caller holds the port lock).
void mouse_indev_setup_locked(void) {
    if (s_indev) return;
    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, mouse_read_cb);

    s_cursor = lv_obj_create(lv_layer_sys());     // top layer: above every app/screen
    lv_obj_remove_style_all(s_cursor);
    lv_obj_set_size(s_cursor, 14, 14);
    lv_obj_set_style_radius(s_cursor, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_cursor, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_cursor, lv_color_hex(0x2F6BFF), 0);   // brand-ish blue dot
    lv_obj_set_style_border_width(s_cursor, 2, 0);
    lv_obj_set_style_border_color(s_cursor, lv_color_white(), 0);
    lv_obj_clear_flag(s_cursor, LV_OBJ_FLAG_CLICKABLE);
    lv_indev_set_cursor(s_indev, s_cursor);       // LVGL keeps the dot glued to the pointer
}

void mouse_report(const uint8_t *d, size_t len) {
    if (len < 3) return;
    // Boot report: [0]=buttons, [1]=dx, [2]=dy (int8).
    const int8_t dx = (int8_t)d[1], dy = (int8_t)d[2];
    int x = s_mx + dx, y = s_my + dy;
    const int W = LV_HOR_RES ? LV_HOR_RES : 1024, H = LV_VER_RES ? LV_VER_RES : 600;
    if (x < 0) x = 0; else if (x >= W) x = W - 1;
    if (y < 0) y = 0; else if (y >= H) y = H - 1;
    s_mx = x;
    s_my = y;
    s_mleft = (d[0] & 0x01) != 0;
    s_mbuttons = (uint8_t)(d[0] & 0x07);
}

// ---------------------------------------------------------------- keyboard -> IME

// HID usage -> ASCII, US layout, [0]=plain [1]=shifted. 0 = not printable here.
struct KeyMap { uint8_t usage; char plain; char shifted; };
constexpr KeyMap kMap[] = {
    {0x2C, ' ', ' '},  {0x2D, '-', '_'},  {0x2E, '=', '+'},  {0x2F, '[', '{'},
    {0x30, ']', '}'},  {0x31, '\\', '|'}, {0x33, ';', ':'},  {0x34, '\'', '"'},
    {0x35, '`', '~'},  {0x36, ',', '<'},  {0x37, '.', '>'},  {0x38, '/', '?'},
};

char usage_to_char(uint8_t u, bool shift) {
    if (u >= 0x04 && u <= 0x1D) {                       // a..z
        char c = (char)('a' + (u - 0x04));
        return shift ? (char)(c - 32) : c;
    }
    if (u >= 0x1E && u <= 0x27) {                       // 1..9,0 + shifted symbols
        static const char digit[] = "1234567890";
        static const char sym[]   = "!@#$%^&*()";
        return shift ? sym[u - 0x1E] : digit[u - 0x1E];
    }
    for (const KeyMap &m : kMap) if (m.usage == u) return shift ? m.shifted : m.plain;
    return 0;
}

// Non-printable usages -> IME special keys. -1 = unhandled.
int usage_to_ime_key(uint8_t u) {
    switch (u) {
        case 0x28: return RK_ENTER;
        case 0x29: return RK_ESC;
        case 0x2A: return RK_BACKSPACE;
        case 0x2B: return RK_TAB;
        case 0x4C: return RK_DELETE;
        case 0x4F: return RK_RIGHT;
        case 0x50: return RK_LEFT;
        case 0x51: return RK_DOWN;
        case 0x52: return RK_UP;
        default:   return -1;
    }
}

uint8_t s_prev_keys[6] = {0};   // also the held-key snapshot for games (nv_hid_host_keys_down)

void keyboard_report(const uint8_t *d, size_t len) {
    if (len < 8) return;
    // Boot report: [0]=modifiers, [1]=reserved, [2..7]=up to 6 pressed usages.
    const bool shift = (d[0] & 0x22) != 0;              // L/R shift
    for (int i = 2; i < 8; i++) {
        const uint8_t u = d[i];
        if (!u) continue;
        bool was = false;                               // only newly pressed keys fire
        for (uint8_t p : s_prev_keys) if (p == u) { was = true; break; }
        if (was) continue;
        const char c = usage_to_char(u, shift);
        const int  k = c ? -1 : usage_to_ime_key(u);
        if ((!c && k < 0) || !s_text_sink || !s_key_sink) continue;
        if (lvgl_port_lock(50)) {                       // IME sinks are LVGL-thread only
            if (c) { char s[2] = {c, 0}; s_text_sink(s); }
            else     s_key_sink(k);
            lvgl_port_unlock();
        }
    }
    memcpy(s_prev_keys, d + 2, 6);
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
                    memset(s_prev_keys, 0, sizeof s_prev_keys);   // no stuck keys in a running game
                    NV_LOGI(TAG, "keyboard disconnected");
                }
                if (p.proto == HID_PROTOCOL_MOUSE) { s_mouse_present = false; s_mbuttons = 0; NV_LOGI(TAG, "mouse disconnected"); }
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
        NV_LOGI(TAG, "USB keyboard connected (types into the focused field)");
    } else if (p.proto == HID_PROTOCOL_MOUSE) {
        s_mouse_present = true;
        if (lvgl_port_lock(1000)) { mouse_indev_setup_locked(); lvgl_port_unlock(); }
        NV_LOGI(TAG, "USB mouse connected (pointer + click)");
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

bool nv_hid_host_keyboard_present(void) { return s_kb_present; }
bool nv_hid_host_mouse_present(void)    { return s_mouse_present; }

// Written by the HID task, read by a game loop: a torn read costs at most one frame of one key.
int nv_hid_host_keys_down(uint8_t usages[6]) {
    if (!s_kb_present) return 0;
    int n = 0;
    for (int i = 0; i < 6; i++) if (s_prev_keys[i]) usages[n++] = s_prev_keys[i];
    return n;
}

bool nv_hid_host_mouse_state(int *x, int *y, uint8_t *buttons) {
    if (!s_mouse_present) return false;
    if (x) *x = s_mx;
    if (y) *y = s_my;
    if (buttons) *buttons = s_mbuttons;
    return true;
}
