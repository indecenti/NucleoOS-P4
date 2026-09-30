// audio.c — Vertice GP: the race sound, mixed in the game. The OS plays one stream at a time (a WAV
// would block every other sound for its whole length), so during a race the game opens its own raw
// stream (ABI v10) and mixes everything itself at 22 kHz: the player's two-stroke engine (pitch from
// the revs, rasp from the throttle), the nearest rival's engine, tyre squeal while drifting, wind at
// speed, one-shot effects (coins, boost, bumps, beeps, jingles) and a chiptune soundtrack per circuit
// (bass, arpeggio, lead and noise drums from short hand-written loops). The menus close the stream
// and use the ACE-Step WAVs through nv_sound instead.
#include "game.h"

#define RATE     22050
#define BLOCK    256                       // samples per mix block
#define TARGET   (RATE / 8 * 2)            // keep ~125 ms queued (bytes)

static int s_open;
static int16_t s_blk[BLOCK];
static uint32_t s_noise = 0x1234567u;
static inline float noise(void) {          // white noise -1..1
    s_noise ^= s_noise << 13; s_noise ^= s_noise >> 17; s_noise ^= s_noise << 5;
    return (int32_t)s_noise * (1.0f / 2147483648.0f);
}
// Phase increment for a frequency (32-bit phase accumulator = one cycle).
static inline uint32_t inc_of(float hz) { return (uint32_t)(hz * (4294967296.0f / RATE)); }
static inline float pulse(uint32_t ph, uint32_t duty) { return ph < duty ? 1.0f : -1.0f; }
static inline float saw(uint32_t ph) { return (int32_t)ph * (1.0f / 2147483648.0f); }
static inline float tri(uint32_t ph) {
    const float s = saw(ph);
    return (s < 0 ? -s : s) * 2.0f - 1.0f;
}
static float midi_hz(int m) {              // equal temperament from A4 = 440 Hz, no libm
    static const float semi[12] = { 1.0f, 1.059463f, 1.122462f, 1.189207f, 1.259921f, 1.334840f,
                                    1.414214f, 1.498307f, 1.587401f, 1.681793f, 1.781797f, 1.887749f };
    int o = (m - 69) / 12, s = (m - 69) % 12;
    if (s < 0) { s += 12; o--; }
    float f = 440.0f * semi[s];
    while (o > 0) { f *= 2; o--; }
    while (o < 0) { f *= 0.5f; o++; }
    return f;
}

// ---- engine, rival, skid, wind (continuous voices driven every frame) -------------------------------
static float e_rpm, e_rpm_t, e_load, e_load_t, e_skid, e_skid_t, a_vol, a_vol_t, a_rpm, a_rpm_t;
static uint32_t e_ph1, e_ph2, a_ph;
static float e_lp, k_lp1, k_lp2, w_lp;

void audio_engine(float rpm, int gas, float skid, float rival_vol, float rival_rpm) {
    e_rpm_t = clampf(rpm, 0, 1.3f); e_load_t = gas ? 1.0f : 0.0f; e_skid_t = clampf(skid, 0, 1);
    a_vol_t = clampf(rival_vol, 0, 1); a_rpm_t = clampf(rival_rpm, 0, 1.3f);
}

