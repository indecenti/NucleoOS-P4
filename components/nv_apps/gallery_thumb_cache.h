// gallery_thumb_cache — persistent SD-backed thumbnail cache for the Gallery app's grid.
// Generates a small RGB565 ".bin" file (LVGL's native raw-image format, loaded by its built-in
// bin decoder with zero per-tile decode cost) per source JPEG, via gallery_jpeg_hw's HW decode +
// PPA downscale. Cache lives under a dot-prefixed directory so gallery_app.cpp's own scan_dir()
// naturally skips it.
#pragma once
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Ensure a fresh cached thumbnail exists on SD for the baseline JPEG at source_posix_path (the
// plain POSIX path, e.g. "/sdcard/DCIM/IMG_0001.jpg" — NOT the "S:"-prefixed LVGL form). src_w/
// src_h are the already-known full-decode dimensions (from the header probe done at scan time).
// On success, writes the "S:"-prefixed thumbnail path (ready for lv_image_set_src) into
// out_thumb_path (capacity out_cap) and returns true. On ANY failure (stat, decode, PPA, write,
// low free space), leaves out_thumb_path as an empty string and returns false — the caller's
// existing SW-decode fallback (the original full-res path) is expected to run instead.
// out_did_build (optional, may be NULL) is set to true only when a decode+PPA+write actually ran
// (cache was missing/stale) vs. a cheap stat-only cache hit — lets a caller show a "generating
// thumbnails..." toast only when a real backlog was processed.
bool gallery_thumb_cache_get_or_build(const char *source_posix_path, int src_w, int src_h,
                                       char *out_thumb_path, size_t out_cap,
                                       bool *out_did_build);

// Cheap UI-thread check (NO decode/build): if a FRESH cached thumbnail already exists on SD,
// writes its "S:"-prefixed path into out_thumb_path and returns true; otherwise returns false and
// leaves out_thumb_path empty. Lets the grid show already-cached thumbs instantly and defer the
// misses to a background/incremental builder instead of decoding everything up front.
bool gallery_thumb_cache_lookup(const char *source_posix_path,
                                char *out_thumb_path, size_t out_cap);

// Remove the cached thumbnail for a source path, if any (best-effort, ignores a missing file).
void gallery_thumb_cache_evict(const char *source_posix_path);

#ifdef __cplusplus
}
#endif
