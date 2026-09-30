// Arduboy2Audio.h — audio on/off switch of the Arduboy2 library (clean-room, same API). The mute
// state is stored in EEPROM like on the Arduboy; the NucleoOS pause menu has its own master switch.
// Part of ports/arduboy (BSD-3-Clause, see LICENSE).
#ifndef ARDUBOY2_AUDIO_H
#define ARDUBOY2_AUDIO_H

#include <Arduino.h>
#include <EEPROM.h>

class Arduboy2Audio {
    friend class Arduboy2Ex;

public:
    static void begin();
    static void on();
    static void off();
    static void toggle();
    static void saveOnOff();
    static bool enabled();

protected:
    static bool audio_enabled;
};

#endif
