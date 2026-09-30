// avr/sleep.h — sleeping the CPU becomes a short host sleep. Part of ports/arduboy.
#pragma once
#ifdef __cplusplus
extern "C" void nvab_idle(void);
#else
void nvab_idle(void);
#endif
#define SLEEP_MODE_IDLE 0
#define SLEEP_MODE_ADC 1
#define SLEEP_MODE_PWR_DOWN 2
#define SLEEP_MODE_PWR_SAVE 3
#define SLEEP_MODE_STANDBY 6
#define SLEEP_MODE_EXT_STANDBY 7
#define set_sleep_mode(m) do {} while (0)
#define sleep_enable() do {} while (0)
#define sleep_disable() do {} while (0)
#define sleep_cpu() nvab_idle()
#define sleep_mode() nvab_idle()
#define sleep_bod_disable() do {} while (0)
