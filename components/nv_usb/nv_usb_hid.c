// nv_usb HID — multi-touch reports back to the PC (board touch controls the extended
// desktop area). Reports are queued and sent one at a time; the EP-complete callback
// releases the next one. (Adapted from esp-iot-solution usb_extend_screen app_hid.c.)
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "tusb.h"
#include "device/usbd.h"
#include "tusb_config.h"
#include "usb_descriptors.h"
#include "nv_usb_internal.h"

#if CFG_TUD_HID

static const char *TAG = "nv_usb_hid";

typedef struct {
    TaskHandle_t task_handle;
    QueueHandle_t hid_queue;
} nv_usb_hid_ctx_t;

static nv_usb_hid_ctx_t *s_hid = NULL;

void nv_usb_hid_send(hid_report_t report) {
    if (!s_hid) {
        return;
    }
    if (tud_suspended()) {
        // Wake the host if it allows remote wakeup.
        tud_remote_wakeup();
    } else {
        xQueueSend(s_hid->hid_queue, &report, 0);
    }
}

// Public API: pack finger points into the report layout the descriptor promises.
void nv_usb_touch_report(const nv_usb_touch_pt_t *pts, uint8_t cnt) {
    hid_report_t report = {0};
    report.report_id = REPORT_ID_TOUCH;
    if (cnt > NV_USB_TOUCH_MAX) {
        cnt = NV_USB_TOUCH_MAX;
    }
    for (uint8_t i = 0; i < cnt; i++) {
        report.touch_report.data[i].press_down = 1;
        report.touch_report.data[i].index = pts[i].id;
        report.touch_report.data[i].x = pts[i].x;
        report.touch_report.data[i].y = pts[i].y;
        report.touch_report.data[i].width = pts[i].strength;
        report.touch_report.data[i].height = pts[i].strength;
    }
    report.touch_report.cnt = cnt;
    nv_usb_hid_send(report);
}

static void hid_task(void *arg) {
    (void) arg;
    hid_report_t report;
    while (1) {
        if (xQueueReceive(s_hid->hid_queue, &report, portMAX_DELAY)) {
            if (tud_suspended()) {
                tud_remote_wakeup();
                xQueueReset(s_hid->hid_queue);
            } else if (report.report_id == REPORT_ID_TOUCH) {
                tud_hid_n_report(0, REPORT_ID_TOUCH, &report.touch_report, sizeof(report.touch_report));
                // Wait for EP completion before sending the next report.
                if (!ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100))) {
                    ESP_LOGW(TAG, "report not sent");
                }
            }
        }
    }
}

esp_err_t nv_usb_hid_init(void) {
    if (s_hid) {
        return ESP_OK;
    }

    s_hid = calloc(1, sizeof(nv_usb_hid_ctx_t));
    ESP_RETURN_ON_FALSE(s_hid, ESP_ERR_NO_MEM, TAG, "calloc failed");
    s_hid->hid_queue = xQueueCreate(10, sizeof(hid_report_t));
    if (!s_hid->hid_queue) {
        free(s_hid);
        s_hid = NULL;
        return ESP_ERR_NO_MEM;
    }

    xTaskCreate(hid_task, "nv_usb_hid", 4096, NULL, NV_USB_HID_PRIO, &s_hid->task_handle);
    if (!s_hid->task_handle) {
        vQueueDelete(s_hid->hid_queue);
        free(s_hid);
        s_hid = NULL;
        return ESP_ERR_NO_MEM;
    }
    xTaskNotifyGive(s_hid->task_handle);
    return ESP_OK;
}

/************************ TinyUSB HID callbacks ************************/

// Sent-complete: release the next queued report.
void tud_hid_report_complete_cb(uint8_t itf, uint8_t const *report, uint16_t len) {
    (void) itf; (void) report; (void) len;
    if (s_hid && s_hid->task_handle) {
        xTaskNotifyGive(s_hid->task_handle);
    }
}

uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen) {
    (void) itf; (void) report_type; (void) reqlen;
    switch (report_id) {
    case REPORT_ID_MAX_COUNT:
        buffer[0] = NV_USB_TOUCH_MAX;
        return 1;
    default:
        break;
    }
    return 0;
}

void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize) {
    (void) itf; (void) report_id; (void) report_type; (void) buffer; (void) bufsize;
}

#endif // CFG_TUD_HID