// ---- one-shot effects ---------------------------------------------------------------------------------
enum { FX_SQ, FX_NOISE, FX_KICK, FX_ARP };
typedef struct { int kind, n, len; float f0, f1, vol; uint32_t ph; float lp; const int8_t *arp; int step; } Fx;
#define NFX 6
static Fx s_fx[NFX];
static Fx *fx_new(int kind, int ms, float f0, float f1, float vol) {
    Fx *best = &s_fx[0];
    for (int i = 0; i < NFX; i++) {
        if (s_fx[i].len == 0) { best = &s_fx[i]; break; }
        if (s_fx[i].n > best->n) best = &s_fx[i];             // steal the oldest
    }
    best->kind = kind; best->n = 0; best->len = RATE * ms / 1000; best->f0 = f0; best->f1 = f1;
    best->vol = vol; best->ph = 0; best->lp = 0; best->arp = 0; best->step = 0;
    return best;
}
static float fx_sample(Fx *x) {
    const float t = (float)x->n / x->len, env = 1.0f - t;
    float v = 0;
    switch (x->kind) {
    case FX_SQ:                                               // square sweep f0 -> f1
        x->ph += inc_of(x->f0 + (x->f1 - x->f0) * t);
        v = pulse(x->ph, 0x80000000u) * env;
        break;
    case FX_NOISE: {                                          // noise through a sweeping low-pass
        const float k = clampf((x->f0 + (x->f1 - x->f0) * t) / RATE * 6.0f, 0.01f, 1);
        x->lp += (noise() - x->lp) * k;
        v = x->lp * env * 1.6f;
        break;
    }
    case FX_KICK:                                             // thud: falling sine + a little noise
        x->ph += inc_of(x->f0 + (x->f1 - x->f0) * t);
        v = (tri(x->ph) * 0.85f + noise() * 0.25f * env) * env * env;
        break;
    case FX_ARP: {                                            // a jingle: notes of `arp`, ~90 ms each
        const int per = RATE * 90 / 1000, i = x->n / per;
        const int8_t m = x->arp[i];
        if (m <= 0) { x->len = 0; return 0; }
        x->ph += inc_of(midi_hz(m));
        const float ne = 1.0f - (float)(x->n % per) / per * 0.6f;
        v = pulse(x->ph, 0x40000000u) * ne * (i >= 3 ? env * 1.5f : 1.0f);
        break;
    }
    }
    if (++x->n >= x->len) x->len = 0;
    return v * x->vol;
}

void sfx_coin(void) {
    static const int8_t a[] = { 83, 88, 0 };
    Fx *x = fx_new(FX_ARP, 180, 0, 0, 0.26f); x->arp = a;
}
void sfx_boost(void) { fx_new(FX_NOISE, 520, 900, 5000, 0.5f); fx_new(FX_SQ, 380, 220, 880, 0.12f); }
void sfx_bump(int hard) { fx_new(FX_KICK, hard ? 160 : 110, 140, 45, hard ? 0.7f : 0.5f); fx_new(FX_NOISE, 90, 2500, 400, 0.3f); }
void sfx_beep(int hi) { fx_new(FX_SQ, hi ? 360 : 150, hi ? 1046 : 523, hi ? 1046 : 523, 0.24f); }
void sfx_lap(void) {
    static const int8_t a[] = { 72, 76, 79, 84, 0 };
    Fx *x = fx_new(FX_ARP, 480, 0, 0, 0.25f); x->arp = a;
}
void sfx_final_lap(void) {
    static const int8_t a[] = { 79, 79, 79, 84, 88, 91, 0 };
    Fx *x = fx_new(FX_ARP, 600, 0, 0, 0.26f); x->arp = a;
}
void sfx_rocket(void) { fx_new(FX_NOISE, 700, 600, 6000, 0.55f); fx_new(FX_SQ, 500, 330, 1320, 0.14f); }
void sfx_flood(void) { fx_new(FX_SQ, 450, 110, 60, 0.3f); fx_new(FX_NOISE, 600, 800, 200, 0.4f); }
void sfx_whoosh(void) { fx_new(FX_NOISE, 300, 3000, 900, 0.35f); }
void sfx_click(void) { fx_new(FX_SQ, 40, 1600, 1200, 0.18f); }

// ---- music: three chiptune loops, 8 bars each ------------------------------------------------------------
// Lead = eighth notes (MIDI; 0 rest, 1 hold). Chords = one per bar (root MIDI, +100 = minor).
typedef struct { int bpm; int8_t chord[8]; int8_t lead[64]; uint8_t bass_style, drum_style; } Song;
static const Song kSongs[3] = {
    {   // Valle — A major, sunny
        140, { 57, 52, 54 + 100, 50, 57, 52, 50, 52 },
        { 76,1,81,1,85,1,83,81,  80,1,83,1,76,1,1,0,  78,1,81,1,85,1,83,81,  78,1,1,1,74,76,78,81,
          85,1,83,81,83,1,81,76,  80,1,1,76,80,1,83,1,  81,1,78,1,74,1,78,81,  83,1,1,1,80,1,1,0 }, 0, 0 },
    {   // Canyon — D minor, western
        128, { 50 + 100, 50 + 100, 48, 48, 46, 46, 45, 45 },
        { 74,1,77,1,81,1,1,79,  77,1,76,74,1,1,0,0,  72,1,76,1,79,1,1,77,  76,1,74,72,1,1,0,0,
          70,1,74,1,77,1,1,76,  74,1,1,72,70,1,69,1,  69,1,73,1,76,1,1,1,  79,1,77,1,76,1,73,1 }, 1, 1 },
    {   // Alpi — F major, sparkling
        150, { 53, 48, 50 + 100, 46, 53, 48, 46, 48 },
        { 77,81,84,81,77,81,84,89,  88,1,84,1,79,1,76,1,  74,77,81,77,74,77,81,86,  86,1,82,1,77,1,74,1,
          84,1,81,84,89,1,88,86,  84,1,1,79,76,1,79,1,  82,1,81,79,77,1,74,1,  76,1,79,1,84,1,1,0 }, 2, 0 },
};
static int s_song = -1, s_step, s_step_n, s_step_len, s_pass;
static float s_tempo = 1.0f, s_mvol = 0.0f, s_mvol_t = 0.0f;
static uint32_t m_lead_ph, m_bass_ph, m_arp_ph, m_kick_ph;
static float m_lead_hz, m_bass_hz, m_arp_hz, m_lead_env, m_bass_env, m_arp_env, m_kick_env, m_kick_hz,
             m_snare_env, m_hat_env, m_hat_lp, m_vib;
