// nv_usb vendor endpoint — reassembles JPEG frames streamed by the Windows IDD driver.
// Frame packets: udisp header (type/x/y/w/h/payload_total) + JPEG payload split across
// bulk transfers. Complete frames go to the consumer task, which hands them to the
// registered sink (step 2: HW JPEG decode -> panel) or drops them while none is set.
// (Adapted from esp-iot-solution usb_extend_screen app_vendor.c, Apache-2.0.)
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tusb.h"
#include "tusb_config.h"
#include "nv_usb_internal.h"
#include "nv_usb_frame.h"
#include "nv_event_bus.h"

static const char *TAG = "nv_usb_vendor";
static frame_t *current_frame = NULL;

#define NV_USB_VENDOR_RX_BUFSIZE  CFG_TUD_VENDOR_RX_BUFSIZE
#define NV_USB_FRAME_BUF_COUNT    6

// -- Display packet types (udisp protocol)
#define UDISP_TYPE_RGB565  0
#define UDISP_TYPE_RGB888  1
#define UDISP_TYPE_YUV420  2
#define UDISP_TYPE_JPG     3
#define UDISP_TYPE_END     0xff

typedef struct {
    uint16_t crc16;
    uint8_t  type;
    uint8_t  cmd;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    uint32_t frame_id: 10;
    uint32_t payload_total: 22;  // padding 32bit align
} __attribute__((packed)) udisp_frame_header_t;

static nv_usb_frame_cb_t s_sink = NULL;
static void *s_sink_user = NULL;
static volatile uint32_t s_frames_total = 0;
static volatile float s_input_fps = 0.f;

// One streaming_unclaimed advertisement per unclaimed session (see nv_usb_internal.h).
volatile bool nv_usb_stream_advertised = false;

void nv_usb_set_frame_sink(nv_usb_frame_cb_t cb, void *user) {
    s_sink_user = user;
    s_sink = cb;
    if (cb) {
        // Claimed. Deliberately NOT re-armed when the sink unregisters: closing the app
        // while the PC still streams must not bounce it straight back open. Replug re-arms.
        nv_usb_stream_advertised = true;
    }
}

uint32_t nv_usb_frames_total(void) { return s_frames_total; }
float nv_usb_input_fps(void) { return s_input_fps; }

static void transfer_task(void *pvParameter) {
    frame_allocate(NV_USB_FRAME_BUF_COUNT, NV_USB_FRAME_LIMIT_B);
    frame_t *usr_frame = NULL;
    uint32_t unclaimed_run = 0;
    while (1) {
        usr_frame = frame_get_filled();
        nv_usb_frame_cb_t sink = s_sink;
        if (sink) {
            unclaimed_run = 0;
            sink(usr_frame->data, usr_frame->info.total,
                 usr_frame->info.width, usr_frame->info.height, s_sink_user);
        } else if (!nv_usb_stream_advertised && ++unclaimed_run >= 5) {
            // The PC is extending its desktop here but nothing consumes it: tell the
            // SystemUI once (it auto-opens Second Screen / notifies the user).
            nv_usb_stream_advertised = true;
            const nv_usb_display_ev_t ev = { .mounted = true, .streaming_unclaimed = true };
            nv_event_publish(NV_EV_USB_DISPLAY, &ev);
        }
        s_frames_total++;
        frame_return_empty(usr_frame);
    }
}

static bool buffer_skip(frame_info_t *frame_info, uint32_t len) {
    if (frame_info->received + len >= frame_info->total) {
        return true;
    }
    frame_info->received += len;
    return false;
}

static bool start_skip_frame(frame_info_t *frame_info, uint32_t total, uint32_t received) {
    memset(frame_info, 0, sizeof(*frame_info));
    frame_info->total = total;
    return !buffer_skip(frame_info, received);
}

static bool buffer_fill(frame_t *frame, uint8_t *buf, uint32_t len) {
    if (0 == len) {
        return false;
    }

    if (frame_add_data(frame, buf, len) != ESP_OK) {
        ESP_LOGW(TAG, "Drop frame: payload overflow, total=%"PRIu32", received=%"PRIu32", len=%"PRIu32,
                 frame->info.total, frame->info.received, len);
        frame_return_empty(frame);
        return true;
    }
    frame->info.received += len;

    if (frame->info.received >= frame->info.total) {
        frame_send_filled(frame);
        return true;
    }
    return false;
}

