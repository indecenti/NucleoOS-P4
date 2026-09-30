// Arduino.h — the slice of the Arduino AVR core that Arduboy sketches use, for NucleoOS (wasm32)
// and the PC harness. Clean-room: types, pin/timer no-ops, time, random, bit macros, PROGMEM as
// plain memory (avr/pgmspace.h), Print/Serial. Part of ports/arduboy (BSD-3-Clause, see LICENSE).
#pragma once
// AVR libc doesn't declare these POSIX/BSD names, and sketches use them for their own globals and
// types (y1, index, mode_t...): hide the host libc's declarations under other names.
#define y0 nvab_libc_y0
#define y1 nvab_libc_y1
#define yn nvab_libc_yn
#define j0 nvab_libc_j0
#define j1 nvab_libc_j1
#define jn nvab_libc_jn
#define y0f nvab_libc_y0f
#define y1f nvab_libc_y1f
#define j0f nvab_libc_j0f
#define j1f nvab_libc_j1f
#define index nvab_libc_index
#define rindex nvab_libc_rindex
#define mode_t nvab_libc_mode_t
#define gamma nvab_libc_gamma
#define drem nvab_libc_drem
#define significand nvab_libc_significand
#define select nvab_libc_select
#define dev_t nvab_libc_dev_t
#define key_t nvab_libc_key_t
#define id_t nvab_libc_id_t
#define ino_t nvab_libc_ino_t
#define nlink_t nvab_libc_nlink_t
#define blkcnt_t nvab_libc_blkcnt_t
#ifdef __wasi__   // (glibc pastes these names into other macros: native builds keep them)
#define sincos nvab_libc_sincos
#define sincosf nvab_libc_sincosf
#endif
#define clock nvab_libc_clock
#define time nvab_libc_time
#include <stdint.h>
#include <stddef.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <math.h>
#include <ctype.h>
#include <limits.h>
#include <time.h>
#include <sys/types.h>
#undef y0
#undef y1
#undef yn
#undef j0
#undef j1
#undef jn
#undef y0f
#undef y1f
#undef j0f
#undef j1f
#undef index
#undef rindex
#undef mode_t
#undef gamma
#undef drem
#undef significand
#undef select
#undef dev_t
#undef key_t
#undef id_t
#undef ino_t
#undef nlink_t
#undef blkcnt_t
#undef sincos
#undef sincosf
#undef clock
#undef time

#include "avr/pgmspace.h"
#include "avr/io.h"
#include "avr/interrupt.h"
#include "avr/power.h"
#include "avr/sleep.h"
#include "avr/wdt.h"
#include "avr/eeprom.h"
#include "util/delay.h"

#ifndef F_CPU
#define F_CPU 16000000UL
#endif
#define ARDUINO 10813
#define ARDUINO_AVR_LEONARDO 1
#define ARDUINO_ARCH_AVR 1

#include "binary.h"

typedef bool boolean;
typedef uint8_t byte;
typedef uint16_t word;
typedef uint8_t u8;     // USBAPI.h of the Leonardo core, pulled in by Arduino.h there
typedef uint16_t u16;
typedef uint32_t u32;

#define HIGH 0x1
#define LOW 0x0
#define INPUT 0x0
#define OUTPUT 0x1
#define INPUT_PULLUP 0x2
#define LSBFIRST 0
#define MSBFIRST 1
#define CHANGE 1
#define FALLING 2
#define RISING 3
#define DEC 10
#define HEX 16
#define OCT 8
#define BIN 2

#define PI 3.1415926535897932384626433832795
#define HALF_PI 1.5707963267948966192313216916398
#define TWO_PI 6.283185307179586476925286766559
#define DEG_TO_RAD 0.017453292519943295769236907684886
#define RAD_TO_DEG 57.295779513082320876798154814105
#define EULER 2.718281828459045235360287471352

// Analog pins of the ATmega32u4 (Leonardo numbering) — only used as pin ids.
#define A0 18
#define A1 19
#define A2 20
#define A3 21
#define A4 22
#define A5 23
#define A6 24
#define A7 25
#define A8 26
#define A9 27
#define A10 28
#define A11 29
#define LED_BUILTIN 13