static int m_lead_note;

void audio_music(int song) {
    if (song == s_song) return;
    s_song = song; s_step = -1; s_step_n = 0; s_pass = 0; s_tempo = 1.0f;
    m_lead_env = m_bass_env = m_arp_env = m_kick_env = m_snare_env = m_hat_env = 0;
    s_mvol_t = song >= 0 ? 1.0f : 0.0f;
}
void audio_music_tempo(float k) { s_tempo = k; }
void audio_music_volume(float v) { s_mvol_t = v; }

static void music_step(void) {                 // one sixteenth
    const Song *g = &kSongs[s_song];
    s_step = (s_step + 1) & 127;               // 8 bars x 16
    if (s_step == 0) s_pass++;
    const int bar = s_step >> 4, six = s_step & 15;
    const int cr = g->chord[bar] % 100, minor = g->chord[bar] >= 100;
    const int third = cr + (minor ? 3 : 4), fifth = cr + 7;
    // lead: on eighths; the second time round an octave up for the first half (a lift)
    if (!(six & 1)) {
        const int m = g->lead[bar * 8 + (six >> 1)];
        if (m > 1) { m_lead_note = m + ((s_pass & 1) && bar < 4 ? 12 : 0); m_lead_hz = midi_hz(m_lead_note); m_lead_env = 1.0f; m_vib = 0; }
        else if (m == 0) m_lead_env = 0;
    }
    // bass: eighths, a pattern per song
    if (!(six & 1)) {
        static const int8_t kBass[3][8] = { { 0, 12, 0, 12, 0, 12, 7, 12 }, { 0, -1, 0, 7, 0, -1, 10, 7 },
                                            { 0, 12, 7, 12, 0, 12, 7, 5 } };
        const int off = kBass[g->bass_style][six >> 1];
        if (off >= 0) { m_bass_hz = midi_hz(cr - 12 + off); m_bass_env = 1.0f; }
    }
    // arpeggio: every sixteenth, chord tones up an octave
    {
        const int tones[4] = { cr + 12, third + 12, fifth + 12, third + 12 };
        m_arp_hz = midi_hz(tones[six & 3]); m_arp_env = 1.0f;
    }
    // drums
    const int kick = g->drum_style ? (six == 0 || six == 7 || six == 10) : (six == 0 || six == 8 || (six == 6 && (bar & 1)));
    if (kick) { m_kick_env = 1.0f; m_kick_hz = 150; }
    if (six == 4 || six == 12) m_snare_env = 1.0f;
    if (!(six & 1)) m_hat_env = (six & 2) ? 0.6f : 1.0f;
    if (bar == 7 && six >= 12) m_snare_env = 0.8f;       // a fill into the loop
}

