// util/delay.h — busy delays become host delays. Part of ports/arduboy.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
void delay(unsigned long ms);
void delayMicroseconds(unsigned int us);
#ifdef __cplusplus
}
#endif
#define _delay_ms(ms) delay((unsigned long)(ms))
#define _delay_us(us) delayMicroseconds((unsigned int)(us))
