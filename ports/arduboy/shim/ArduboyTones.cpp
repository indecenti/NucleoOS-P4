// ArduboyTones.cpp — see ArduboyTones.h. Part of ports/arduboy (BSD-3-Clause, see LICENSE).
#include "ArduboyTones.h"
#include "nvab.h"

static bool (*s_out_en)() = nullptr;
static const uint16_t *s_seq;        // current sequence (NULL = none)
static const uint16_t *s_seq_start;
static uint16_t s_inline[MAX_TONES * 2 + 1];
static bool s_playing;
static uint32_t s_left;              // ms left of the current tone, 0 = infinite
static bool s_infinite;
static uint8_t s_vol_mode = VOLUME_IN_TONE;

ArduboyTones::ArduboyTones(bool (*outEn)()) { s_out_en = outEn; }
void ArduboyTones::setOutputEnabled(bool (*outEn)()) { s_out_en = outEn; }

void ArduboyTones::next() {
    for (;;) {
        uint16_t f = *s_seq++;
        if (f == TONES_REPEAT) { s_seq = s_seq_start; f = *s_seq++; if (f == TONES_END || f == TONES_REPEAT) break; }
        if (f == TONES_END) break;
        const uint16_t d = *s_seq++;
        const bool high = s_vol_mode == VOLUME_ALWAYS_HIGH || (s_vol_mode == VOLUME_IN_TONE && (f & TONE_HIGH_VOLUME));
        const uint16_t hz = f & ~TONE_HIGH_VOLUME;
        if (hz && (!s_out_en || s_out_en())) nvab::voice_on(nvab::V_TONES, (float)hz, high ? nvab::VOL_HIGH : nvab::VOL_NORMAL);
        else nvab::voice_off(nvab::V_TONES);
        s_infinite = d == 0;
        s_left = d;
        s_playing = true;
        return;
    }
    noTone();
}

void ArduboyTones::tick() {
    if (!s_playing || s_infinite) return;
    if (s_left > 1) { s_left--; return; }
    next();
}

void ArduboyTones::start(const uint16_t *seq, bool ram) {
    (void)ram;
    nvab::add_ticker(tick);
    s_seq = s_seq_start = seq;
    next();
}

void ArduboyTones::tone(uint16_t freq, uint16_t dur) {
    s_inline[0] = freq; s_inline[1] = dur; s_inline[2] = TONES_END;
    start(s_inline, true);
}
void ArduboyTones::tone(uint16_t f1, uint16_t d1, uint16_t f2, uint16_t d2) {
    s_inline[0] = f1; s_inline[1] = d1; s_inline[2] = f2; s_inline[3] = d2; s_inline[4] = TONES_END;
    start(s_inline, true);
}
void ArduboyTones::tone(uint16_t f1, uint16_t d1, uint16_t f2, uint16_t d2, uint16_t f3, uint16_t d3) {
    s_inline[0] = f1; s_inline[1] = d1; s_inline[2] = f2; s_inline[3] = d2; s_inline[4] = f3; s_inline[5] = d3;
    s_inline[6] = TONES_END;
    start(s_inline, true);
}
void ArduboyTones::tones(const uint16_t *t) { start(t, false); }
void ArduboyTones::tonesInRAM(uint16_t *t) { start(t, true); }
void ArduboyTones::noTone() {
    s_playing = false;
    s_seq = nullptr;
    nvab::voice_off(nvab::V_TONES);
}
void ArduboyTones::volumeMode(uint8_t mode) { s_vol_mode = mode; }
bool ArduboyTones::playing() { return s_playing; }