static float music_sample(void) {
    if (s_song < 0) return 0;
    if (--s_step_n <= 0) {
        s_step_len = (int)(RATE * 60.0f / (kSongs[s_song].bpm * s_tempo * 4));
        s_step_n = s_step_len;
        music_step();
    }
    float v = 0;
    m_vib += 1.0f / RATE;
    const float vib = m_vib > 0.15f ? 1.0f + sinf_(m_vib * 34.0f) * 0.006f : 1.0f;   // delayed vibrato
    m_lead_ph += inc_of(m_lead_hz * vib);
    v += pulse(m_lead_ph, s_song == 2 ? 0x80000000u : 0x40000000u) * m_lead_env * 0.16f;
    m_lead_env *= 0.99992f;
    m_bass_ph += inc_of(m_bass_hz);
    v += (tri(m_bass_ph) * 0.7f + pulse(m_bass_ph, 0x80000000u) * 0.3f) * m_bass_env * 0.24f;
    m_bass_env *= 0.99975f;
    m_arp_ph += inc_of(m_arp_hz);
    v += pulse(m_arp_ph, 0x20000000u) * m_arp_env * 0.05f;
    m_arp_env *= 0.9994f;
    if (m_kick_env > 0.001f) {
        m_kick_ph += inc_of(m_kick_hz);
        m_kick_hz = 45 + (m_kick_hz - 45) * 0.9993f;
        v += tri(m_kick_ph) * m_kick_env * 0.4f;
        m_kick_env *= 0.9993f;
    }
    if (m_snare_env > 0.001f) { v += noise() * m_snare_env * 0.16f; m_snare_env *= 0.9990f; }
    if (m_hat_env > 0.001f) {
        const float n = noise();
        m_hat_lp += (n - m_hat_lp) * 0.3f;
        v += (n - m_hat_lp) * m_hat_env * 0.07f;         // high-passed noise
        m_hat_env *= 0.996f;
    }
    return v;
}

// ---- the mixer -------------------------------------------------------------------------------------------
int audio_start(void) {
    if (!s_open) s_open = nv_audio_open(RATE, 1) == 1;
    e_rpm = e_load = e_skid = a_vol = 0;
    for (int i = 0; i < NFX; i++) s_fx[i].len = 0;
    return s_open;
}
void audio_stop(void) {
    if (s_open) nv_audio_close();
    s_open = 0;
    s_song = -1;
}
int audio_on(void) { return s_open; }

static void mix_block(void) {
    for (int i = 0; i < BLOCK; i++) {
        // parameter glides (per sample, ~20-60 ms)
        e_rpm += (e_rpm_t - e_rpm) * 0.0012f; e_load += (e_load_t - e_load) * 0.0025f;
        e_skid += (e_skid_t - e_skid) * 0.002f; a_vol += (a_vol_t - a_vol) * 0.001f;
        a_rpm += (a_rpm_t - a_rpm) * 0.001f; s_mvol += (s_mvol_t - s_mvol) * 0.0005f;
        float v = 0;
        // Player engine: a buzzy pulse plus a detuned saw an octave up, low-passed — brighter when
        // the throttle is open.
        const float f = 62.0f + e_rpm * 175.0f;
        e_ph1 += inc_of(f); e_ph2 += inc_of(f * 2.012f);
        const float raw = pulse(e_ph1, 0x50000000u) * 0.6f + saw(e_ph2) * 0.4f;
        e_lp += (raw - e_lp) * (0.12f + 0.3f * e_load);
        v += e_lp * (0.13f + 0.10f * e_load + 0.05f * e_rpm);
        // Nearest rival
        if (a_vol > 0.01f) { a_ph += inc_of(58.0f + a_rpm * 170.0f); v += pulse(a_ph, 0x60000000u) * a_vol * 0.07f; }
        // Tyre squeal: band-limited noise plus a wobbling whistle
        if (e_skid > 0.01f) {
            const float n = noise();
            k_lp1 += (n - k_lp1) * 0.35f; k_lp2 += (k_lp1 - k_lp2) * 0.35f;
            v += ((k_lp1 - k_lp2) * 1.8f) * e_skid * 0.18f;
        }
        // Wind at speed
        if (e_rpm > 0.4f) { w_lp += (noise() - w_lp) * 0.05f; v += w_lp * (e_rpm - 0.4f) * 0.35f; }
        for (int k = 0; k < NFX; k++) if (s_fx[k].len) v += fx_sample(&s_fx[k]);
        v += music_sample() * s_mvol;
        int s = (int)(v * 12500.0f);
        s_blk[i] = (int16_t)(s > 20000 ? 20000 : s < -20000 ? -20000 : s);
    }
}

void audio_pump(void) {
    if (!s_open) return;
    int backlog = nv_audio_backlog();
    if (backlog < 0) { s_open = 0; return; }
    for (int guard = 0; backlog < TARGET && guard < 24; guard++) {
        mix_block();
        if (nv_audio_write(s_blk, sizeof s_blk) < 0) { s_open = 0; return; }
        backlog += sizeof s_blk;
    }
}
