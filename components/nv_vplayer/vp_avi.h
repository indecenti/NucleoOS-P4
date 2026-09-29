// vp_avi — AVI (RIFF, MJPEG) header + index parsing for nv_vplayer, split out so it builds on the PC
// with no ESP-IDF: tests/host unit-tests and fuzzes it. Every byte read here is untrusted (files reach
// the SD over the LAN with no auth).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VP_SECT    512
// SDMMC reads straight into the caller's PSRAM buffer (multi-block DMA) only when the pointer AND
// length are cache-line aligned and the file position is sector aligned; anything else falls back
// to one 512-byte sector per command through a bounce buffer (sdmmc_cmd.c) — ~10x slower. So every
// bulk read is issued on sector boundaries into a 128-byte-aligned slot, and the payload is used in
// place at slot + (offset & 511) (the JPEG decoder has no input alignment rule).
#define VP_ALIGN   128

// RIFF fourcc as the little-endian word read from the file (VP_FCC('M','J','P','G')).
#define VP_FCC(a,b,c,d) ((uint32_t)(a)|((uint32_t)(b)<<8)|((uint32_t)(c)<<16)|((uint32_t)(d)<<24))

typedef struct {
    long     movi_pos;            // first byte after the 'movi' fourcc
    long     movi_end;            // end of the movi payload (EOF for an unfinished recording)
    long     fsz;
    uint32_t uspf, total, vw, vh; // avih
    uint32_t v_scale, v_rate;     // video strh time base: fps = rate / scale
    uint32_t v_fcc;               // video compression fourcc (strh handler or BITMAPINFOHEADER)
    int      v_stream, a_stream;  // stream numbers (the "00" in "00dc"); -1 = none
    uint16_t a_tag, a_ch, a_bits, a_align;
    uint32_t a_rate, a_bps;       // audio sample rate, bytes/second
    int      nstreams;
    long     v_indx, a_indx;      // OpenDML super index payload offsets ('indx' in the strl), 0 = none
    uint32_t v_indx_sz, a_indx_sz;
} vp_avi_t;

typedef struct { uint32_t off, size; } vp_ent_t;   // absolute payload offset + payload size

// Positioned read straight through the file descriptor (NOT stdio: on an unbuffered FILE every fread
// first walks and locks every open stream — ~600 ms per 44 KB frame against 3.5 ms through read()).
size_t vp_rd_at(FILE *f, long off, void *dst, size_t n);
long   vp_file_size(FILE *f);
// Read [off, off+len) as whole sectors into the aligned `buf` (cap bytes); returns the payload
// pointer inside it, NULL on a short read / oversize.
const uint8_t *vp_read_span(FILE *f, uint8_t *buf, size_t cap, uint32_t off, uint32_t len);

// RIFF/AVI header: stream roles, geometry, time base, the movi span. false = not a playable AVI.
bool vp_avi_probe(FILE *f, vp_avi_t *A);
// Index tables (allocated here, released with vp_avi_free). OpenDML super index first, then idx1,
// then a scan of the movi chunks; `keep_going` is polled during the scan (NULL = never abort).
bool vp_avi_index_odml(FILE *f, const vp_avi_t *A, long indx, uint32_t indx_sz, vp_ent_t **out, uint32_t *out_n);
bool vp_avi_index_idx1(FILE *f, const vp_avi_t *A, vp_ent_t **V, uint32_t *vn, vp_ent_t **Au, uint32_t *an);
bool vp_avi_index_scan(FILE *f, const vp_avi_t *A, vp_ent_t **V, uint32_t *vn, bool (*keep_going)(void));
void vp_avi_free(void *p);

#ifdef __cplusplus
}
#endif
