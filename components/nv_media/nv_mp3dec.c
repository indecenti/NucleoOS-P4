// nv_mp3dec — minimp3 implementation unit. Compiled -O2 (see CMakeLists): the rest of the
// component stays -Os, but decode speed is the whole point here.
//
// Symbol note: esp_audio_codec's prebuilt MP3 decoder is ALSO minimp3 (exports mp3dec_init /
// mp3dec_decode_frame) — rename ours to avoid the multiple-definition link error.
#define mp3dec_init         nv_minimp3_init
#define mp3dec_decode_frame nv_minimp3_decode_frame
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3          // no MP1/MP2 tables
#define MINIMP3_NO_SIMD           // RISC-V: no SSE/NEON paths
#include "minimp3.h"

#include "nv_mp3dec.h"
#include "esp_heap_caps.h"
#include <stdlib.h>

// The decoder's hot working set — mp3dec_t state (~6.6 KB), minimp3's ~16 KB scratch and the
// PCM output frame (4.6 KB) — is allocated per playback and freed when the track ends: it used
// to be ~27 KB of PERMANENT internal .bss for a feature active a few % of the uptime. Internal
// heap first (decode speed is why this wrapper exists), PSRAM as a fallback rather than no music.
static mp3dec_t *s_dec;
static int16_t  *s_pcm;
mp3dec_scratch_t *nv_minimp3_scratch_ptr;   // read by mp3dec_decode_frame (minimp3.h patch)

static void *hot_alloc(size_t n) {
    void *p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p;
}

bool nv_mp3dec_reset(void) {
    if (!s_dec)                  s_dec = (mp3dec_t *)hot_alloc(sizeof(mp3dec_t));
    if (!nv_minimp3_scratch_ptr) nv_minimp3_scratch_ptr = (mp3dec_scratch_t *)hot_alloc(sizeof(mp3dec_scratch_t));
    if (!s_pcm)                  s_pcm = (int16_t *)hot_alloc(NV_MP3_MAX_SAMPLES * sizeof(int16_t));
    if (!s_dec || !nv_minimp3_scratch_ptr || !s_pcm) { nv_mp3dec_release(); return false; }
    mp3dec_init(s_dec);
    return true;
}

int16_t *nv_mp3dec_pcm(void) { return s_pcm; }

void nv_mp3dec_release(void) {
    free(s_dec);                  s_dec = NULL;
    free(nv_minimp3_scratch_ptr); nv_minimp3_scratch_ptr = NULL;
    free(s_pcm);                  s_pcm = NULL;
}

int nv_mp3dec_frame(const uint8_t *in, int in_bytes, int16_t *pcm,
                    int *hz, int *ch, int *frame_bytes) {
    mp3dec_frame_info_t fi = {0};
    const int samples = s_dec ? mp3dec_decode_frame(s_dec, in, in_bytes, pcm, &fi) : 0;
    if (hz) *hz = fi.hz;
    if (ch) *ch = fi.channels;
    if (frame_bytes) *frame_bytes = fi.frame_bytes;
    return samples;
}
