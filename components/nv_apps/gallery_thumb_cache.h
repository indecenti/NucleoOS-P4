// gallery_thumb_cache — persistent SD-backed thumbnail cache for the Gallery app's grid.
// One small RGB565 ".bin" file (LVGL's raw image layout) per photo or Motion-JPEG video (its first
// frame), built once via gallery_jpeg_hw's HW decode + PPA downscale and handed back in PSRAM, so
// the grid draws thumbnails straight from RAM (no SD read per redraw, no decode, no scaling).
// Cache lives under a dot-prefixed directory so the gallery's own scan skips it.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Thumbnail geometry (16:9, the camera's aspect). The grid lays its tiles out around this size and
// draws the thumbnail 1:1: any draw-time scaling is an image transform the P4 software renderer
// draws slowly and as streaks.
#define GALLERY_THUMB_W 256
#define GALLERY_THUMB_H 144

// Get the thumbnail of `source_posix_path` (plain POSIX path, e.g. "/sdcard/DCIM/IMG_1.jpg"): a
// baseline JPEG, or with `video` the first frame of a Motion-JPEG AVI. Uses the SD cache when it
// is fresh, else builds and stores it. On success *px is a PSRAM buffer the caller owns (free with
// heap_caps_free, after dropping `dsc` from LVGL's image cache) and *dsc describes it. did_build
// (optional) reports whether a decode actually ran. Runs on a worker, never the LVGL thread.
bool gallery_thumb_get(const char *source_posix_path, bool video,
                       lv_image_dsc_t *dsc, uint8_t **px, bool *did_build);

// Remove the cached thumbnail for a source path, if any (best-effort, ignores a missing file).
void gallery_thumb_cache_evict(const char *source_posix_path);

#ifdef __cplusplus
}
#endif
