// core.cpp — NucleoOS hardware core of the Arduboy 1.x library (see core.h).
// Part of ports/arduboy (BSD-3-Clause, see LICENSE).
#include "core.h"
#include "nvab.h"

void ArduboyCore::idle() { nvab::idle(); }
uint8_t ArduboyCore::buttonsState() { return nvab::buttons(); }
void ArduboyCore::boot() { nvab::boot(); }
void ArduboyCore::blank() { nvab::blank(); }
void ArduboyCore::invert(boolean inverse) { nvab::set_invert(inverse); }
void ArduboyCore::allPixelsOn(boolean on) { nvab::set_all_on(on); }
void ArduboyCore::sendLCDCommand(uint8_t command) { nvab::lcd_command(command); }
static uint8_t s_stream[1024];
static uint16_t s_stream_pos;
void ArduboyCore::paint8Pixels(uint8_t pixels) {
    s_stream[s_stream_pos++] = pixels;
    if (s_stream_pos >= sizeof s_stream) {
        s_stream_pos = 0;
        nvab::paint(s_stream);
    }
}
void ArduboyCore::paintScreen(const unsigned char *image) { nvab::paint(image); }
void ArduboyCore::paintScreen(unsigned char image[]) { nvab::paint(image); }
