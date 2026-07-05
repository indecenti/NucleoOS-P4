// nv_usb frame pool. See nv_usb_frame.h.
#include <string.h>
#include <stdlib.h>
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "nv_usb_frame.h"
#include "sdkconfig.h"
#if CONFIG_SOC_JPEG_DECODE_SUPPORTED
#include "driver/jpeg_decode.h"
#endif

static const char *TAG = "nv_usb_frame";

static QueueHandle_t empty_fb_queue = NULL;
static QueueHandle_t filled_fb_queue = NULL;

esp_err_t frame_allocate(int nb_of_fb, size_t fb_size) {
    // Frames are passed by reference through the queues.
    empty_fb_queue = xQueueCreate(nb_of_fb, sizeof(frame_t *));
    ESP_RETURN_ON_FALSE(empty_fb_queue, ESP_ERR_NO_MEM, TAG, "empty_fb_queue alloc failed");
    filled_fb_queue = xQueueCreate(nb_of_fb, sizeof(frame_t *));
    ESP_RETURN_ON_FALSE(filled_fb_queue, ESP_ERR_NO_MEM, TAG, "filled_fb_queue alloc failed");

    for (int i = 0; i < nb_of_fb; i++) {
        frame_t *this_fb = malloc(sizeof(frame_t));
        ESP_RETURN_ON_FALSE(this_fb, ESP_ERR_NO_MEM, TAG, "frame header alloc failed");
#if CONFIG_SOC_JPEG_DECODE_SUPPORTED
        // Decoder-input-capable memory: step 2 hands these buffers to the HW JPEG engine.
        size_t malloc_size = 0;
        jpeg_decode_memory_alloc_cfg_t tx_mem_cfg = {
            .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
        };
        uint8_t *this_data = (uint8_t *)jpeg_alloc_decoder_mem(fb_size, &tx_mem_cfg, &malloc_size);
#else
        uint8_t *this_data = (uint8_t *)heap_caps_aligned_alloc(16, fb_size, MALLOC_CAP_SPIRAM);
#endif
        if (!this_data) {
            free(this_fb);
            ESP_LOGE(TAG, "frame data alloc failed (%u B)", (unsigned)fb_size);
            return ESP_ERR_NO_MEM;
        }

        this_fb->data = this_data;
        this_fb->data_buffer_len = fb_size;
        this_fb->data_len = 0;

        const BaseType_t result = xQueueSend(empty_fb_queue, &this_fb, 0);
        assert(pdPASS == result);
    }
    return ESP_OK;
}

void frame_reset(frame_t *frame) {
    assert(frame);
    frame->data_len = 0;
}

esp_err_t frame_return_empty(frame_t *frame) {
    frame_reset(frame);
    BaseType_t result = xQueueSend(empty_fb_queue, &frame, 0);
    ESP_RETURN_ON_FALSE(result == pdPASS, ESP_ERR_NO_MEM, TAG, "empty_fb_queue full");
    return ESP_OK;
}

esp_err_t frame_send_filled(frame_t *frame) {
    frame_reset(frame);
    BaseType_t result = xQueueSend(filled_fb_queue, &frame, 0);
    ESP_RETURN_ON_FALSE(result == pdPASS, ESP_ERR_NO_MEM, TAG, "filled_fb_queue full");
    return ESP_OK;
}

esp_err_t frame_add_data(frame_t *frame, const uint8_t *data, size_t data_len) {
    ESP_RETURN_ON_FALSE(frame && data && data_len, ESP_ERR_INVALID_ARG, TAG, "invalid args");
    if (frame->data_buffer_len < frame->data_len + data_len) {
        ESP_LOGD(TAG, "frame buffer overflow");
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(frame->data + frame->data_len, data, data_len);
    frame->data_len += data_len;
    return ESP_OK;
}

frame_t *frame_get_empty(void) {
    frame_t *this_fb;
    if (xQueueReceive(empty_fb_queue, &this_fb, 0) == pdPASS) {
        return this_fb;
    }
    return NULL;
}

frame_t *frame_get_filled(void) {
    frame_t *this_fb;
    if (xQueueReceive(filled_fb_queue, &this_fb, portMAX_DELAY) == pdPASS) {
        return this_fb;
    }
    return NULL;
}
