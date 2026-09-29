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

// PPA-downscale the current panel framebuffer to dw x dh raw RGB565 pixels into `dst`
// (64B-aligned, dw*dh*2 bytes, owned by the caller). For Recents card previews — the reader shows
// it as an LVGL RGB565 image with no decode. The caller supplies the buffer so it can come from a
// slab made once at boot: an allocation per app close landed right behind the closing app's big
// buffers and, once those were freed, split the free PSRAM the camera needs contiguous.
// Best-effort; false on any failure (dst content then undefined). LVGL-thread safe (PPA, ~ms).
bool nv_hal_thumbnail_grab(uint8_t *dst, int dw, int dh);

// Direct-to-panel video blit: PPA-scale an RGB565 frame (src, sw x sh visible pixels, rows
// `src_pitch` pixels apart — the HW JPEG decoder pads rows to whole MCUs) straight into the frame on
// screen at rect (dx,dy,dw,dh). Immediate, so it can tear (it writes while the panel scans): a
// producer that can hand out its latest picture on demand should use an nv_disp layer with
// nv_hal_video_draw() instead (tear-free, vsync-paced — the video player does). Bypasses LVGL's per-frame canvas compositing + partial-flush
// entirely — the whole point is smooth full-rate video without the software-render tax. Caller must
// keep the destination rect free of LVGL redraws (no overlay/invalidate over it) or they will fight
// for the pixels. The rect becomes nv_disp's direct region (carried across LVGL's buffer swaps)
// until nv_hal_video_blit_end(). PPA-blocking, ~1-3 ms. Returns false on failure or when a swap held
// the frame (skip that frame).
// `mode`: NV_HAL_BLIT_FIT (letterbox, aspect kept), _STRETCH (fill, aspect ignored), _ZOOM (fill,
// aspect kept, overflow cropped). The PPA scales in 1/16 steps rounded DOWN, so every mode picks an
// exact k/16 factor (plus at most a few % of edge crop) — a non-k/16 float left a stripe of the rect
// unwritten. The source must already be in memory (CPU-written frames: msync C2M before calling).
// `clear_bars`: black the part of the rect the picture doesn't cover (first frame / after a resize or
// mode change; skip afterwards to save the fill).
// _FIT_EXACT: like _FIT but never crops (game canvases: their edges carry HUD and touch targets).
enum { NV_HAL_BLIT_FIT = 0, NV_HAL_BLIT_STRETCH = 1, NV_HAL_BLIT_ZOOM = 2, NV_HAL_BLIT_FIT_EXACT = 3 };
bool nv_hal_video_blit(const void *src, int sw, int sh, int src_pitch, int dx, int dy, int dw, int dh,
                       int mode, bool clear_bars);
// The video stopped or LVGL UI now covers its rect: stop carrying the rect across swaps (else a
// drawer or the notification shade drawn over it would be overwritten with the old picture).
void nv_hal_video_blit_end(void);

// The geometry nv_hal_video_blit applies for the same arguments: the source block (bx,by,bw,bh) it
// scales by kx/16 × ky/16 into (ox,oy,tw,th) on the panel, inside the clamped rect (dx,dy,dw,dh).
// Lets a caller map panel coordinates back to source pixels: sx = bx + (px - ox) * 16 / kx.
// False when nothing would be drawn.
typedef struct { int kx, ky, bx, by, bw, bh, ox, oy, tw, th, dx, dy, dw, dh; } nv_hal_blit_geom_t;
bool nv_hal_video_geom(int sw, int sh, int dx, int dy, int dw, int dh, int mode, nv_hal_blit_geom_t *g);
// Same scaling as nv_hal_video_blit, into the given frame buffer (`stride` must be the panel width):
// for nv_disp layer draw callbacks, which receive the back buffer being composed.
bool nv_hal_video_draw(uint16_t *fb, int stride, const void *src, int sw, int sh, int src_pitch, int dx,
                       int dy, int dw, int dh, int mode, bool clear_bars);

#ifdef __cplusplus
}
#endif
