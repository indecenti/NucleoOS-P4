// Arduboy2Beep.cpp — see Arduboy2Beep.h. Part of ports/arduboy (BSD-3-Clause, see LICENSE).
#include "Arduboy2Beep.h"
#include "nvab.h"

uint8_t BeepPin1::duration = 0;
uint8_t BeepPin2::duration = 0;

void BeepPin1::tone(uint16_t count) { tone(count, 0); }
void BeepPin1::tone(uint16_t count, uint8_t dur) {
    duration = dur;
    nvab::voice_on(nvab::V_BEEP1, 1000000.0f / (float)(count + 1), nvab::VOL_HIGH);
}
void BeepPin1::timer() { if (duration && --duration == 0) noTone(); }
void BeepPin1::noTone() { duration = 0; nvab::voice_off(nvab::V_BEEP1); }

void BeepPin2::tone(uint16_t count) { tone(count, 0); }
void BeepPin2::tone(uint16_t count, uint8_t dur) {
    duration = dur;
    nvab::voice_on(nvab::V_BEEP2, 62500.0f / (float)(count + 1), nvab::VOL_NORMAL);
}
void BeepPin2::timer() { if (duration && --duration == 0) noTone(); }
void BeepPin2::noTone() { duration = 0; nvab::voice_off(nvab::V_BEEP2); }