void tud_vendor_rx_cb(uint8_t itf, uint8_t const *buffer, uint16_t bufsize) {
    (void) buffer; (void) bufsize;
    static uint8_t rx_buf[NV_USB_VENDOR_RX_BUFSIZE];
    static bool skip_frame = false;
    static frame_info_t skip_frame_info = {0};

    while (tud_vendor_n_available(itf)) {
        if (!current_frame && !skip_frame && tud_vendor_n_available(itf) < sizeof(udisp_frame_header_t)) {
            break;
        }

        // Frame continuation: read straight into the frame buffer — no rx_buf bounce.
        // (Saves one memcpy per bulk chunk; ~100 chunks per 60fps frame.)
        if (current_frame && !skip_frame) {
            size_t space = current_frame->data_buffer_len - current_frame->data_len;
            if (space == 0) {  // shouldn't happen: payload_total <= buffer size
                frame_return_empty(current_frame);
                current_frame = NULL;
                continue;
            }
            if (space > NV_USB_VENDOR_RX_BUFSIZE) space = NV_USB_VENDOR_RX_BUFSIZE;
            int n = tud_vendor_n_read(itf, current_frame->data + current_frame->data_len, space);
            if (n <= 0) {
                continue;
            }
            current_frame->data_len += n;
            current_frame->info.received += n;
            if (current_frame->info.received >= current_frame->info.total) {
                frame_send_filled(current_frame);
                current_frame = NULL;
            }
            continue;
        }

        int read_res = tud_vendor_n_read(itf, rx_buf, NV_USB_VENDOR_RX_BUFSIZE);
        if (read_res <= 0) {
            continue;
        }

        if (!current_frame && !skip_frame) {
            if (read_res < (int)sizeof(udisp_frame_header_t)) {
                ESP_LOGW(TAG, "Drop short frame header: %d", read_res);
                continue;
            }

            udisp_frame_header_t *pblt = (udisp_frame_header_t *)rx_buf;
            uint32_t first_payload_len = read_res - sizeof(udisp_frame_header_t);

            switch (pblt->type) {
            case UDISP_TYPE_RGB565:
            case UDISP_TYPE_RGB888:
            case UDISP_TYPE_YUV420:
                ESP_LOGW(TAG, "Drop unsupported frame type: %u", pblt->type);
                skip_frame = start_skip_frame(&skip_frame_info, pblt->payload_total, first_payload_len);
                break;
            case UDISP_TYPE_JPG: {
                if (pblt->x != 0 || pblt->y != 0 || pblt->width != NV_USB_SCREEN_W || pblt->height != NV_USB_SCREEN_H) {
                    ESP_LOGW(TAG, "Drop frame with unexpected area: x=%u y=%u w=%u h=%u",
                             pblt->x, pblt->y, pblt->width, pblt->height);
                    skip_frame = start_skip_frame(&skip_frame_info, pblt->payload_total, first_payload_len);
                    break;
                }
                if (pblt->payload_total == 0 || pblt->payload_total > NV_USB_FRAME_LIMIT_B) {
                    ESP_LOGW(TAG, "Drop frame: payload_total=%"PRIu32", limit=%u",
                             (uint32_t)pblt->payload_total, NV_USB_FRAME_LIMIT_B);
                    skip_frame = start_skip_frame(&skip_frame_info, pblt->payload_total, first_payload_len);
                    break;
                }

                // Rolling input fps over 50-frame windows.
                static int fps_count = 0;
                static int64_t start_time = 0;
                fps_count++;
                if (fps_count == 50) {
                    int64_t end_time = esp_timer_get_time();
                    s_input_fps = 1000000.0f / ((end_time - start_time) / 50.0f);
                    ESP_LOGI(TAG, "Input fps: %.1f", s_input_fps);
                    start_time = end_time;
                    fps_count = 0;
                }

                current_frame = frame_get_empty();
                if (current_frame) {
                    current_frame->info.width = pblt->width;
                    current_frame->info.height = pblt->height;
                    current_frame->info.total = pblt->payload_total;
                    current_frame->info.received = 0;
                    if (buffer_fill(current_frame, &rx_buf[sizeof(udisp_frame_header_t)], first_payload_len)) {
                        current_frame = NULL;
                    }
                } else {
                    skip_frame = start_skip_frame(&skip_frame_info, pblt->payload_total, first_payload_len);
                    ESP_LOGD(TAG, "no empty frame buffer, skipping");
                    // Let lower-priority tasks (frame consumer) run so buffers free up.
                    vTaskDelay(1 / portTICK_PERIOD_MS);
                }
                break;
            }
            case UDISP_TYPE_END:
                break;
            default:
                ESP_LOGE(TAG, "unknown packet type %u", pblt->type);
                break;
            }
        } else {  // skip_frame (continuation handled by the direct-read path above)
            if (buffer_skip(&skip_frame_info, read_res)) {
                current_frame = NULL;
                skip_frame = false;
            }
        }
    }
}

esp_err_t nv_usb_vendor_init(void) {
    xTaskCreatePinnedToCore(transfer_task, "nv_usb_xfer", 4096, NULL, NV_USB_VENDOR_PRIO, NULL, 1);
    return ESP_OK;
}
