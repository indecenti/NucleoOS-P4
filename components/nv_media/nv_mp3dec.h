// nv_mp3dec — thin wrapper around minimp3 (public-domain, vendored) for the music engine.
// Why not esp_audio_codec's MP3? Its prebuilt decoder measured 145-155 KB/s on this P4 —
// BELOW the 188 KB/s a 48 kHz stereo track needs (telemetry: "pipe: dec=5000ms/5000ms",
// core-pinned and alone on core 1). minimp3 keeps its ~6.6 KB hot state in internal .bss
// (never PSRAM), is compiled -O2 in its own translation unit, and resyncs across garbage
// frames natively.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Max samples one call can produce (MINIMP3_MAX_SAMPLES_PER_FRAME): 1152 frames x 2 ch.
#define NV_MP3_MAX_SAMPLES (1152 * 2)

// Reset the decoder state (track start / after a seek — clears the bit reservoir).
void nv_mp3dec_reset(void);

// Decode ONE frame from `in`. Returns samples PER CHANNEL (0 = no output: either garbage
// skipped or more input needed). *frame_bytes = input consumed (0 = feed more data).
int nv_mp3dec_frame(const uint8_t *in, int in_bytes, int16_t *pcm,
                    int *hz, int *ch, int *frame_bytes);

#ifdef __cplusplus
}
#endif
