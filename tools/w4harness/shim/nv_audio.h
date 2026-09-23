#pragma once
#include <stddef.h>
typedef enum { NV_PCM_MUSIC = 0, NV_PCM_SFX = 1, NV_PCM_VOICE = 2 } nv_pcm_owner_t;
static inline bool nv_audio_pcm_begin_as(int, int, int, nv_pcm_owner_t) { return false; }
static inline int nv_audio_pcm_write(const void *, size_t n) { return (int)n; }
static inline size_t nv_audio_pcm_backlog(void) { return 0; }
static inline void nv_audio_pcm_flush(void) {}
static inline void nv_audio_pcm_end(void) {}
