// nv_time — system time service for NucleoOS Anima.
//
// Owns the wall clock: sets the timezone, seeds the clock from the firmware build time (so the
// header never shows 1970 before a sync), and runs SNTP against pool.ntp.org. SNTP auto-syncs
// the moment the device has network (C6 Wi-Fi or Ethernet) and quietly retries until then, so
// the header time becomes authoritative automatically once online. 12/24h is a persisted user
// preference. Formatting goes through strftime on the active local time.
#pragma once
#include <time.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Set TZ, seed the clock from build time, and start SNTP (non-blocking; syncs when online).
void nv_time_init(void);

// The network just came up (Wi-Fi got an IP): nudge SNTP to sync now instead of waiting for its
// next poll. Idempotent and safe to call before SNTP init.
void nv_time_notify_online(void);

// True once the first NTP sync has landed (the clock is now authoritative).
bool nv_time_is_synced(void);

// Fill `out` with the current local broken-down time.
void nv_time_now(struct tm *out);

// strftime the current local time with `fmt` into `out` (size `n`). Returns chars written.
size_t nv_time_format(char *out, size_t n, const char *fmt);

// 12/24-hour display preference (persisted; default 24h).
void nv_time_set_24h(bool on);
bool nv_time_is_24h(void);

// Time zone preference: a small named-city table of POSIX TZ rules (DST-correct).
// set applies live (setenv+tzset — every later strftime/localtime uses it) and persists
// the index ("tz_ix"). Out-of-range indices are ignored/clamped. Default: Rome/Berlin CET.
int         nv_time_tz_count(void);
const char *nv_time_tz_name(int index);   // display label ("Rome / Berlin / Paris"), "" if OOR
int         nv_time_get_tz(void);         // active table index
void        nv_time_set_tz(int index);

#ifdef __cplusplus
}
#endif
