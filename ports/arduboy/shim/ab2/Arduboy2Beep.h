// Arduboy2Beep.h — BeepPin1 / BeepPin2 of the Arduboy2 library (clean-room, same API): a square
// wave whose "count" is the AVR timer count, duration in frames (timer() once per frame).
// Part of ports/arduboy (BSD-3-Clause, see LICENSE).
#ifndef ARDUBOY2_BEEP_H
#define ARDUBOY2_BEEP_H

#include <stdint.h>

class BeepPin1 {
public:
    static uint8_t duration;
    static void begin() {}
    static void tone(uint16_t count);
    static void tone(uint16_t count, uint8_t dur);
    static void timer();
    static void noTone();
    static constexpr uint16_t freq(const float hz) { return (uint16_t)(((16000000UL / 8 / 2) + (hz / 2)) / hz) - 1; }
};

class BeepPin2 {
public:
    static uint8_t duration;
    static void begin() {}
    static void tone(uint16_t count);
    static void tone(uint16_t count, uint8_t dur);
    static void timer();
    static void noTone();
    static constexpr uint16_t freq(const float hz) { return (uint16_t)(((16000000UL / 128 / 2) + (hz / 2)) / hz) - 1; }
};

#endif
