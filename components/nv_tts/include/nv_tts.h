// nv_tts — OS-wide offline concatenative voice (ported from G:\Nucleo nucleo_tts).
// Voice packs live on SD at /sdcard/data/tts/<lang>/{index.bin,clips.pcm} (NTI1, mono 16-bit @24kHz).
// Speaking is non-blocking (a worker task streams the clips to the codec via nv_audio's PCM path).
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Start the voice service (spawns the worker task) and set the default language ("it"/"en"/...).
// Returns true if a voice pack for that language is present on SD. Call once at boot.
bool nv_tts_init(const char *lang);

// True if the default-language voice pack is installed on SD.
bool nv_tts_available(void);

// Speak `text` (numbers are expanded, phrases/words matched, unknown words dropped as micro-pauses;
// too many unknowns -> stays silent). `lang` NULL/"" uses the default. Non-blocking. false if muted
// or no pack. Any native app can call this; WASM apps use the nv.speak import.
bool nv_tts_say(const char *text, const char *lang);

void nv_tts_set_lang(const char *lang);
void nv_tts_set_enabled(bool on);   // master mute (default on)
bool nv_tts_enabled(void);
void nv_tts_set_speed(int pct);     // reading speed % (70..160, 100 = natural)
int  nv_tts_speed(void);
void nv_tts_stop(void);

#ifdef __cplusplus
}
#endif
