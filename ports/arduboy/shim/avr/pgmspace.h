// avr/pgmspace.h — PROGMEM is ordinary memory on NucleoOS: the pgm_read_* accessors read it
// directly. pgm_read_word/pgm_read_ptr on a table of pointers return the whole (32-bit) pointer,
// so the common `(const uint8_t *)pgm_read_word(&table[i])` keeps working. Part of ports/arduboy.
#pragma once
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

#define PROGMEM
#define PGM_P const char *
#define PGM_VOID_P const void *
#define PSTR(s) (s)
#define FPSTR(p) (reinterpret_cast<const __FlashStringHelper *>(p))
typedef char prog_char;
typedef unsigned char prog_uchar;
typedef int8_t prog_int8_t;
typedef uint8_t prog_uint8_t;
typedef int16_t prog_int16_t;
typedef uint16_t prog_uint16_t;
typedef int32_t prog_int32_t;
typedef uint32_t prog_uint32_t;

#define pgm_read_byte(addr) (*(const uint8_t *)(addr))
#define pgm_read_byte_near(addr) pgm_read_byte(addr)
#define pgm_read_byte_far(addr) (*(const uint8_t *)(uintptr_t)(addr))
#define pgm_read_float(addr) (*(const float *)(addr))
#define pgm_read_float_near(addr) pgm_read_float(addr)
#define pgm_get_far_address(var) ((uintptr_t)(&(var)))
typedef uintptr_t uint_farptr_t;
#define strlen_PF(a) strlen((const char *)(uintptr_t)(a))
#define strnlen_PF(a, n) strnlen((const char *)(uintptr_t)(a), n)
#define strcpy_PF(d, a) strcpy(d, (const char *)(uintptr_t)(a))
#define strncpy_PF(d, a, n) strncpy(d, (const char *)(uintptr_t)(a), n)
#define strcat_PF(d, a) strcat(d, (const char *)(uintptr_t)(a))
#define strcmp_PF(s, a) strcmp(s, (const char *)(uintptr_t)(a))

