// avr/eeprom.h — the 1 KB EEPROM lives in app memory and is saved to the app's SD folder
// (nv_save "eeprom.bin") shortly after the last write and when the app closes. Addresses are
// taken modulo 1024 (an EEMEM variable's address lands somewhere in it). Part of ports/arduboy.
#pragma once
#include <stdint.h>
#include <stddef.h>
#define E2END 0x3FF
#define EEMEM
#ifdef __cplusplus
extern "C" {
#endif
uint8_t nvab_ee_read(uint16_t addr);
void nvab_ee_write(uint16_t addr, uint8_t v);
uint8_t eeprom_read_byte(const uint8_t *p);
uint16_t eeprom_read_word(const uint16_t *p);
uint32_t eeprom_read_dword(const uint32_t *p);
float eeprom_read_float(const float *p);
void eeprom_read_block(void *dst, const void *src, size_t n);
void eeprom_write_byte(uint8_t *p, uint8_t v);
void eeprom_write_word(uint16_t *p, uint16_t v);
void eeprom_write_dword(uint32_t *p, uint32_t v);
void eeprom_write_float(float *p, float v);
void eeprom_write_block(const void *src, void *dst, size_t n);
void eeprom_update_byte(uint8_t *p, uint8_t v);
void eeprom_update_word(uint16_t *p, uint16_t v);
void eeprom_update_dword(uint32_t *p, uint32_t v);
void eeprom_update_float(float *p, float v);
void eeprom_update_block(const void *src, void *dst, size_t n);
#ifdef __cplusplus
}
#endif
#define eeprom_is_ready() 1
#define eeprom_busy_wait() do {} while (0)
