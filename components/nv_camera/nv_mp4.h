// nv_mp4 — minimal MP4 (ISO-BMFF) muxer for a single H.264/AVC video track. Streams samples into
// one mdat, then writes moov at close. Frames are fed as Annex-B (start-code) byte streams exactly
// as the ESP32-P4 hardware H.264 encoder emits them; SPS/PPS are stripped into avcC, slice NALs are
// rewritten to length-prefixed AVCC in mdat. All samples go in one chunk (trivial stsc/stco).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nv_mp4 nv_mp4_t;

// Create an .mp4 at `path` for a width x height @ fps AVC track. NULL on failure.
nv_mp4_t *nv_mp4_open(const char *path, int width, int height, int fps);

// Append one encoded frame (Annex-B). `keyframe` = the frame is an IDR. Returns false on write error.
bool nv_mp4_write(nv_mp4_t *m, const uint8_t *annexb, size_t len, bool keyframe);

// Write moov, close the file, free `m`. Returns total file bytes (0 on error). Safe on NULL.
long nv_mp4_close(nv_mp4_t *m);

#ifdef __cplusplus
}
#endif
