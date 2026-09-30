// EEPROM.h — Arduino EEPROM library API over the NucleoOS 1 KB EEPROM image (see avr/eeprom.h):
// read/write/update/get/put, EEPROM[i] references and length(). Clean-room. Part of ports/arduboy.
#pragma once
#include <stdint.h>
#include "avr/eeprom.h"

struct EERef {
    int index;
    EERef(int i) : index(i) {}
    uint8_t operator*() const { return nvab_ee_read((uint16_t)index); }
    operator uint8_t() const { return **this; }
    EERef &operator=(const EERef &r) { return *this = *r; }
    EERef &operator=(uint8_t v) { nvab_ee_write((uint16_t)index, v); return *this; }
    EERef &operator+=(uint8_t v) { return *this = **this + v; }
    EERef &operator-=(uint8_t v) { return *this = **this - v; }
    EERef &operator*=(uint8_t v) { return *this = **this * v; }
    EERef &operator/=(uint8_t v) { return *this = **this / v; }
    EERef &operator^=(uint8_t v) { return *this = **this ^ v; }
    EERef &operator%=(uint8_t v) { return *this = **this % v; }
    EERef &operator&=(uint8_t v) { return *this = **this & v; }
    EERef &operator|=(uint8_t v) { return *this = **this | v; }
    EERef &operator<<=(uint8_t v) { return *this = **this << v; }
    EERef &operator>>=(uint8_t v) { return *this = **this >> v; }
    EERef &update(uint8_t v) { if (v != **this) *this = v; return *this; }
    EERef &operator++() { return *this += 1; }
    EERef &operator--() { return *this -= 1; }
    uint8_t operator++(int) { uint8_t r = **this; ++(*this); return r; }
    uint8_t operator--(int) { uint8_t r = **this; --(*this); return r; }
};

struct EEPtr {
    int index;
    EEPtr(int i) : index(i) {}
    operator int() const { return index; }
    EEPtr &operator=(int i) { index = i; return *this; }
    bool operator!=(const EEPtr &p) { return index != p.index; }
    EERef operator*() { return EERef(index); }
    EEPtr &operator++() { ++index; return *this; }
    EEPtr &operator--() { --index; return *this; }
    EEPtr operator++(int) { return EEPtr(index++); }
    EEPtr operator--(int) { return EEPtr(index--); }
};

struct EEPROMClass {
    EERef operator[](int i) { return EERef(i); }
    uint8_t read(int i) { return nvab_ee_read((uint16_t)i); }
    void write(int i, uint8_t v) { nvab_ee_write((uint16_t)i, v); }
    void update(int i, uint8_t v) { if (read(i) != v) write(i, v); }
    EEPtr begin() { return EEPtr(0); }
    EEPtr end() { return EEPtr(length()); }
    uint16_t length() { return E2END + 1; }
    template <typename T> T &get(int idx, T &t) {
        uint8_t *p = (uint8_t *)&t;
        for (unsigned n = 0; n < sizeof(T); n++) p[n] = read(idx + (int)n);
        return t;
    }
    template <typename T> const T &put(int idx, const T &t) {
        const uint8_t *p = (const uint8_t *)&t;
        for (unsigned n = 0; n < sizeof(T); n++) update(idx + (int)n, p[n]);
        return t;
    }
    void commit() {}
};
extern EEPROMClass EEPROM;
