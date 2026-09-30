// avr/power.h — power-reduction switches are no-ops. Part of ports/arduboy.
#pragma once
#define power_all_enable() do {} while (0)
#define power_all_disable() do {} while (0)
#define power_adc_enable() do {} while (0)
#define power_adc_disable() do {} while (0)
#define power_usart0_enable() do {} while (0)
#define power_usart0_disable() do {} while (0)
#define power_usart1_enable() do {} while (0)
#define power_usart1_disable() do {} while (0)
#define power_spi_enable() do {} while (0)
#define power_spi_disable() do {} while (0)
#define power_twi_enable() do {} while (0)
#define power_twi_disable() do {} while (0)
#define power_timer0_enable() do {} while (0)
#define power_timer0_disable() do {} while (0)
#define power_timer1_enable() do {} while (0)
#define power_timer1_disable() do {} while (0)
#define power_timer2_enable() do {} while (0)
#define power_timer2_disable() do {} while (0)
#define power_timer3_enable() do {} while (0)
#define power_timer3_disable() do {} while (0)
#define power_timer4_enable() do {} while (0)
#define power_timer4_disable() do {} while (0)
#define power_usb_enable() do {} while (0)
#define power_usb_disable() do {} while (0)
#define clock_prescale_set(x) do {} while (0)
#define clock_div_1 0
#define clock_div_2 1
#define clock_div_4 2
#define clock_div_8 3
