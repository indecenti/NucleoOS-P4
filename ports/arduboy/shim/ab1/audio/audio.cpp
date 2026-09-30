// audio.cpp — see audio.h. Part of ports/arduboy (BSD-3-Clause, see LICENSE).
#include "audio.h"
#include "nvab.h"
#include "nvab_score.h"

enum { EE_AUDIO_ON_OFF = 2 };   // EEPROM_AUDIO_ON_OFF of Arduboy 1.x

bool ArduboyAudio::audio_enabled = false;
static bool audio_on() { return ArduboyAudio::enabled(); }

void ArduboyAudio::on() { audio_enabled = true; }
void ArduboyAudio::off() {
    audio_enabled = false;
    for (int v = 0; v < nvab::V_COUNT; v++) nvab::voice_off(v);
}
void ArduboyAudio::saveOnOff() { EEPROM.update(EE_AUDIO_ON_OFF, audio_enabled); }
void ArduboyAudio::begin() { if (EEPROM.read(EE_AUDIO_ON_OFF)) on(); else off(); }
bool ArduboyAudio::enabled() { return audio_enabled; }

void ArduboyTunes::initChannel(byte) { nvab::score_set_enable(audio_on); nvab::score_init_channel(); }
void ArduboyTunes::playScore(const byte *score) { nvab::score_set_enable(audio_on); nvab::score_play(score); }
void ArduboyTunes::stopScore() { nvab::score_stop(); }
void ArduboyTunes::delay(unsigned msec) { ::delay(msec); }
void ArduboyTunes::closeChannels() { nvab::score_close_channels(); }
bool ArduboyTunes::playing() { return nvab::score_playing(); }
void ArduboyTunes::tone(unsigned int frequency, unsigned long duration) {
    nvab::score_set_enable(audio_on);
    nvab::score_tone(frequency, duration);
}
