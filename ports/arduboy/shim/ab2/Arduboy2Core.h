// Arduboy2Core.h — NucleoOS replacement for the hardware layer of the Arduboy2 library (clean-room:
// same class name, constants and static API as Arduboy2Core; the display, buttons, LEDs and timing
// are routed to nvab.cpp). Part of ports/arduboy (BSD-3-Clause, see LICENSE).
#ifndef ARDUBOY2_CORE_H
#define ARDUBOY2_CORE_H

#include <Arduino.h>
#include <avr/power.h>
#include <avr/sleep.h>
#include <limits.h>

#define ARDUBOY_10
#define RGB_ON LOW
#define RGB_OFF HIGH

#define PIN_CS 12
#define PIN_DC 4
#define PIN_RST 6
#define RED_LED 10
#define GREEN_LED 11
#define BLUE_LED 9
#define RED_LED_PORT PORTB
#define RED_LED_BIT PORTB6
#define GREEN_LED_PORT PORTB
#define GREEN_LED_BIT PORTB7
#define BLUE_LED_PORT PORTB
#define BLUE_LED_BIT PORTB5

#define LEFT_BUTTON _BV(5)
#define RIGHT_BUTTON _BV(6)
#define UP_BUTTON _BV(7)
#define DOWN_BUTTON _BV(4)
#define A_BUTTON _BV(3)
#define B_BUTTON _BV(2)

#define PIN_LEFT_BUTTON A2
#define PIN_RIGHT_BUTTON A1
#define PIN_UP_BUTTON A0
#define PIN_DOWN_BUTTON A3
#define PIN_A_BUTTON 7
#define PIN_B_BUTTON 8
#define LEFT_BUTTON_PORTIN PINF
#define RIGHT_BUTTON_PORTIN PINF
#define UP_BUTTON_PORTIN PINF
#define DOWN_BUTTON_PORTIN PINF
#define A_BUTTON_PORTIN PINE
#define B_BUTTON_PORTIN PINB
#define LEFT_BUTTON_BIT PORTF5
#define RIGHT_BUTTON_BIT PORTF6
#define UP_BUTTON_BIT PORTF7
#define DOWN_BUTTON_BIT PORTF4
#define A_BUTTON_BIT PORTE6
#define B_BUTTON_BIT PORTB4

#define PIN_SPEAKER_1 5
#define PIN_SPEAKER_2 13
#define SPEAKER_1_PORT PORTC
#define SPEAKER_1_DDR DDRC
#define SPEAKER_1_BIT PORTC6
#define SPEAKER_2_PORT PORTC
#define SPEAKER_2_DDR DDRC
#define SPEAKER_2_BIT PORTC7

#define RAND_SEED_IN A4

#define OLED_PIXELS_INVERTED 0xA7
#define OLED_PIXELS_NORMAL 0xA6
#define OLED_ALL_PIXELS_ON 0xA5
#define OLED_PIXELS_FROM_RAM 0xA4
#define OLED_VERTICAL_FLIPPED 0xC0
#define OLED_VERTICAL_NORMAL 0xC8
#define OLED_HORIZ_FLIPPED 0xA0
#define OLED_HORIZ_NORMAL 0xA1
#define OLED_SET_PAGE_ADDRESS 0xB0
#define OLED_SET_COLUMN_ADDRESS_LOW 0x00
#define OLED_SET_COLUMN_ADDRESS_HIGH 0x10

#define WIDTH 128
#define HEIGHT 64
#define COLUMN_ADDRESS_END (WIDTH - 1) & 127
#define PAGE_ADDRESS_END ((HEIGHT / 8) - 1) & 7

#define ARDUBOY_NO_USB

class Arduboy2NoUSB {
};

class Arduboy2Core : public Arduboy2NoUSB {
    friend class Arduboy2Ex;

public:
    static void idle();
    static void LCDDataMode() {}
    static void LCDCommandMode() {}
    static void SPItransfer(uint8_t) {}
    static uint8_t SPItransferAndRead(uint8_t) { return 0; }
    static void displayOff() {}
    static void displayOn() {}
    static constexpr uint8_t width() { return WIDTH; }
    static constexpr uint8_t height() { return HEIGHT; }
    static uint8_t buttonsState();
    static void paint8Pixels(uint8_t pixels);
    static void paintScreen(const uint8_t *image);
    static void paintScreen(uint8_t image[], bool clear = false);
    static void blank();
    static void invert(bool inverse);
    static void allPixelsOn(bool on);
    static void flipVertical(bool) {}
    static void flipHorizontal(bool) {}
    static void sendLCDCommand(uint8_t command);
    static void setRGBled(uint8_t, uint8_t, uint8_t) {}
    static void setRGBled(uint8_t, uint8_t) {}
    static void freeRGBled() {}
    static void digitalWriteRGB(uint8_t, uint8_t, uint8_t) {}
    static void digitalWriteRGB(uint8_t, uint8_t) {}
    static void boot();
    static void safeMode() {}
    static unsigned long generateRandomSeed();
    static void delayShort(uint16_t ms);
    static void exitToBootloader();
    static void mainNoUSB() {}

protected:
    static void setCPUSpeed8MHz() {}
    static void bootSPI() {}
    static void bootOLED() {}
    static void bootPins() {}
    static void bootPowerSaving() {}
};

#endif
