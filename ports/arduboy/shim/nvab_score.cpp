// nvab_score.cpp — see nvab_score.h. Part of ports/arduboy (BSD-3-Clause, see LICENSE).
#include "nvab_score.h"
#include <math.h>
#include "nvab.h"

namespace nvab {

static bool (*s_enabled)() = nullptr;
static int s_channels;
static const uint8_t *s_score, *s_pos;
static uint32_t s_wait;                  // ms until the next command
static bool s_playing;
static uint32_t s_tone_left;             // ms, 0 = none; UINT32_MAX = until stopped
static bool s_tone_on, s_tone_mutes = true;
static bool s_note_on[2];

static bool enabled() { return !s_enabled || s_enabled(); }
static float note_hz(uint8_t n) { return 440.0f * powf(2.0f, ((float)n - 69.0f) / 12.0f); }

static void note(int ch, uint8_t n) {
    if (ch >= 2) return;
    s_note_on[ch] = true;
    if (ch == 1 && s_tone_on && s_tone_mutes) return;
    if (enabled()) voice_on(V_SCORE0 + ch, note_hz(n & 0x7F), VOL_NORMAL);
}
static void note_off(int ch) {
    if (ch >= 2) return;
    s_note_on[ch] = false;
    voice_off(V_SCORE0 + ch);
}

static const uint8_t *s_mark;           // OBONO dialect: repeat point (0xD0), repeat count
static int s_repeat, s_pitch, s_pitch0;

static void step() {   // run commands until the next wait
    for (int guard = 0; guard < 256 && s_playing; guard++) {
        const uint8_t c = *s_pos++;
        if (c & 0x80) {
            // generator in the low 2 bits (OBONO's scores keep a duty cycle in bits 2-3)
            const uint8_t op = c & 0xF0, ch = c & 0x03, val = c & 0x0F;
            if (op == 0x90) note(ch, (uint8_t)(*s_pos++ + s_pitch));
            else if (op == 0x80) note_off(ch);
            else if (op == 0xD0) s_mark = s_pos - 1;
            else if (op == 0xE0) {        // restart (Playtune) / repeat from the mark n times (OBONO)
                if (val == 0 || ++s_repeat < (1 << val)) s_pos = s_mark;
                else { s_mark = s_pos; s_pitch = s_pitch0; s_repeat = 0; }
            }
            else if (op == 0xF0) { score_stop(); return; }
            else { score_stop(); return; }   // unknown opcode: stop rather than read garbage
        } else {
            s_wait = ((uint32_t)c << 8) | *s_pos++;
            if (s_wait) return;
        }
    }
}

static void tick() {
    if (s_playing) {
        if (s_wait > 1) s_wait--;
        else { s_wait = 0; step(); }
    }
    if (s_tone_on && s_tone_left != UINT32_MAX) {
        if (s_tone_left > 1) s_tone_left--;
        else {
            s_tone_on = false;
            voice_off(V_TONES);
        }
    }
}

void score_set_enable(bool (*en)()) { s_enabled = en; add_ticker(tick); }
void score_init_channel() { if (s_channels < 2) s_channels++; add_ticker(tick); }
void score_close_channels() { score_stop(); s_channels = 0; }
void score_play(const uint8_t *score, int pitch) {
    add_ticker(tick);
    score_stop();
    if (!score) return;
    s_score = s_pos = s_mark = score;
    s_pitch = s_pitch0 = pitch;
    s_repeat = 0;
    s_playing = true;
    s_wait = 0;
    step();
}
void score_stop() {
    s_playing = false;
    note_off(0);
    note_off(1);
}
bool score_playing() { return s_playing; }
void score_tone(unsigned int freq, unsigned long dur) {
    add_ticker(tick);
    if (!enabled() || !freq) { voice_off(V_TONES); s_tone_on = false; return; }
    s_tone_on = true;
    s_tone_left = dur ? (uint32_t)dur : UINT32_MAX;
    if (s_tone_mutes) voice_off(V_SCORE1);
    voice_on(V_TONES, (float)freq, VOL_NORMAL);
}
bool score_tone_playing() { return s_tone_on; }
void score_tone_mutes(bool on) { s_tone_mutes = on; }

}  // namespace nvab
