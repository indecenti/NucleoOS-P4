// nv_audio — audio output service for NucleoOS Anima.
//
// Brings up the on-board ES8311 DAC over I2S (MCLK13/BCLK12/WS10/DOUT9) with the power amp on
// GPIO11, controlled over the shared I2C bus. Playback runs on a dedicated worker task fed by a
// small queue, so UI-thread callers (key clicks, system chimes) never block on the I2S write.
//
// OUTPUT needs the external speaker on the board's 2P JST (the on-board codec drives it); with
// no speaker plugged the driver still runs, just silently. The ES7210 mic (DIN48) is wired for a
// later capture/STT path. Honors the persisted "volume" and "mute" preferences.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bring up I2S + ES8311 + PA and start the playback task. Non-fatal: logs and no-ops on failure
// (e.g. codec absent). Call after nv_hal_init() (needs the shared I2C bus). Applies the saved
// volume/mute.
void nv_audio_init(void);
bool nv_audio_ready(void);

// Live volume 0..100 and mute. The caller persists the config; these apply immediately.
void nv_audio_set_volume(int percent);
void nv_audio_set_mute(bool on);

// Queue a generated tone (sine). Non-blocking; dropped when muted or not ready.
void nv_audio_tone(int freq_hz, int ms);

// Keyboard-click gate ("keyclick" pref; default on). The caller persists the config;
// this applies immediately — nv_audio_click() becomes a no-op when off.
void nv_audio_set_key_click(bool on);

// System sounds (thin wrappers over tones for now).
void nv_audio_click(void);   // short soft tick (keyboard / taps); gated by key-click pref
void nv_audio_chime(void);   // startup / positive confirmation
void nv_audio_alert(void);   // error / warning

// ---- PCM streaming (music / video players) -----------------------------------------------------
// Reconfigures the ES8311 DAC to (sample_rate, channels, bits) and opens a streaming session,
// then accepts raw interleaved PCM via _write until _end. The single shared DAC is serialized
// with the tone path (system tones/clicks are silently skipped while a stream is open).
//
// Threading: _write BLOCKS for real-time pacing (the codec absorbs one buffer at the wire rate),
// so call the trio from a dedicated worker task, NEVER the LVGL/UI thread. _begin/_end are cheap.
// bits = 16 or 32; channels = 1 or 2; rate 8000..96000 (clamped). Mute keeps timing (writes
// silence at volume 0) so the track still advances. Returns false/-1 when audio is unavailable.
bool nv_audio_pcm_begin(int sample_rate, int channels, int bits);
int  nv_audio_pcm_write(const void *pcm, size_t bytes);   // bytes consumed, or <0 on error
void nv_audio_pcm_end(void);                              // drains the ring, restores tone format
// Writes land in a ~3 s PSRAM playout ring drained by a feeder task, so _write only blocks
// when the ring is full (that block IS the decoder's pacing) and output never underruns.
// _end plays the buffered tail out; call _flush first for an INSTANT cut (stop/seek/switch).
void nv_audio_pcm_flush(void);
void nv_audio_pcm_pause(bool on);    // freeze/unfreeze the RING too (true audible pause)
size_t nv_audio_pcm_backlog(void);   // buffered-not-yet-played bytes (playhead correction)

// ---- stream ownership & voice priority ---------------------------------------------------------
// The single DAC carries three kinds of PCM stream. Voice OWNS the channel: when it wants to
// speak it preempts sound effects and silences tones so a spoken word is never smothered by a
// game jingle. Music is protected (never auto-cut). The plain nv_audio_pcm_begin() above is
// music/default. Sound effects and the voice engine use the tagged variant below.
typedef enum { NV_PCM_MUSIC = 0, NV_PCM_SFX = 1, NV_PCM_VOICE = 2 } nv_pcm_owner_t;
bool nv_audio_pcm_begin_as(int sample_rate, int channels, int bits, nv_pcm_owner_t owner);
// Raise/lower voice priority. While raised: any playing SFX stream is cancelled (its _write
// returns <0 so its worker bails and releases the channel), pending tones are flushed, and new
// tones are suppressed — so the voice acquires the DAC immediately and plays clean. Reference
// counted; balance every true with a false. nv_tts brackets each utterance with this.
void nv_audio_voice_priority(bool on);

// ---- microphone (on-board MIC1 via ES7210 ADC, I2S duplex on the same port) --------------------
// Capture runs on its own worker task, started on demand. Two consumers:
//   * live level meter (Settings → Sound): meter_start/stop + poll nv_audio_mic_level()
//   * mic test: record N ms to PSRAM, then play it back through the speaker
// All calls are non-blocking and safe from the LVGL thread (poll state from an LVGL timer).

typedef enum {
    NV_MIC_IDLE = 0,   // no capture running
    NV_MIC_METER,      // live level metering
    NV_MIC_REC,        // test: recording
    NV_MIC_PLAY,       // test: playing the recording back
} nv_mic_state_t;

// True when the ES7210 came up at init (capture available).
bool nv_audio_mic_ready(void);

// Live level meter. start returns false when the mic is unavailable or a test is running.
bool nv_audio_mic_meter_start(void);
void nv_audio_mic_meter_stop(void);

// Rolling input level 0..100 (valid while METER or REC; 0 otherwise).
int nv_audio_mic_level(void);

// Record `ms` (clamped to 5000) then play it back. Returns false if mic/speaker unavailable
// or already busy. Poll nv_audio_mic_state() until it returns to IDLE (or METER).
bool nv_audio_mic_test_start(int ms);

nv_mic_state_t nv_audio_mic_state(void);

// ---- voice recorder: stream the mic to a WAV file on SD (mono 16-bit @ 48 kHz) ---------------
// Arbitrary length (limited by SD space). While recording, poll nv_audio_mic_level() for the live
// meter and nv_audio_rec_secs() for elapsed time. The written .wav plays back via nv_media / the
// Music player. Non-blocking; capture runs on the mic worker task.
bool     nv_audio_rec_start(const char *path);  // false if mic unavailable or already busy
void     nv_audio_rec_stop(void);               // finalizes the WAV header and closes the file
bool     nv_audio_rec_active(void);
uint32_t nv_audio_rec_secs(void);

#ifdef __cplusplus
}
#endif
