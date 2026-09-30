// SPI.h — stub: there is no SPI bus for sketches on NucleoOS. Part of ports/arduboy.
#pragma once
#include "Arduino.h"
#define SPI_CLOCK_DIV2 0
#define SPI_MODE0 0
struct SPISettings {
    SPISettings() {}
    SPISettings(uint32_t, uint8_t, uint8_t) {}
};
class SPIClass {
public:
    static void begin() {}
    static void end() {}
    static void beginTransaction(SPISettings) {}
    static void endTransaction() {}
    static uint8_t transfer(uint8_t) { return 0; }
    static void transfer(void *, size_t) {}
    static void setClockDivider(uint8_t) {}
    static void setDataMode(uint8_t) {}
    static void setBitOrder(uint8_t) {}
};
extern SPIClass SPI;