// Like the AVR core these are macros (sketches mix types in min/max).
#ifndef __cplusplus
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif
#undef abs
#define abs(x) ((x) > 0 ? (x) : -(x))
#define constrain(amt, low, high) ((amt) < (low) ? (low) : ((amt) > (high) ? (high) : (amt)))
#define radians(deg) ((deg) * DEG_TO_RAD)
#define degrees(rad) ((rad) * RAD_TO_DEG)
#define sq(x) ((x) * (x))

#define lowByte(w) ((uint8_t)((w) & 0xff))
#define highByte(w) ((uint8_t)((w) >> 8))
#define bitRead(value, bit) (((value) >> (bit)) & 0x01)
#define bitSet(value, bit) ((value) |= (1UL << (bit)))
#define bitClear(value, bit) ((value) &= ~(1UL << (bit)))
#define bitToggle(value, bit) ((value) ^= (1UL << (bit)))
#define bitWrite(value, bit, bitvalue) ((bitvalue) ? bitSet(value, bit) : bitClear(value, bit))
#define bit(b) (1UL << (b))

#define interrupts() sei()
#define noInterrupts() cli()
#define clockCyclesPerMicrosecond() (F_CPU / 1000000L)
#define digitalPinToInterrupt(p) (-1)
#define digitalPinToPort(p) ((uint8_t)1)
#define digitalPinToBitMask(p) ((uint8_t)1)
#define digitalPinToTimer(p) ((uint8_t)0)
#define portOutputRegister(port) (&nvab_reg8[73])
#define portInputRegister(port) (&nvab_reg8[74])
#define portModeRegister(port) (&nvab_reg8[75])
#define NOT_A_PIN 0
#define NOT_A_PORT 0
#define NOT_ON_TIMER 0

#define TXLED0
#define TXLED1
#define RXLED0
#define RXLED1
#define TX_RX_LED_INIT

#ifdef __cplusplus
extern "C" {
#endif
unsigned long millis(void);
unsigned long micros(void);
void delay(unsigned long ms);
void delayMicroseconds(unsigned int us);
void pinMode(uint8_t pin, uint8_t mode);
void digitalWrite(uint8_t pin, uint8_t val);
int digitalRead(uint8_t pin);
int analogRead(uint8_t pin);
void analogWrite(uint8_t pin, int val);
void analogReference(uint8_t mode);
void yield(void);
void tone(uint8_t pin, unsigned int frequency, unsigned long duration);
void noTone(uint8_t pin);
unsigned long pulseIn(uint8_t pin, uint8_t state, unsigned long timeout);
void shiftOut(uint8_t dataPin, uint8_t clockPin, uint8_t bitOrder, uint8_t val);
uint8_t shiftIn(uint8_t dataPin, uint8_t clockPin, uint8_t bitOrder);
void attachInterrupt(uint8_t n, void (*f)(void), int mode);
void detachInterrupt(uint8_t n);
char *itoa(int value, char *str, int base);
char *utoa(unsigned value, char *str, int base);
char *ltoa(long value, char *str, int base);
char *ultoa(unsigned long value, char *str, int base);
char *dtostrf(double val, signed char width, unsigned char prec, char *s);
void setup(void);
void loop(void);
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
// Arduino's random(): [0, howbig) and [howsmall, howbig).
long random(long howbig);
long random(long howsmall, long howbig);
void randomSeed(unsigned long seed);
long map(long x, long in_min, long in_max, long out_min, long out_max);
inline unsigned int makeWord(unsigned int w) { return w; }
inline unsigned int makeWord(unsigned char h, unsigned char l) { return (h << 8) | l; }
#define word(...) makeWord(__VA_ARGS__)

// min/max as templates that accept mixed argument types (the AVR core has macros; templates keep
// libc++-free code happy and still accept min(int, uint8_t)).
// (the result type is that of a + b: a value, never a reference to a parameter)
template <class A, class B> inline auto min(A a, B b) -> decltype(a + b) { return a < b ? a : b; }
template <class A, class B> inline auto max(A a, B b) -> decltype(a + b) { return a > b ? a : b; }

#include "WString.h"
#include "Print.h"
#include "HardwareSerial.h"
#endif
