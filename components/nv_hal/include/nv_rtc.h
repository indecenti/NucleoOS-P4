// nv_rtc — RX8130 hardware clock bridge for the system time service.
//
// The canonical wall clock lives in nv_time (nv_kernel): timezone, build-time seed, SNTP. This
// module adds the on-board RX8130 RTC (I2C 0x14) as the OFFLINE persistence layer:
//   * on boot, seed the system clock from the RTC (survives reboots with no network);
//   * once SNTP has corrected the clock, write the accurate time back into the RTC.
// SNTP always wins when online; the RTC just keeps time plausible offline.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Read the RX8130 into the system clock (if its contents are plausible), then arm a background
// watcher that persists the time back to the RTC after the first SNTP sync. Call once, AFTER
// nv_hal_init() (needs the shared I2C bus).
void nv_rtc_sync(void);

#ifdef __cplusplus
}
#endif
