// nv_camera — 2MP MIPI-CSI camera (OV02C10) bring-up + live RGB565 frames.
// The sensor's SCCB control bus is the SAME internal I2C bus as the touch/RTC/codecs
// (GPIO7/8), so nv_camera reuses nv_hal_i2c_bus() instead of creating a second master.
// The MIPI PHY LDO is already powered by the display panel (nv_hal), so nv_camera does
// NOT re-acquire it. Pipeline: sensor RAW8 -> P4 ISP -> RGB565 (directly drawable).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Allocate nv_camera_render() destinations with this many bytes, 64-byte aligned: a whole number of
// cache lines, so the buffer can also be handed to the PPA / DMA (which refuse partial lines).
#define NV_CAMERA_RENDER_BYTES(w, h) ((((size_t)(w) * (size_t)(h) * 2u) + 127u) & ~(size_t)127u)

#ifdef __cplusplus
extern "C" {
#endif

// Preview sizes are the sensor frame times k/16, k = 1..16: 120x67, 240x135, ... 840x472,
// 960x540 ... for 1920x1080 — the only sizes nv_camera_render() (and the PPA elsewhere) scale to
// exactly. Returns false if k is out of range.
bool nv_camera_preview_size(int k, int *w, int *h);

// Bring up sensor + CSI + ISP and start streaming. Returns false (gracefully, no crash) if
// no sensor is attached/detected or a stage fails. Call from the LVGL/app thread.
bool nv_camera_start(void);

// Stop streaming and free everything. Safe to call even if not running.
void nv_camera_stop(void);

bool nv_camera_running(void);

// Total frames the driver has captured since start (ISR-counted). Rising = live capture works.
uint32_t nv_camera_frames(void);

// Native frame geometry (valid after a successful start).
void nv_camera_dims(int *w, int *h);

// Downscale the latest frame into `dst` (RGB565, dst_w x dst_h), on the CPU (the PPA costs ~106 ms
// per 1080p frame on this silicon; this takes a fraction). false if no frame yet or the size is not
// an exact k/16 of the sensor frame — use a size from nv_camera_preview_size(). `dst` should be
// 64-byte aligned and hold NV_CAMERA_RENDER_BYTES(dst_w, dst_h). Call from one thread (the LVGL/app
// thread); the frame being read is pinned so the camera DMA never overwrites it mid-read.
bool nv_camera_render(uint8_t *dst, int dst_w, int dst_h);

// ---- Auto exposure / white balance (software 3A) ----
// Feed every preview frame you just got from nv_camera_render() (RGB565, w x h). It meters the
// scene and steers sensor exposure/gain plus the ISP tone/white-balance curves, rate-limited
// internally. Without it the image stays at the sensor's fixed, dark, green-tinted defaults.
void nv_camera_auto_update(const uint8_t *rgb565, int w, int h);
// Exposure compensation in 1/2 EV steps, clamped to -4..+4 (-2..+2 EV).
void nv_camera_set_ev(int half_steps);
int  nv_camera_get_ev(void);
// Spot metering point in viewfinder permille (0..1000 each axis); negative = centre-weighted.
void nv_camera_set_meter_point(int x_permille, int y_permille);
// Exposure currently programmed: time in microseconds, total sensor gain x100 (100 = 1x).
void nv_camera_exposure_info(uint32_t *exp_us, uint32_t *gain_x100);

// Encode the latest full-resolution frame to a JPEG file (hardware encoder). false on failure.
bool nv_camera_save_jpeg(const char *path);

// --- Video recording (HW-encoded; .avi = Motion-JPEG; .mp4 = H.264, chip revision v3+ only) ---
// Start recording the live stream to `path` (e.g. "/sdcard/DCIM/VID_20260923_223901.avi"). Requires
// the camera to be running. Returns false if it can't start. Safe to call twice (no-op if recording).
bool nv_camera_video_start(const char *path);

// Encoded video frame size (1200x672: the 10/16 PPA step, height cut to whole 16-row macroblocks).
void nv_camera_video_dims(int *w, int *h);

// Stop + finalize the current recording (writes the index and back-patches the header). No-op if
// not recording. Called automatically by nv_camera_stop().
void nv_camera_video_stop(void);

// True while a recording is in progress.
bool nv_camera_video_recording(void);

// Elapsed seconds of the current recording (0 if not recording) — for a REC timer in the UI.
uint32_t nv_camera_video_secs(void);

#ifdef __cplusplus
}
#endif
