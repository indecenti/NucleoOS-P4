// vp_mp4 — ISO-BMFF (MP4) moov parsing for nv_vplayer's H.264 path, split out so it builds on the PC
// with no ESP-IDF: tests/host unit-tests and fuzzes it. The moov bytes are untrusted.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline uint32_t vp_be32(const uint8_t *p){ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }

typedef struct { uint32_t offset, size; } vp_sample_t;

typedef struct {
    bool     present;
    uint32_t nsamp;
    vp_sample_t *tbl;         // PSRAM, nsamp entries — flat, chunk-interleaving already resolved
    uint32_t period_ms;       // ms/sample (CFR assumption: first stts run's delta)
    uint32_t *sync;           // PSRAM 0-based keyframe sample indices from stss; NULL = every sample is one
    uint32_t sync_count;
    const uint8_t *sps; uint16_t sps_len;   // video: point INTO the moov buffer (kept alive for the clip)
    const uint8_t *pps; uint16_t pps_len;
    uint8_t  nal_len_size;                   // video: avcC lengthSizeMinusOne+1 (1/2/3/4); 0 = unset
    int rate, channels, bits;                // audio only
} vp_track_t;

// Fill V (handler 'vide') and/or A (handler 'soun', mp4a/AAC only) from a whole moov box (header
// included). Tables are allocated here; release them with vp_mp4_track_free.
void vp_mp4_parse_moov(const uint8_t *moov, uint32_t moov_sz, vp_track_t *V, vp_track_t *A);
// Nearest keyframe sample index <= the sample nearest pos_ms (CFR assumption via period_ms).
uint32_t vp_mp4_seek_video_index(const vp_track_t *V, int want_ms);
void vp_mp4_track_free(vp_track_t *T);

#ifdef __cplusplus
}
#endif
