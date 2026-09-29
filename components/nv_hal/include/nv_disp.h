// nv_disp — the display compositor: tear-free output on the MIPI-DSI panel.
//
// The DPI controller scans a frame buffer out of PSRAM continuously at ~60 Hz. Writing into the
// buffer being scanned shows half an old and half a new frame (tearing). nv_disp owns TWO frame
// buffers: one on screen ("front"), one being composed ("back"). A finished frame becomes the front
// by a buffer switch that the DPI driver applies at the next frame boundary (vsync), and nothing is
// written into a buffer while it is scanned.
//
//   * Landscape (rotation 0): LVGL renders in DIRECT mode straight into the back buffer — no copy at
//     all. LVGL itself carries the previous frame's changes into the new back buffer.
//   * Rotated (90/180/270): LVGL renders in PARTIAL mode; the PPA rotates each rendered strip
//     directly into the back buffer, and nv_disp carries the previous frame's changes (minus what
//     the new frame redraws anyway).
//
// Other code that touches the panel pixels goes through the front-buffer API below instead of
// esp_lcd_dpi_panel_get_frame_buffer(): readers (screenshot, Recents thumbnail) and direct writers
// that bypass LVGL (video player, second screen). Holding the front buffer only blocks the next
// swap; LVGL keeps rendering the next frame meanwhile.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_lcd_types.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Create the LVGL display on `panel` (a DPI panel created with num_fbs = 2). Call once, after
// lvgl_port_init(), from any task; it takes the LVGL port lock itself. NULL on failure.
lv_display_t *nv_disp_create(esp_lcd_panel_handle_t panel, int hres, int vres);

typedef struct {
    uint16_t *px;       // RGB565 pixels of the buffer on screen (or queued to be on screen next)
    int       w, h;     // physical panel size
    int       stride;   // pixels per row
} nv_disp_surface_t;

// Lock the front buffer for reading or direct writing (blocks the next swap, never LVGL rendering).
// Pixels written by the CPU must be written back (esp_cache_msync C2M) before nv_disp_front_end();
// DMA/PPA writes need nothing. False when the lock was not free within timeout_ms (skip the frame).
bool nv_disp_front_begin(nv_disp_surface_t *out, uint32_t timeout_ms);
void nv_disp_front_end(void);

// A rectangle owned by a direct writer (the video picture): LVGL does not paint it, so every swap
// carries its pixels from the old front to the new one, keeping the latest frame on screen.
// w <= 0 or h <= 0 clears it (call when the writer stops or is covered by LVGL UI).
void nv_disp_set_direct_region(int x, int y, int w, int h);

typedef struct {
    bool     rotated;          // PARTIAL + PPA rotation (false: DIRECT, zero-copy)
    int      rotation;         // lv_display_rotation_t
    uint32_t swaps;            // frames presented
    uint32_t vsyncs;           // panel refreshes counted by the vsync interrupt
    uint32_t vsync_timeouts;   // a requested swap not confirmed within 100 ms (stalled panel)
    uint32_t violations;       // LVGL handed us the front buffer to present (must stay 0)
    uint32_t wait_us_avg;      // LVGL time blocked on vsync before reusing the other buffer
    uint32_t wait_us_max;
    uint32_t present_us_avg;   // cost of presenting a frame (cache write-back + region carry)
    uint32_t sync_us_avg;      // rotated mode: carrying the previous frame into the back buffer
    uint32_t render_us_avg;    // LVGL rendering time of a frame (render start -> present)
    uint32_t frame_us_avg;     // interval between consecutive presented frames while animating
} nv_disp_stats_t;

void nv_disp_get_stats(nv_disp_stats_t *out);

#ifdef __cplusplus
}
#endif
