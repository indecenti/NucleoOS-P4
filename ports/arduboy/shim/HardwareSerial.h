// HardwareSerial.h — Serial goes to the OS log (a line at a time, rate-limited); input never
// arrives. Part of ports/arduboy.
#pragma once
#include "Print.h"

class NvabSerial : public Print {
public:
    void begin(unsigned long) {}
    void begin(unsigned long, uint8_t) {}
    void end() {}
    int available() { return 0; }
    int read() { return -1; }
    int peek() { return -1; }
    size_t readBytes(char *, size_t) { return 0; }
    void setTimeout(unsigned long) {}
    size_t write(uint8_t c) override;
    using Print::write;
    operator bool() { return true; }
};
extern NvabSerial Serial;
extern NvabSerial Serial1;
typedef NvabSerial Serial_;
typedef NvabSerial HardwareSerial;