#ifdef __cplusplus
// A pointer-table element comes back as the full pointer; anything else as a 16/32-bit value read
// from possibly unaligned memory (a word stored in a byte array).
// (the pointer comes back without const, as avr-gcc -fpermissive let `uint8_t *p = pgm_read_word(..)` through)
template <class T> struct nvab_unconst { typedef T type; };
template <class T> struct nvab_unconst<const T> { typedef T type; };
template <class T> struct nvab_pgm_word { static uint16_t get(const T *p) { uint16_t v; memcpy(&v, (const void *)p, 2); return v; } };
template <class T> struct nvab_pgm_word<T *> {
    static typename nvab_unconst<T>::type *get(T *const *p) { return (typename nvab_unconst<T>::type *)*p; }
};
template <class T> struct nvab_pgm_word<T *const> {
    static typename nvab_unconst<T>::type *get(T *const *p) { return (typename nvab_unconst<T>::type *)*p; }
};
template <class T> inline auto nvab_pgm_read_word(const T *p) -> decltype(nvab_pgm_word<T>::get(p)) { return nvab_pgm_word<T>::get(p); }
inline uint16_t nvab_pgm_read_word(const void *p) { uint16_t v; memcpy(&v, p, 2); return v; }
inline uint16_t nvab_pgm_read_word(uint16_t a) { uint16_t v; memcpy(&v, (const void *)(uintptr_t)a, 2); return v; }
inline uint16_t nvab_pgm_read_word(unsigned long a) { uint16_t v; memcpy(&v, (const void *)(uintptr_t)a, 2); return v; }
inline uint16_t nvab_pgm_read_word(unsigned a) { uint16_t v; memcpy(&v, (const void *)(uintptr_t)a, 2); return v; }
inline uint16_t nvab_pgm_read_word(int a) { uint16_t v; memcpy(&v, (const void *)(uintptr_t)a, 2); return v; }
template <class T> struct nvab_pgm_dword { static uint32_t get(const T *p) { uint32_t v; memcpy(&v, (const void *)p, 4); return v; } };
template <class T> struct nvab_pgm_dword<T *> {
    static typename nvab_unconst<T>::type *get(T *const *p) { return (typename nvab_unconst<T>::type *)*p; }
};
template <class T> struct nvab_pgm_dword<T *const> {
    static typename nvab_unconst<T>::type *get(T *const *p) { return (typename nvab_unconst<T>::type *)*p; }
};
template <class T> inline auto nvab_pgm_read_dword(const T *p) -> decltype(nvab_pgm_dword<T>::get(p)) { return nvab_pgm_dword<T>::get(p); }
inline uint32_t nvab_pgm_read_dword(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
// an element of a pointer table comes back typed (`const uint8_t *p = pgm_read_ptr(&table[i])` compiles, as
// with avr-gcc -fpermissive); anything else as void *
template <class T> struct nvab_pgm_ptr { static void *get(const T *p) { void *v; memcpy(&v, (const void *)p, sizeof v); return v; } };
template <class T> struct nvab_pgm_ptr<T *> {
    static typename nvab_unconst<T>::type *get(T *const *p) { return (typename nvab_unconst<T>::type *)*p; }
};
template <class T> struct nvab_pgm_ptr<T *const> {
    static typename nvab_unconst<T>::type *get(T *const *p) { return (typename nvab_unconst<T>::type *)*p; }
};
template <class T> inline auto nvab_pgm_read_ptr(const T *p) -> decltype(nvab_pgm_ptr<T>::get(p)) { return nvab_pgm_ptr<T>::get(p); }
inline void *nvab_pgm_read_ptr(const void *p) { void *v; memcpy(&v, p, sizeof v); return v; }
inline void *nvab_pgm_read_ptr(uintptr_t a) { void *v; memcpy(&v, (const void *)a, sizeof v); return v; }
inline void *nvab_pgm_read_ptr(int a) { return nvab_pgm_read_ptr((uintptr_t)(unsigned)a); }
inline void *nvab_pgm_read_ptr(unsigned a) { return nvab_pgm_read_ptr((uintptr_t)a); }
inline void *nvab_pgm_read_ptr(uint16_t a) { return nvab_pgm_read_ptr((uintptr_t)a); }
#define pgm_read_word(addr) nvab_pgm_read_word(addr)
#define pgm_read_dword(addr) nvab_pgm_read_dword(addr)
#define pgm_read_ptr(addr) nvab_pgm_read_ptr(addr)
#else
#define pgm_read_word(addr) (*(const uint16_t *)(addr))
#define pgm_read_dword(addr) (*(const uint32_t *)(addr))
#define pgm_read_ptr(addr) (*(void *const *)(addr))
#endif
#define pgm_read_word_near(addr) pgm_read_word(addr)
#define pgm_read_word_far(addr) pgm_read_word(addr)
#define pgm_read_dword_near(addr) pgm_read_dword(addr)
#define pgm_read_dword_far(addr) pgm_read_dword(addr)
#define pgm_read_ptr_near(addr) pgm_read_ptr(addr)
#define pgm_read_ptr_far(addr) pgm_read_ptr(addr)

#define memcpy_P memcpy
#define memcpy_PF(d, a, n) memcpy(d, (const void *)(uintptr_t)(a), n)
#define memcmp_P memcmp
#define memchr_P memchr
#define strcpy_P strcpy
#define strncpy_P strncpy
#define strcat_P strcat
#define strncat_P strncat
#define strcmp_P strcmp
#define strncmp_P strncmp
#define strcasecmp_P strcasecmp
#define strlen_P strlen
#define strnlen_P strnlen
#define strstr_P strstr
#define strchr_P strchr
#define strrchr_P strrchr
#define sprintf_P sprintf
#define snprintf_P snprintf
#define vsnprintf_P vsnprintf
#define printf_P printf
