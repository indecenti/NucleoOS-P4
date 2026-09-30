// Print.h — Arduino-compatible Print base class (clean-room): print/println for text, F() strings,
// characters, integers in any base and floats. Part of ports/arduboy.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "WString.h"

#define DEC 10
#define HEX 16
#define OCT 8
#define BIN 2

class Printable;

class Print {
public:
    virtual ~Print() {}
    virtual size_t write(uint8_t c) = 0;
    virtual size_t write(const uint8_t *buf, size_t n);
    size_t write(const char *s) { return s ? write((const uint8_t *)s, strlen(s)) : 0; }
    size_t write(const char *buf, size_t n) { return write((const uint8_t *)buf, n); }
    virtual int availableForWrite() { return 0; }
    virtual void flush() {}
    int getWriteError() { return 0; }
    void clearWriteError() {}

    size_t print(const __FlashStringHelper *s) { return write(reinterpret_cast<const char *>(s)); }
    size_t print(const String &s) { return write(s.c_str(), s.length()); }
    size_t print(const char s[]) { return write(s); }
    size_t print(char c) { return write((uint8_t)c); }
    size_t print(unsigned char n, int base = DEC) { return printNumber((unsigned long)n, base); }
    size_t print(int n, int base = DEC) { return printSigned((long)n, base); }
    size_t print(unsigned int n, int base = DEC) { return printNumber((unsigned long)n, base); }
    size_t print(long n, int base = DEC) { return printSigned(n, base); }
    size_t print(unsigned long n, int base = DEC) { return printNumber(n, base); }
    size_t print(long long n, int base = DEC);
    size_t print(unsigned long long n, int base = DEC);
    size_t print(double n, int digits = 2) { return printFloat(n, digits); }
    size_t print(const Printable &p);

    size_t println() { return write('\r') + write('\n'); }
    template <class T> size_t println(const T &v) { size_t n = print(v); return n + println(); }
    template <class T> size_t println(const T &v, int b) { size_t n = print(v, b); return n + println(); }
    size_t println(const char s[]) { size_t n = print(s); return n + println(); }

protected:
    size_t printNumber(unsigned long n, int base);
    size_t printSigned(long n, int base);
    size_t printFloat(double n, int digits);
};

class Printable {
public:
    virtual ~Printable() {}
    virtual size_t printTo(Print &p) const = 0;
};
