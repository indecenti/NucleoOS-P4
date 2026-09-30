// nvab_score.h — Playtune-format score player shared by ArduboyPlaytune (Arduboy2 sketches) and
// ArduboyTunes (Arduboy 1.x "arduboy.tunes"). Clean-room, from the public score format:
//   0x9t nn   note nn (MIDI number) on tone generator t     0x8t   stop generator t
//   0xF0      stop the score                                0xE0   restart from the beginning
//   (OBONO's variant: 0xD0 sets a repeat mark, 0xEn repeats from it 2^n times, 0xE0 forever)
//   other     (b1 & 0x7F) << 8 | b2 : wait that many milliseconds
// Part of ports/arduboy (BSD-3-Clause, see LICENSE).
#pragma once
#include <stdint.h>

namespace nvab {
void score_set_enable(bool (*enabled)());
void score_init_channel();               // one more tone generator (max 2)
void score_close_channels();
void score_play(const uint8_t *score, int pitch = 0);   // pitch: semitones added to every note
void score_stop();
bool score_playing();
void score_tone(unsigned int freq, unsigned long dur_ms);
bool score_tone_playing();
void score_tone_mutes(bool on);
}  // namespace nvab
