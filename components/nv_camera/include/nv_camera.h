// nv_camera — 2MP MIPI-CSI camera (OV5647/SC2336) bring-up + live RGB565 frames.
// The sensor's SCCB control bus is the SAME internal I2C bus as the touch/RTC/codecs
// (GPIO7/8), so nv_camera reuses nv_hal_i2c_bus() instead of creating a second master.
// The MIPI PHY LDO is already powered by the display panel (nv_hal), so nv_camera does
// NOT re-acquire it. Pipeline: sensor RAW8 -> P4 ISP -> RGB565 (directly drawable).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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

// PPA-downscale the latest frame into `dst` (RGB565, dst_w x dst_h). false if no frame yet.
// LVGL-thread safe (PPA blocking). Use this to refresh a preview canvas.
bool nv_camera_render(uint8_t *dst, int dst_w, int dst_h);

// Encode the latest full-resolution frame to a JPEG file (hardware encoder). false on failure.
bool nv_camera_save_jpeg(const char *path);

// --- Video recording (Motion-JPEG in an AVI container, 1280x720, HW-encoded) ---
// Start recording the live stream to `path` (e.g. "/sdcard/DCIM/vid-123.avi"). Requires the
// camera to be running. Returns false if it can't start. Safe to call twice (no-op if recording).
bool nv_camera_video_start(const char *path);

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
