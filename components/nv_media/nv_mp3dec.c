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

// Hot decoder state in internal .bss — the whole reason this wrapper exists. ~6.6 KB.
static mp3dec_t s_dec;

void nv_mp3dec_reset(void) { mp3dec_init(&s_dec); }

int nv_mp3dec_frame(const uint8_t *in, int in_bytes, int16_t *pcm,
                    int *hz, int *ch, int *frame_bytes) {
    mp3dec_frame_info_t fi;
    const int samples = mp3dec_decode_frame(&s_dec, in, in_bytes, pcm, &fi);
    if (hz) *hz = fi.hz;
    if (ch) *ch = fi.channels;
    if (frame_bytes) *frame_bytes = fi.frame_bytes;
    return samples;
}
