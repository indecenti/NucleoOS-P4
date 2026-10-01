// nv_wake — "hands-free ANIMA": an always-on wake word, then the spoken question.
//
// The microphone runs a low-power detector (ESP-SR WakeNet, fully offline). When it hears the wake
// word the device chimes, opens ANIMA and records the question until the speaker goes quiet; the
// recording then follows the usual voice path (home Whisper server first, cloud second).
//
// The detector is optional at build time (CONFIG_NV_WAKE_ESP_SR, see Kconfig): without it the API
// still works and nv_wake_status() says why it is unavailable, so every UI can explain it.
// Settings (NVS): "wake.on" (bool), "wake.word" (model name), "wake.sens" (0 low, 1 normal, 2 high).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NV_WAKE_MAX_WORDS 8

typedef enum {
    NV_WAKE_OFF = 0,        // disabled by the user
    NV_WAKE_UNAVAILABLE,    // no detector in this build / no model / no microphone (see reason)
    NV_WAKE_LISTENING,      // waiting for the wake word
    NV_WAKE_HEARD,          // just triggered: cooling down while the question is taken
    NV_WAKE_PAUSED,         // the microphone is busy (recording, playback) - resumes by itself
} nv_wake_state_t;

typedef struct {
    nv_wake_state_t state;
    bool available;                         // a detector + model + microphone exist
    bool enabled;                           // the user setting
    int  sensitivity;                       // 0..2
    char word[32];                          // active model name ("" = none)
    char label[32];                         // what to say, e.g. "Hi ESP"
    int  nwords;                            // installed wake words
    char words[NV_WAKE_MAX_WORDS][32];      // their model names
    char labels[NV_WAKE_MAX_WORDS][32];     // and what to say for each
    uint32_t triggers;                      // since boot
    int32_t  last_ago_s;                    // seconds since the last trigger (-1 = never)
    char reason[96];                        // why UNAVAILABLE / PAUSED (localized by the caller's lang)
} nv_wake_status_t;

// Boot: reads the settings and starts listening if enabled (needs the microphone up).
void nv_wake_init(void);
void nv_wake_set_enabled(bool on);
bool nv_wake_set_word(const char *model);    // false: not an installed wake word
void nv_wake_set_sensitivity(int level);     // 0..2 (clamped)
void nv_wake_status(nv_wake_status_t *out, bool en);
const char *nv_wake_state_name(nv_wake_state_t s);

// What happens on a trigger (runs on the LVGL thread via lv_async_call). Default: none.
void nv_wake_set_handler(void (*fn)(void));

// End-of-question detection on the live mic level (0..100, nv_audio_mic_level), called every
// `dt_ms` while the question is recorded. Returns NV_VAD_GO to keep recording, NV_VAD_DONE when
// the speaker stopped (or the time ran out), NV_VAD_NOTHING when nobody spoke at all.
typedef enum { NV_VAD_GO = 0, NV_VAD_DONE, NV_VAD_NOTHING } nv_vad_t;
typedef struct { int ms, speech_ms, quiet_ms, floor; bool spoke; } nv_vad_state_t;
void     nv_vad_reset(nv_vad_state_t *v);
nv_vad_t nv_vad_step(nv_vad_state_t *v, int level, int dt_ms);

#ifdef __cplusplus
}
#endif
