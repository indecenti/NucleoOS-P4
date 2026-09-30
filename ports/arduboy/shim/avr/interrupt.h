// avr/interrupt.h — no interrupts on NucleoOS: cli/sei do nothing; an ISR() body compiles into an
// unused function. Part of ports/arduboy.
#pragma once
#define cli() do {} while (0)
#define sei() do {} while (0)
#define ISR_NAKED
#define ISR_BLOCK
#define ISR_NOBLOCK
#define ISR(vector, ...) extern "C" void vector##_nvab_unused(void); extern "C" void vector##_nvab_unused(void)
#define EMPTY_INTERRUPT(vector) extern "C" void vector##_nvab_unused(void) {}
#define reti() do {} while (0)
