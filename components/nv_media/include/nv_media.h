// nv_media — audio playback engine for NucleoOS Anima (music player, later video audio track).
// Wraps the Espressif unified decoder (esp_audio_codec "simple decoder": WAV/MP3/AAC/FLAC/...)
// on a dedicated worker task that pulls file bytes, decodes to PCM, and pushes it to the ES8311
// DAC via nv_audio's PCM streaming path. The UI drives it with transport calls and polls state.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NV_MEDIA_STOPPED = 0,
    NV_MEDIA_PLAYING,
    NV_MEDIA_PAUSED,
    NV_MEDIA_ERROR,     // last play() failed to open/decode (transient; cleared on next play)
} nv_media_state_t;

// Start the worker task (idempotent). Safe before nv_audio is up.
void nv_media_init(void);

// Decode + play `path` (a full VFS path). Replaces any current track. Non-blocking: returns
// true if the file opened and playback started. Formats depend on the built decoder set.
bool nv_media_play(const char *path);

void nv_media_pause(bool on);   // pause/resume the current track
void nv_media_stop(void);       // stop and release the DAC

nv_media_state_t nv_media_state(void);

// Playback position / total duration in ms (dur 0 when the decoder can't report it).
int nv_media_pos_ms(void);
int nv_media_dur_ms(void);

// True when reaching end-of-track since the last poll (edge; self-clears). Lets the UI auto-advance.
bool nv_media_took_eot(void);

// Seek to pct_x10 (0..1000) of the track. Byte-proportional with decoder re-sync — only honored
// when nv_media_seekable() (MP3/AAC); other formats ignore it (the UI disables the slider).
bool nv_media_seekable(void);
void nv_media_seek(int pct_x10);

// Sample format of the playing track (0s when unknown/stopped). For the now-playing subtitle.
void nv_media_track_info(int *rate, int *ch, int *bits);

// Header-only duration probe (no decode): MP3 (Xing exact / CBR net of ID3), WAV, FLAC.
// Fast enough to run per file at library scan. Returns 0 when unknown.
int nv_media_probe_dur_ms(const char *path);

// Extension sniff: does this path look like a playable audio file?
bool nv_media_is_audio(const char *path);

#ifdef __cplusplus
}
#endif
