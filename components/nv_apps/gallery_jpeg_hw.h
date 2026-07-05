// gallery_jpeg_hw — HW JPEG decode (driver/jpeg_decode.h) + PPA scale (driver/ppa.h) helpers for
// the Gallery app. No LVGL/gallery-model knowledge: given a baseline-JPEG file and a target box,
// produces RGB565 pixels. Mirrors the exact engine/PPA-client lifecycle already proven in
// components/nv_vplayer/nv_vplayer.c and components/nv_camera/nv_camera.c on this board.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// PPA output buffers (both the pointer AND the byte size) must be aligned to this many bytes on
// this chip (64B = the external-RAM cache line size the PPA driver validates against — see
// ppa_srm.c's "out.buffer addr or out.buffer_size not aligned to cache line size" check). Only
// the OUTPUT side of a PPA op has this requirement; the input side accepts any pointer/size.
// Allocate every destination buffer passed to the scale functions below like this:
//   size_t cap = gallery_ppa_align_size((size_t)w * h * 2);
//   uint8_t *buf = (uint8_t *)heap_caps_aligned_alloc(GALLERY_PPA_ALIGN, cap,
//                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#define GALLERY_PPA_ALIGN 64

size_t gallery_ppa_align_size(size_t raw_bytes);

// Decode a baseline JPEG file at full source resolution. On success, *out_buf is set to a
// driver-allocated RGB565 buffer (src_w*src_h*2 bytes, free with gallery_jpeg_hw_free) and
// *out_len to its byte length. On failure, *out_buf is set to NULL and *out_len to 0.
bool gallery_jpeg_hw_decode_file(const char *posix_path, int src_w, int src_h,
                                  uint8_t **out_buf, size_t *out_len);

// Free a buffer returned by gallery_jpeg_hw_decode_file. Safe to call with NULL.
void gallery_jpeg_hw_free(uint8_t *buf);

// PPA STRETCH-fill: scales RGB565 src (src_w x src_h, tightly packed) into dst, filling the
// ENTIRE dst_w x dst_h rectangle and ignoring aspect ratio. dst must satisfy GALLERY_PPA_ALIGN
// (see above); dst_cap must be >= gallery_ppa_align_size(dst_w*dst_h*2). Used for thumbnail
// generation, where the grid tile's own COVER crop happens later at draw time (matches today's
// visual result: full image stretched then cropped).
bool gallery_ppa_scale_stretch(const uint8_t *src, int src_w, int src_h,
                                uint8_t *dst, int dst_w, int dst_h, size_t dst_cap);

// PPA letterboxed/centered fit (CONTAIN): scales RGB565 src into dst preserving aspect ratio,
// centered; borders are left untouched (PPA only writes the scaled region) — caller must
// pre-clear dst to the desired border color before calling. Same dst alignment/cap rules as
// gallery_ppa_scale_stretch.
bool gallery_ppa_scale_fit(const uint8_t *src, int src_w, int src_h,
                            uint8_t *dst, int dst_w, int dst_h, size_t dst_cap);

// Release the lazily-created JPEG decoder engine + PPA client. Not currently called anywhere
// (this OS has no app-close/teardown hook — see NvApp in nv_app.h) but kept available in case
// one is added later. Safe to call even if never lazily created (no-op).
void gallery_jpeg_hw_release(void);

#ifdef __cplusplus
}
#endif
