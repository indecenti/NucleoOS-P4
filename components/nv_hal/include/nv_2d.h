// nv_2d — one lock for the P4's 2D engines (PPA and the JPEG codec), which share the 2D-DMA.
//
// Every PPA operation and every JPEG decode/encode in the OS goes through these wrappers instead of
// calling ppa_do_* / jpeg_*_process directly.
//
// Why: on this chip revision the JPEG codec can only use 2D-DMA channel 0 (the only one with the
// reorder feature), and the PPA takes channel 0 whenever the display's frame-buffer copy holds
// channel 1. A JPEG job submitted while a PPA job runs therefore waits in the 2D-DMA queue, and its
// timeout (60-100 ms here; a large PPA scale takes ~200 ms) can expire before it ever started. IDF
// 5.5.2 then calls dma2d_force_end() on a job that is NOT in flight: it either stops another
// client's transfer (a PPA or display job then never completes) or fails and leaves the queued job
// pointing at descriptors on the stack of a call that already returned — a later pick-up runs on
// garbage (crash, or an interrupt that re-fires forever). Serializing PPA and JPEG removes the
// queueing entirely, so a JPEG timeout can only ever hit a job that is really running, which
// force_end handles correctly.
//
// The wait for the lock is bounded: on a timeout the wrapper returns ESP_ERR_TIMEOUT without having
// touched the hardware, so a stuck engine costs its caller one frame, never the whole OS.
#pragma once
#include "driver/jpeg_decode.h"
#include "driver/jpeg_encode.h"
#include "driver/ppa.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t nv_2d_srm(ppa_client_handle_t client, const ppa_srm_oper_config_t *op);

esp_err_t nv_2d_jpeg_decode(jpeg_decoder_handle_t engine, const jpeg_decode_cfg_t *cfg, const uint8_t *in,
                            uint32_t in_len, uint8_t *out, uint32_t out_cap, uint32_t *out_size);

esp_err_t nv_2d_jpeg_encode(jpeg_encoder_handle_t engine, const jpeg_encode_cfg_t *cfg, const uint8_t *in,
                            uint32_t in_len, uint8_t *out, uint32_t out_cap, uint32_t *out_size);

#ifdef __cplusplus
}
#endif
