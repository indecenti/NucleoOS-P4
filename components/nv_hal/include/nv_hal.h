// nv_hal — NucleoOS Anima hardware bring-up (Phase 0.2: display + touch + backlight).
// Wraps the JD9165 MIPI-DSI panel, GT911 touch, and LEDC backlight behind one init,
// and hands a ready LVGL display to the UI layer.
#pragma once
#include <stdbool.h>
#include "lvgl.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bring up backlight + display + touch + LVGL. Returns true on success.
bool nv_hal_init(void);

// The LVGL display created during init (NULL if not up).
lv_display_t *nv_hal_display(void);

// Raw JD9165 panel handle (esp_lcd_panel_handle_t; NULL if not up). For direct-draw
// paths (Second Screen streaming) that bypass LVGL while lvgl_port is stopped.
void *nv_hal_panel(void);

// Raw GT911 handle (esp_lcd_touch_handle_t; NULL without touch). Only read it while
// the LVGL indev timer is stopped (lvgl_port_stop) — the driver is not re-entrant.
void *nv_hal_touch(void);

// Multi-touch: the GT911 reports up to 5 simultaneous fingers. LVGL's pointer indev only ever
// consumes finger 0 (one cursor — correct for widget interaction), but the full set is cached by
// the 60 Hz poll task and exposed here so any component that wants gestures (pinch-zoom, a
// multi-key instrument, etc.) can read every active point. Coordinates are PANEL space
// (0..1023 x 0..599, rotation-independent raw). Fills up to `max` points into xs/ys (either may be
// NULL) and returns the number of active fingers (0 when nothing is touched). Lock-free snapshot —
// safe to call from any task.
#define NV_TOUCH_MAX 5
int nv_hal_touch_points(int16_t *xs, int16_t *ys, int max);

// Backlight 0..100 (%).
void nv_hal_backlight_set(int percent);

// The shared internal I2C master bus (SDA7/SCL8), created during nv_hal_init. NULL until then.
// Other on-bus peripherals (RX8130 RTC @ 0x14, codecs) attach their own device to this bus.
i2c_master_bus_handle_t nv_hal_i2c_bus(void);

// On-die temperature in °C (P4 internal sensor; lazy install on first call).
// False when the sensor is unavailable. UI-thread use only (no locking inside).
bool nv_hal_temp_read(float *out_c);

// Capture the current panel framebuffer to a JPEG file (P4 hardware JPEG encoder).
// `path` is a full VFS path (e.g. "/sdcard/Screenshots/shot.jpg"); parent dir must exist.
// Always the physical 1024x600 landscape image regardless of UI rotation. Returns true on
// success. Not hot-path (allocates ~2.4 MB PSRAM scratch, freed before return).
bool nv_hal_screenshot(const char *path);

// PPA-downscale the current panel framebuffer to dw x dh and write it as raw RGB565 (no
// header) to `path`. For Recents card previews — the reader shows it as an LVGL RGB565 image
// with no decode. Best-effort; false on any failure. LVGL-thread safe (PPA blocking, ~ms).
bool nv_hal_thumbnail(const char *path, int dw, int dh);

// Direct-to-panel video blit: PPA-scale an RGB565 frame (src, sw x sh) straight into the live DSI
// framebuffer at rect (dx,dy,dw,dh), letterboxed (aspect preserved, centered). Bypasses LVGL's
// per-frame canvas compositing + partial-flush entirely — the whole point is smooth full-rate video
// without the software-render tax. Caller must keep the destination rect free of LVGL redraws (no
// overlay/invalidate over it) or they will fight for the pixels. PPA-blocking, ~1 ms. Returns false
// on failure. `clear_bars`: when the letterbox leaves margins, black them (needed the FIRST frame /
// after a resize; skip afterwards to save a fill).
bool nv_hal_video_blit(const void *src, int sw, int sh, int dx, int dy, int dw, int dh, bool clear_bars);

#ifdef __cplusplus
}
#endif
