// core.h — NucleoOS replacement for the hardware core of the Arduboy 1.x library (clean-room: same
// ArduboyCore class, constants and API, routed to nvab.cpp). Part of ports/arduboy (BSD-3-Clause).
#ifndef ArduboyCore_h
#define ArduboyCore_h

#include <Arduino.h>
#include <avr/power.h>
#include <SPI.h>
#include <avr/sleep.h>
#include <limits.h>

#define ARDUBOY_10
#define DEVKIT
#define SAFE_MODE

#define CS 12
#define DC 4
#define RST 6
#define RED_LED 10
#define GREEN_LED 11
#define BLUE_LED 9
#define TX_LED 30
#define RX_LED 17
#define PIN_LEFT_BUTTON A2
#define PIN_RIGHT_BUTTON A1
#define PIN_UP_BUTTON A0
#define PIN_DOWN_BUTTON A3
#define PIN_A_BUTTON 7
#define PIN_B_BUTTON 8
#define LEFT_BUTTON _BV(5)
#define RIGHT_BUTTON _BV(6)
#define UP_BUTTON _BV(7)
#define DOWN_BUTTON _BV(4)
#define A_BUTTON _BV(3)
#define B_BUTTON _BV(2)
#define PIN_SPEAKER_1 5
#define PIN_SPEAKER_2 13
#define PIN_SPEAKER_1_PORT &PORTC
#define PIN_SPEAKER_2_PORT &PORTC
#define PIN_SPEAKER_1_BITMASK _BV(6)
#define PIN_SPEAKER_2_BITMASK _BV(7)

#define OLED_PIXELS_INVERTED 0xA7
#define OLED_PIXELS_NORMAL 0xA6
#define OLED_ALL_PIXELS_ON 0xA5
#define OLED_PIXELS_FROM_RAM 0xA4
#define OLED_VERTICAL_FLIPPED 0xC0
#define OLED_VERTICAL_NORMAL 0xC8
#define OLED_HORIZ_FLIPPED 0xA0
#define OLED_HORIZ_NORMAL 0xA1

#define COLUMN_ADDRESS_END (WIDTH - 1) & 0x7F
#define PAGE_ADDRESS_END ((HEIGHT / 8) - 1) & 0x07
#define WIDTH 128
#define HEIGHT 64
#define INVERT 2
#define WHITE 1
#define BLACK 0

class ArduboyCore {
public:
    ArduboyCore() {}
    void static idle();
    void static LCDDataMode() {}
    void static LCDCommandMode() {}
    uint8_t static width() { return WIDTH; }
    uint8_t static height() { return HEIGHT; }
    uint8_t static getInput() { return buttonsState(); }
    uint8_t static buttonsState();
    void static paint8Pixels(uint8_t pixels);
    void static paintScreen(const unsigned char *image);
    void static paintScreen(unsigned char image[]);
    void static blank();
    void static invert(boolean inverse);
    void static allPixelsOn(boolean on);
    void static flipVertical(boolean) {}
    void static flipHorizontal(boolean) {}
    void static sendLCDCommand(uint8_t command);
    void setRGBled(uint8_t, uint8_t, uint8_t) {}

protected:
    void static boot();
    void static safeMode() {}
    void static bootLCD() {}
    void static bootPins() {}
    void static slowCPU() {}
    void static saveMuchPower() {}
};

#endif
