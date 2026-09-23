// gallery_jpeg_hw — HW JPEG decode (driver/jpeg_decode.h) + PPA scale (driver/ppa.h) helpers for
// the Gallery app. No LVGL/gallery-model knowledge: given a baseline JPEG (a file, or the first
// frame of a Motion-JPEG AVI) and a target box, produces RGB565 pixels. Mirrors the engine/PPA-
// client lifecycle proven in components/nv_vplayer/nv_vplayer.c and components/nv_camera.
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

// A decoded picture: RGB565, low byte first (what LVGL and the panel use), `w` x `h` visible
// pixels in rows `pitch` pixels apart — the decoder writes whole MCUs, so pitch is w rounded up
// to 16 (4:2:0 / 4:2:2) or 8 (4:4:4 / grey). Free with gallery_jpeg_hw_free.
typedef struct {
    uint8_t *px;
    size_t   len;
    int      w, h, pitch;
} gallery_raster_t;

// Decode a baseline JPEG file at full resolution. False (raster zeroed) on any failure,
// including pictures over 2048x2048 or files over 6 MB.
bool gallery_jpeg_hw_decode_file(const char *posix_path, gallery_raster_t *out);

// Decode the first video frame of a Motion-JPEG AVI (the camera's recordings): the poster used for
// the video's thumbnail and in the viewer. False for other codecs or a malformed file.
bool gallery_jpeg_hw_decode_avi_poster(const char *posix_path, gallery_raster_t *out);

void gallery_jpeg_hw_free(gallery_raster_t *r);

// PPA COVER-fill: scales src into dst, filling the ENTIRE dst_w x dst_h rectangle with a centre
// crop (aspect preserved; the PPA's 1/16 scale steps make an arbitrary stretch impossible without
// leaving part of dst unwritten). dst must satisfy GALLERY_PPA_ALIGN; dst_cap must be >=
// gallery_ppa_align_size(dst_w*dst_h*2). Used for thumbnails.
bool gallery_ppa_scale_cover(const gallery_raster_t *src, uint8_t *dst, int dst_w, int dst_h,
                             size_t dst_cap);

// PPA letterboxed/centered fit (CONTAIN): scales src into dst preserving aspect ratio, centered;
// borders are left untouched (PPA only writes the scaled region) — caller must pre-fill dst with
// the border colour. Same dst alignment/cap rules as gallery_ppa_scale_cover.
bool gallery_ppa_scale_fit(const gallery_raster_t *src, uint8_t *dst, int dst_w, int dst_h,
                           size_t dst_cap);

// Release the lazily-created JPEG decoder engine + PPA client. Safe to call even if never
// created (no-op).
void gallery_jpeg_hw_release(void);

#ifdef __cplusplus
}
#endif
