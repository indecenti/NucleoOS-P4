// ArduboyTones.h — NucleoOS implementation of the ArduboyTones API (clean-room: same constants and
// methods; tones are square waves on the nvab synth, sequences step in audio time).
// Note names come from ArduboyTonesPitches.h (copied from the pinned ArduboyTones release, MIT).
// Part of ports/arduboy (BSD-3-Clause, see LICENSE).
#ifndef ARDUBOY_TONES_H
#define ARDUBOY_TONES_H

#include <Arduino.h>
#include "ArduboyTonesPitches.h"

#define TONES_END 0x8000
#define TONES_REPEAT 0x8001
#define TONE_HIGH_VOLUME 0x8000
#define VOLUME_IN_TONE 0
#define VOLUME_ALWAYS_NORMAL 1
#define VOLUME_ALWAYS_HIGH 2
#define MAX_TONES 3

class ArduboyTones {
public:
    ArduboyTones(bool (*outEn)());
    static void tone(uint16_t freq, uint16_t dur = 0);
    static void tone(uint16_t freq1, uint16_t dur1, uint16_t freq2, uint16_t dur2);
    static void tone(uint16_t freq1, uint16_t dur1, uint16_t freq2, uint16_t dur2, uint16_t freq3, uint16_t dur3);
    static void tones(const uint16_t *tones);
    static void tonesInRAM(uint16_t *tones);
    static void noTone();
    static void volumeMode(uint8_t mode);
    static bool playing();
    static void setOutputEnabled(bool (*outEn)());

private:
    static void start(const uint16_t *seq, bool ram);
    static void next();
    static void tick();
};

#endif
