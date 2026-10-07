// nv_wasm_snd — the game sound mixer (ABI v15): polyphonic WAV voices for WASM games, mixed in their
// own task apart from the game loop (see nv_wasm_snd.cpp). Paths are full SD paths, resolved by the
// caller (nv_wasm) from the app's own snd/ folder.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NV_WSND_VOICES 12
#define NV_WSND_LOOP   1   // play again from the start at the end
#define NV_WSND_STREAM 2   // read from the SD while playing (music), instead of loading it whole

int  nv_wsnd_preload(const char *path);                       // decode into the cache now: 0 / -1
// Start a voice: vol 0..512 (256 = as recorded), pitch 32..1024 (256 = as recorded). Returns a
// handle >= 0, or -1 (no such WAV, out of memory, all voices busy with loops/streams).
int  nv_wsnd_play(const char *path, int vol, int pitch, int flags);
void nv_wsnd_set(int handle, int vol, int pitch);              // vol < 0 / pitch <= 0: unchanged
void nv_wsnd_stop(int handle, int fade_ms);                    // handle -1: every voice
void nv_wsnd_master(int vol);                                  // 0..512
void nv_wsnd_pause(bool on);                                   // release the DAC (another stream)
bool nv_wsnd_active(void);
void nv_wsnd_shutdown(void);                                   // app teardown: everything freed

#ifdef __cplusplus
}
#endif
