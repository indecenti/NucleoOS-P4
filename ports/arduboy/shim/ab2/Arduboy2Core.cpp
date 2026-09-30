// Arduboy2Core.cpp — NucleoOS hardware layer for the Arduboy2 library (see Arduboy2Core.h).
// Part of ports/arduboy (BSD-3-Clause, see LICENSE).
#include "Arduboy2Core.h"
#include "nvab.h"

void Arduboy2Core::idle() { nvab::idle(); }
uint8_t Arduboy2Core::buttonsState() { return nvab::buttons(); }
void Arduboy2Core::boot() { nvab::boot(); }
unsigned long Arduboy2Core::generateRandomSeed() { return (unsigned long)nvab::random32() ^ nvab::micros(); }
void Arduboy2Core::delayShort(uint16_t ms) { nvab::delay(ms); }
void Arduboy2Core::exitToBootloader() { nvab::quit(); }
void Arduboy2Core::sendLCDCommand(uint8_t command) { nvab::lcd_command(command); }
void Arduboy2Core::invert(bool inverse) { nvab::set_invert(inverse); }
void Arduboy2Core::allPixelsOn(bool on) { nvab::set_all_on(on); }
void Arduboy2Core::blank() { nvab::blank(); }

// paint8Pixels: a sketch streaming its own bytes to the display (128 columns x 8 pages order).
static uint8_t s_stream[1024];
static uint16_t s_stream_pos;
void Arduboy2Core::paint8Pixels(uint8_t pixels) {
    s_stream[s_stream_pos++] = pixels;
    if (s_stream_pos >= sizeof s_stream) {
        s_stream_pos = 0;
        nvab::paint(s_stream);
    }
}
void Arduboy2Core::paintScreen(const uint8_t *image) { nvab::paint(image); }
void Arduboy2Core::paintScreen(uint8_t image[], bool clear) {
    nvab::paint(image);
    if (clear) memset(image, 0, WIDTH * HEIGHT / 8);
}
