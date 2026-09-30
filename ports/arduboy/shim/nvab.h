// nvab.h — the NucleoOS side of the Arduboy shim: what the Arduboy2Core / Arduboy core / audio
// classes call into. Implemented in nvab.cpp. Part of ports/arduboy (BSD-3-Clause, see LICENSE).
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
void nvab_idle(void);
#ifdef __cplusplus
}

namespace nvab {

// Arduboy button bits (Arduboy2Core.h / core.h numbering, production Arduboy 1.0)
enum : uint8_t { BTN_B = 1 << 2, BTN_A = 1 << 3, BTN_DOWN = 1 << 4, BTN_LEFT = 1 << 5, BTN_RIGHT = 1 << 6, BTN_UP = 1 << 7 };

void boot();                              // idempotent: config, EEPROM image, canvas
void paint(const uint8_t *buf);           // show a 1024-byte SSD1306 page buffer, then present
void lcd_command(uint8_t cmd);            // invert / all-pixels-on / normal; others ignored
void blank();                             // an all-black frame
void set_invert(bool on);
void set_all_on(bool on);
uint8_t buttons();                        // BTN_* held now (touch + pads + keyboard)
uint32_t millis();                        // since start, pause-menu time excluded
uint32_t micros();
void delay(uint32_t ms);
void idle();                              // ~1 ms nap + host service when due
void poll();                              // host service if a present is overdue
void quit();                              // save, close audio, end the app (never returns)
uint32_t random32();
void random_seed(uint32_t s);

// ---- square-wave synth, 48 kHz --------------------------------------------------------------
enum { V_SCORE0, V_SCORE1, V_TONES, V_BEEP1, V_BEEP2, V_TONE, V_COUNT };
enum { VOL_OFF = 0, VOL_NORMAL = 1, VOL_HIGH = 2 };
void voice_on(int v, float freq_hz, int vol);
void voice_off(int v);
bool voice_active(int v);
// A sequencer (ArduboyTones, Playtune scores, tone() durations) runs in audio time: every
// registered ticker is called once per millisecond of generated sound.
typedef void (*ticker_fn)(void);
void add_ticker(ticker_fn f);

// ---- EEPROM image (1 KB) --------------------------------------------------------------------
uint8_t ee_read(uint16_t a);
void ee_write(uint16_t a, uint8_t v);

}  // namespace nvab
#endif
