// nv_hid_host — USB HID host (keyboard -> IME, mouse -> LVGL pointer). See nv_hid_host.h.
#include "nv_hid_host.h"

#include "nv_log.h"

#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"
#include "usb/hid_usage_mouse.h"

#include "esp_lvgl_port.h"   // lvgl_port_lock — IME injection + indev setup off the LVGL thread
#include "lvgl.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

uint8_t s_prev_keys[6] = {0};

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
            break;
        }
        case HID_HOST_INTERFACE_EVENT_DISCONNECTED: {
            hid_host_dev_params_t p;
            if (hid_host_device_get_params(h, &p) == ESP_OK) {
                if (p.proto == HID_PROTOCOL_KEYBOARD) { s_kb_present = false; NV_LOGI(TAG, "keyboard disconnected"); }
                if (p.proto == HID_PROTOCOL_MOUSE)    { s_mouse_present = false; NV_LOGI(TAG, "mouse disconnected"); }
            }
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
    if (hid_host_device_start(h) != ESP_OK) { NV_LOGW(TAG, "device start failed"); hid_host_device_close(h); return; }

    if (p.proto == HID_PROTOCOL_KEYBOARD) {
        s_kb_present = true;
        memset(s_prev_keys, 0, sizeof s_prev_keys);
        NV_LOGI(TAG, "USB keyboard connected (types into the focused field)");
    } else if (p.proto == HID_PROTOCOL_MOUSE) {
        s_mouse_present = true;
        if (lvgl_port_lock(1000)) { mouse_indev_setup_locked(); lvgl_port_unlock(); }
        NV_LOGI(TAG, "USB mouse connected (pointer + click)");
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
