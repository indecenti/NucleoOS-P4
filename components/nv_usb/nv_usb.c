// nv_usb — USB PHY + TinyUSB device stack bring-up on the P4 OTG-HS controller.
// The board's HS Type-C (interface #9) is the data port; the FS Type-C (COM5,
// USB-Serial/JTAG) is a separate peripheral and keeps working for flash/monitor.
// (Adapted from esp-iot-solution usb_extend_screen app_usb.c, Apache-2.0.)
#include <stdint.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_private/usb_phy.h"
#include "tusb.h"
#include "device/usbd.h"
#include "usb_descriptors.h"
#include "nv_usb_internal.h"
#include "nv_log.h"
#include "nv_event_bus.h"

static const char *TAG = "nv_usb";
static usb_phy_handle_t s_phy_hdl = NULL;
static volatile bool s_mounted = false;
static bool s_running = false;

bool nv_usb_mounted(void) { return s_mounted; }

static esp_err_t usb_phy_init(void) {
    if (s_phy_hdl) {
        return ESP_OK;
    }

    usb_phy_config_t phy_conf = {
        .controller = USB_PHY_CTRL_OTG,
        .otg_mode = USB_OTG_MODE_DEVICE,
        .otg_speed = USB_PHY_SPEED_UNDEFINED,
    };

#if CONFIG_IDF_TARGET_ESP32P4
    // OTG-HS controller behind the UTMI PHY (480 Mbps).
    phy_conf.target = USB_PHY_TARGET_UTMI;
    phy_conf.otg_speed = USB_PHY_SPEED_HIGH;
#else
    phy_conf.target = USB_PHY_TARGET_INT;
    phy_conf.otg_speed = USB_PHY_SPEED_FULL;
#endif

    ESP_RETURN_ON_ERROR(usb_new_phy(&phy_conf, &s_phy_hdl), TAG, "USB PHY init failed");
    return ESP_OK;
}

static void tusb_device_task(void *arg) {
    (void) arg;
    while (1) {
        tud_task();
    }
}

esp_err_t nv_usb_init(void) {
    if (s_running) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(usb_phy_init(), TAG, "USB PHY init failed");
    if (!tusb_init()) {
        NV_LOGE(TAG, "TinyUSB device stack init failed");
        return ESP_FAIL;
    }

    esp_err_t ret = nv_usb_vendor_init();
    ESP_RETURN_ON_FALSE(ret == ESP_OK, ESP_FAIL, TAG, "vendor init failed");

#if CFG_TUD_HID
    ret = nv_usb_hid_init();
    ESP_RETURN_ON_FALSE(ret == ESP_OK, ESP_FAIL, TAG, "hid init failed");
#endif

    xTaskCreate(tusb_device_task, "nv_usb_tud", 4096, NULL, NV_USB_TASK_PRIO, NULL);
    s_running = true;
    NV_LOGI(TAG, "extend-screen device up (VID 0x%04X PID 0x%04X, %dx%d @ %d fps max)",
            USB_VID, USB_PID, NV_USB_SCREEN_W, NV_USB_SCREEN_H, NV_USB_MAX_FPS);
    return ESP_OK;
}

/************************ TinyUSB device callbacks ************************/

void tud_mount_cb(void) {
    s_mounted = true;
    NV_LOGI(TAG, "USB mounted (host configured device)");
    const nv_usb_display_ev_t ev = { .mounted = true, .streaming_unclaimed = false };
    nv_event_publish(NV_EV_USB_DISPLAY, &ev);
}

void tud_umount_cb(void) {
    s_mounted = false;
    nv_usb_stream_advertised = false;  // a replug advertises the stream again
    NV_LOGI(TAG, "USB unmounted");
    const nv_usb_display_ev_t ev = { .mounted = false, .streaming_unclaimed = false };
    nv_event_publish(NV_EV_USB_DISPLAY, &ev);
}

void tud_suspend_cb(bool remote_wakeup_en) {
    (void) remote_wakeup_en;
    s_mounted = false;
    NV_LOGI(TAG, "USB suspended");
}

void tud_resume_cb(void) {
    s_mounted = true;
    NV_LOGI(TAG, "USB resumed");
}
