// audio.h — ArduboyAudio + ArduboyTunes of the Arduboy 1.x library for NucleoOS (clean-room: same
// classes and methods; scores and tones play on the nvab synth, see nvab_score.h).
// Part of ports/arduboy (BSD-3-Clause, see LICENSE).
#ifndef ArduboyAudio_h
#define ArduboyAudio_h

#include <Arduino.h>
#include <EEPROM.h>
#include <avr/pgmspace.h>
#include <avr/power.h>

#define AVAILABLE_TIMERS 2
#define TUNE_OP_PLAYNOTE 0x90
#define TUNE_OP_STOPNOTE 0x80
#define TUNE_OP_RESTART 0xe0
#define TUNE_OP_STOP 0xf0

class ArduboyAudio {
public:
    void static begin();
    void static on();
    void static off();
    void static saveOnOff();
    bool static enabled();

protected:
    bool static audio_enabled;
};

class ArduboyTunes {
public:
    void initChannel(byte pin);
    void playScore(const byte *score);
    void stopScore();
    void delay(unsigned msec);
    void closeChannels();
    bool playing();
    void tone(unsigned int frequency, unsigned long duration);
    void static step() {}
    void static soundOutput() {}
};

#endif
