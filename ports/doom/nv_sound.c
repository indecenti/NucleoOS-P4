// nv_sound.c — Doom sound for NucleoOS: 8-voice SFX mixer + OPL music on one PCM stream (ABI 10).
//
// Doom's sound effects are DMX lumps (8-bit unsigned mono, 11025 Hz mostly) played on up to
// snd_channels voices with a volume and a stereo separation; the music is MIDI/MUS played through
// Chocolate Doom's OPL player on the emulated OPL2 (opl_nv.c). Both are mixed here in int32 and
// pushed to the OS stream as 22050 Hz stereo s16. nv_snd_pump() tops the stream up to ~70 ms each
// time it runs (every rendered frame and every sleep while Doom waits for the next tic), so the
// sound keeps going as long as the game loop does.
#include <stdlib.h>
#include <string.h>

#include "doomtype.h"
#include "i_sound.h"
#include "m_misc.h"
#include "w_wad.h"
#include "z_zone.h"

#include "nucleo_sdk.h"
#include "nv_doom.h"

#ifdef NV_SIM
#include <time.h>
void nv_sim_mix_time(double s);
#endif

// i_sound.c binds these config variables for its libsamplerate option (unused here)
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

enum { NCH = 16, CHUNK = 256 };
#define TARGET_BYTES (NV_DOOM_RATE * 4 * 70 / 1000)   // queued audio we aim for, ~70 ms

typedef struct {
    const uint8_t *data;     // 8-bit unsigned samples
    uint32_t len;            // samples
    uint32_t pos;            // 16.16
    uint32_t step;           // 16.16 source samples per output sample
    int gl, gr;              // gains 0..256
    int on;
} voice_t;

static voice_t s_voice[NCH];
static boolean s_prefix;
static int s_audio;          // stream open
static int32_t s_retry_ms;
static int32_t s_acc[CHUNK * 2];
static int16_t s_out[CHUNK * 2];

music_module_t DG_music_module;

// ---- stream -------------------------------------------------------------------------------------
static void stream_open(void) {
    s_audio = nv_audio_open(NV_DOOM_RATE, 2) == 1;
    s_retry_ms = nv_millis();
    if (!s_audio) nv_log(NV_LOG_WARN, "doom: speaker busy, playing muted");
}

static void mix_chunk(void) {
    memset(s_acc, 0, sizeof s_acc);
    nv_opl_render(s_acc, CHUNK, 256);
    for (int c = 0; c < NCH; c++) {
        voice_t *v = &s_voice[c];
        if (!v->on) continue;
        int32_t *a = s_acc;
        for (int i = 0; i < CHUNK; i++, a += 2) {
            const uint32_t idx = v->pos >> 16;
            if (idx + 1 >= v->len) { v->on = 0; break; }
            // linear interpolation between the two nearest source samples
            const int s0 = (int)v->data[idx] - 128, s1 = (int)v->data[idx + 1] - 128;
            const int f = (int)(v->pos & 0xffff) >> 8;
            const int s = (s0 * 256 + (s1 - s0) * f);       // 8.8 -> ~16-bit
            a[0] += s * v->gl >> 8;
            a[1] += s * v->gr >> 8;
            v->pos += v->step;
        }
    }
    for (int i = 0; i < CHUNK * 2; i++) {
        int32_t x = s_acc[i] * 3 >> 2;                       // headroom (USB speakers brown out)
        s_out[i] = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
    }
}

void nv_snd_pump(void) {
    if (!s_audio) {
        if (nv_millis() - s_retry_ms > 3000) stream_open();   // e.g. the Music app let go
        if (!s_audio) return;
    }
    int bl = nv_audio_backlog();
    for (int guard = 0; bl >= 0 && bl < TARGET_BYTES && guard < 16; guard++) {
#ifdef NV_SIM
        { struct timespec t0, t1; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t0); mix_chunk();
          clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t1);
          nv_sim_mix_time((t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9); }
#else
        mix_chunk();
#endif
        if (nv_audio_write(s_out, (int32_t)sizeof s_out) < 0) { s_audio = 0; return; }
        bl += (int)sizeof s_out;
    }
    if (bl < 0) s_audio = 0;
}

