// nv_date_rel — the date strings `date -d` understands (GNU style), as a pure function the host tests
// compile: "@1700000000", "now", "today", "tomorrow", "yesterday", "+10 days", "10 days", "-2 hours",
// "3 weeks ago", "1 day ago", "next week", "last month" (months and years by calendar, in local time).
// Absolute dates too: "2026-12-25", "2026/12/25 14:30", "2026-12-25T14:30:05Z" (UTC), "December 25 2026",
// "25 Dec 2026", optionally followed by a relative part ("2026-12-25 +10 days").
#pragma once
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// The time `s` names, relative to `base`. False (and *out untouched) for anything it does not know.
bool nv_date_rel(const char *s, time_t base, time_t *out);

#ifdef __cplusplus
}
#endif
