// Arduboy2Audio.cpp — see Arduboy2Audio.h. Part of ports/arduboy (BSD-3-Clause, see LICENSE).
#include "Arduboy2Audio.h"
#include "nvab.h"

bool Arduboy2Audio::audio_enabled = false;

enum { EE_AUDIO_ON_OFF = 2 };   // Arduboy2Base::eepromAudioOnOff

void Arduboy2Audio::on() { audio_enabled = true; }
void Arduboy2Audio::off() {
    audio_enabled = false;
    for (int v = 0; v < nvab::V_COUNT; v++) nvab::voice_off(v);
}
void Arduboy2Audio::toggle() { if (audio_enabled) off(); else on(); }
void Arduboy2Audio::saveOnOff() { EEPROM.update(EE_AUDIO_ON_OFF, audio_enabled); }
void Arduboy2Audio::begin() { if (EEPROM.read(EE_AUDIO_ON_OFF)) on(); else off(); }
bool Arduboy2Audio::enabled() { return audio_enabled; }
