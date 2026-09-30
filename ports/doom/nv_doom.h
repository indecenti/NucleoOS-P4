// nv_doom.h — glue shared by the NucleoOS Doom front-end files (nv_doom.c, nv_sound.c, opl_nv.c).
#pragma once
#include <stdint.h>

#define NV_DOOM_RATE 22050              // output sample rate (stereo s16)

// opl_nv.c: mix `frames` samples of OPL music into acc (stereo int32, interleaved).
void nv_opl_render(int32_t *acc, int frames, int volume_q8);

// nv_sound.c
void nv_snd_setup(void);                // before D_DoomMain: wires the music module
void nv_snd_pump(void);                 // keep the stream fed; call often (frame + sleeps)
void nv_snd_shutdown(void);

// nv_doom.c: the directory for config + saves ("/appdata/" or "/"), used by the m_config patch.
extern const char *nv_doom_datadir;

// g_game.c patch: analog sticks, added to the tic command built from keys/mouse.
void DG_AnalogTiccmd(int *forward, int *side, short *angleturn);
