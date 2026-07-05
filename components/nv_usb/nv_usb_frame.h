// nv_usb frame pool — fixed set of PSRAM JPEG buffers cycled between the vendor RX
// callback (producer) and the consumer task. Buffers come from jpeg_alloc_decoder_mem
// so step-2 can feed them straight to the P4 hardware JPEG decoder without a copy.
// (Adapted from esp-iot-solution usb_extend_screen usb_frame.c, Apache-2.0.)
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t width;
    uint16_t height;
    uint32_t received;
    uint32_t total;
} frame_info_t;

typedef struct {
    size_t data_buffer_len;  // capacity
    size_t data_len;         // bytes stored so far
    uint8_t *data;
    frame_info_t info;
} frame_t;

esp_err_t frame_allocate(int nb_of_fb, size_t fb_size);
void frame_reset(frame_t *frame);
esp_err_t frame_return_empty(frame_t *frame);
esp_err_t frame_send_filled(frame_t *frame);
esp_err_t frame_add_data(frame_t *frame, const uint8_t *data, size_t data_len);
frame_t *frame_get_empty(void);
frame_t *frame_get_filled(void);

#ifdef __cplusplus
}
#endif
