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

// PPA scale/rotate/mirror. The job itself is bounded too: the driver's BLOCKING mode waits on it
// forever, and a configuration the driver accepts but the hardware refuses never completes (the
// H.264 I420 path froze the OS that way). It runs NON_BLOCKING and is waited for at most 1.5 s; on a
// timeout the PPA is retired until the next restart (the stuck job keeps the engine's queue and the
// driver cannot abort it) and every later call returns ESP_ERR_NOT_SUPPORTED at once: fall back to
// the CPU or skip the frame. op->mode and op->user_data are ignored, and nv_2d owns the client's
// on_trans_done callback.
esp_err_t nv_2d_srm(ppa_client_handle_t client, const ppa_srm_oper_config_t *op);
// False once the PPA was retired (above): the UI tells the user, /api/display reports it.
bool nv_2d_ppa_ok(void);
// Test drill (GET /api/display?ppa_drill=1): the next nv_2d_srm job is not submitted, so its wait
// times out exactly like a stuck job's and the PPA is retired, without touching the hardware.
void nv_2d_ppa_drill(void);

esp_err_t nv_2d_jpeg_decode(jpeg_decoder_handle_t engine, const jpeg_decode_cfg_t *cfg, const uint8_t *in,
                            uint32_t in_len, uint8_t *out, uint32_t out_cap, uint32_t *out_size);

esp_err_t nv_2d_jpeg_encode(jpeg_encoder_handle_t engine, const jpeg_encode_cfg_t *cfg, const uint8_t *in,
                            uint32_t in_len, uint8_t *out, uint32_t out_cap, uint32_t *out_size);

// Bulk copy by AXI-GDMA: long PSRAM bursts instead of CPU cache-line misses, several times faster
// than memcpy while the camera and the panel DMA load PSRAM. Cache coherence is handled (source
// written back, destination invalidated before the transfer). dst, src and n must be cache-line
// aligned (ESP_ERR_INVALID_ARG otherwise: use memcpy). Blocks until done; if the DMA does not finish
// within timeout_ms the channel is retired (it may still write later) and every later call returns
// ESP_ERR_NOT_SUPPORTED at once, so callers fall back for good. Separate lock from the 2D engines.
esp_err_t nv_2d_copy(void *dst, const void *src, size_t n, uint32_t timeout_ms);
// The same copy without waiting: queued on the DMA (up to 4 in flight), `done` (a binary semaphore
// the caller owns) is given when it has landed. The caller must not touch src or dst until then —
// wait with nv_2d_copy_wait(done, timeout_ms), which retires the channel for good on a timeout like
// nv_2d_copy does. ESP_ERR_NOT_SUPPORTED / ESP_ERR_INVALID_ARG: copy it yourself (memcpy).
esp_err_t nv_2d_copy_start(void *dst, const void *src, size_t n, void *done);
esp_err_t nv_2d_copy_wait(void *done, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
