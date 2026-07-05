// nv_usb — USB extended-screen device on the OTG-HS Type-C port (interface #9).
// Enumerates as the Espressif extend-screen composite (vendor bulk endpoint for JPEG
// frames + HID touchscreen), VID 0x303A PID 0x2986 — matched by the signed Windows
// IDD driver (xfz1986_usb_graphic). The PC extends its desktop onto the board; frames
// arrive as JPEG over the vendor endpoint and are handed to the registered sink.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "nv_usb_screen.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bring up USB PHY (UTMI, high-speed) + TinyUSB device stack + vendor/HID tasks.
// Idempotent; returns ESP_OK if already running.
esp_err_t nv_usb_init(void);

// True while the host has configured the device (cable plugged + enumeration done).
bool nv_usb_mounted(void);

// Complete JPEG frames from the PC are delivered on the consumer task through this
// sink. With no sink registered frames are counted and dropped (smoke-test mode).
typedef void (*nv_usb_frame_cb_t)(const uint8_t *jpg, uint32_t len,
                                  uint16_t w, uint16_t h, void *user);
void nv_usb_set_frame_sink(nv_usb_frame_cb_t cb, void *user);

// Report fingers to the PC (coords in NV_USB_SCREEN_W/H space). cnt==0 = all released.
typedef struct {
    uint8_t  id;        // track id (stable per finger)
    uint16_t x, y;
    uint8_t  strength;  // contact size hint
} nv_usb_touch_pt_t;
void nv_usb_touch_report(const nv_usb_touch_pt_t *pts, uint8_t cnt);

// Stats (Diagnostics / app UI).
uint32_t nv_usb_frames_total(void);   // frames fully received since boot
float    nv_usb_input_fps(void);      // rolling input fps (0 when idle)

// NV_EV_USB_DISPLAY payload. Published from the USB tasks (subscribers must only set
// flags — hop to the LVGL thread for any UI work):
//  - mounted edge (cable plugged + host configured, or lost)
//  - streaming_unclaimed: the PC is sending desktop frames but no app consumes them
//    (SystemUI reacts by auto-opening Second Screen / notifying).
typedef struct {
    bool mounted;
    bool streaming_unclaimed;
} nv_usb_display_ev_t;

#ifdef __cplusplus
}
#endif
