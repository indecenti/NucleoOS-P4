// nv_usb internals shared between the usb/vendor/hid translation units.
#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "nv_usb.h"

#ifdef __cplusplus
extern "C" {
#endif

// Task priorities (example defaults from esp-iot-solution usb_extend_screen).
#define NV_USB_TASK_PRIO     6   // tud_task loop
#define NV_USB_VENDOR_PRIO   5   // JPEG frame consumer
#define NV_USB_HID_PRIO      5   // touch report sender

// HID touchscreen report layout — must match TUD_HID_REPORT_DESC_TOUCH_SCREEN.
typedef struct {
    uint8_t  press_down;
    uint8_t  index;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
} __attribute__((packed)) touch_report_t;

typedef struct {
    uint32_t report_id;
    struct {
        touch_report_t data[NV_USB_TOUCH_MAX];
        uint8_t cnt;
    } __attribute__((packed)) touch_report;
} __attribute__((packed)) hid_report_t;

esp_err_t nv_usb_vendor_init(void);
esp_err_t nv_usb_hid_init(void);
void nv_usb_hid_send(hid_report_t report);

// One NV_EV_USB_DISPLAY{streaming_unclaimed} per no-sink streaming session (vendor.c);
// re-armed on unmount so a replug advertises again.
extern volatile bool nv_usb_stream_advertised;

#ifdef __cplusplus
}
#endif