void nv_snd_shutdown(void) {
    if (s_audio) nv_audio_close();
    s_audio = 0;
}

void nv_snd_setup(void) {
    memcpy(&DG_music_module, &music_opl_module, sizeof DG_music_module);
    DG_music_module.Poll = NULL;
}

// ---- sound module -------------------------------------------------------------------------------
static snddevice_t s_devices[] = {SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS,
                                  SNDDEVICE_WAVEBLASTER, SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32};

static boolean snd_init(boolean use_sfx_prefix) {
    s_prefix = use_sfx_prefix;
    memset(s_voice, 0, sizeof s_voice);
    stream_open();
    return true;                         // muted when busy: the game must still run
}

static void snd_shutdown(void) { nv_snd_shutdown(); }

static int snd_lump(sfxinfo_t *sfx) {
    char name[9];
    if (sfx->link) sfx = sfx->link;
    if (s_prefix) M_snprintf(name, sizeof name, "ds%s", sfx->name);
    else M_StringCopy(name, sfx->name, sizeof name);
    return W_CheckNumForName(name);
}

static void snd_update(void) { nv_snd_pump(); }

static void gains(voice_t *v, int vol, int sep) {
    // chocolate i_sdlsound: left = (254 - sep) * vol / 127, right = sep * vol / 127 (0..254)
    if (vol < 0) vol = 0;
    if (vol > 127) vol = 127;
    if (sep < 0) sep = 0;
    if (sep > 254) sep = 254;
    v->gl = (254 - sep) * vol / 127;
    v->gr = sep * vol / 127;
}

static void snd_params(int ch, int vol, int sep) {
    if (ch >= 0 && ch < NCH) gains(&s_voice[ch], vol, sep);
}

static int snd_start(sfxinfo_t *sfx, int ch, int vol, int sep) {
    if (ch < 0 || ch >= NCH) return -1;
    s_voice[ch].on = 0;
    const int lump = sfx->lumpnum >= 0 ? sfx->lumpnum : snd_lump(sfx);
    if (lump < 0) return -1;
    const int size = W_LumpLength(lump);
    if (size < 8 + 32 + 2) return -1;
    // DMX: u16 format (3), u16 rate, u32 length, then 16 pad bytes, samples, 16 pad bytes
    const uint8_t *d = sfx->driver_data;
    if (!d) {
        d = W_CacheLumpNum(lump, PU_STATIC);                 // kept, like vanilla's PU_MUSIC
        sfx->driver_data = (void *)d;
    }
    const unsigned fmt = d[0] | d[1] << 8, rate = d[2] | d[3] << 8;
    uint32_t len = d[4] | d[5] << 8 | (uint32_t)d[6] << 16 | (uint32_t)d[7] << 24;
    if (fmt != 3 || rate < 4000 || len <= 32) return -1;
    if (len > (uint32_t)size - 8) len = (uint32_t)size - 8;
    voice_t *v = &s_voice[ch];
    v->data = d + 8 + 16;
    v->len = len - 32;
    v->pos = 0;
    v->step = (uint32_t)(((uint64_t)rate << 16) / NV_DOOM_RATE);
    gains(v, vol, sep);
    v->on = 1;
    return ch;
}

static void snd_stop(int ch) {
    if (ch >= 0 && ch < NCH) s_voice[ch].on = 0;
}

static boolean snd_playing(int ch) { return ch >= 0 && ch < NCH && s_voice[ch].on; }

static void snd_cache(sfxinfo_t *sounds, int n) {
    for (int i = 0; i < n; i++) sounds[i].lumpnum = snd_lump(&sounds[i]);
}

sound_module_t DG_sound_module = {
    s_devices, sizeof s_devices / sizeof s_devices[0],
    snd_init, snd_shutdown, snd_lump, snd_update, snd_params,
    snd_start, snd_stop, snd_playing, snd_cache,
};
