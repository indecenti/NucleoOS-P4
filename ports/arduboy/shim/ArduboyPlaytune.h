// ArduboyPlaytune.h — NucleoOS implementation of the ArduboyPlaytune API (clean-room: same class
// and methods; scores play on the nvab synth, see nvab_score.h). Part of ports/arduboy.
#ifndef ARDUBOY_PLAYTUNE_H
#define ARDUBOY_PLAYTUNE_H

#include <Arduino.h>
#include "nvab_score.h"

#define TUNE_OP_PLAYNOTE 0x90
#define TUNE_OP_STOPNOTE 0x80
#define TUNE_OP_RESTART 0xe0
#define TUNE_OP_STOP 0xf0

class ArduboyPlaytune {
public:
    ArduboyPlaytune(bool (*outEn)()) { nvab::score_set_enable(outEn); }
    void initChannel(byte) { nvab::score_init_channel(); }
    void playScore(const byte *score) { nvab::score_play(score); }
    void stopScore() { nvab::score_stop(); }
    bool playing() { return nvab::score_playing(); }
    void closeChannels() { nvab::score_close_channels(); }
    void tone(unsigned int frequency, unsigned long duration) { nvab::score_tone(frequency, duration); }
    void toneMutesScore(bool mute) { nvab::score_tone_mutes(mute); }
};

#endif
