// nv_date_rel — see nv_date_rel.h. Seconds, minutes, hours, days and weeks move by seconds; months and
// years move the calendar fields and let mktime() normalise them (31 January + 1 month = 3 March, as
// GNU date does).
#include "nv_date_rel.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *skip(const char *p)
{
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

// The unit word at p: its length in seconds (0 = month, -1 = year), or -2 = unknown. *len = chars used.
static long unit_of(const char *p, int *len)
{
    static const struct { const char *w; long sec; } U[] = {
        { "seconds", 1 }, { "second", 1 }, { "secs", 1 }, { "sec", 1 },
        { "minutes", 60 }, { "minute", 60 }, { "mins", 60 }, { "min", 60 },
        { "hours", 3600 }, { "hour", 3600 },
        { "days", 86400 }, { "day", 86400 },
        { "weeks", 604800 }, { "week", 604800 }, { "fortnight", 1209600 },
        { "months", 0 }, { "month", 0 },
        { "years", -1 }, { "year", -1 },
    };
    for (size_t i = 0; i < sizeof U / sizeof U[0]; i++) {
        const size_t n = strlen(U[i].w);
        if (!strncasecmp(p, U[i].w, n) && !isalpha((unsigned char)p[n])) { *len = (int)n; return U[i].sec; }
    }
    return -2;
}

static bool shift(time_t base, long n, long unit, time_t *out)
{
    if (unit > 0) { *out = base + (time_t)n * unit; return true; }
    struct tm tm;
    localtime_r(&base, &tm);
    if (unit == 0) tm.tm_mon += (int)n; else tm.tm_year += (int)n;
    tm.tm_isdst = -1;
    const time_t t = mktime(&tm);
    if (t == (time_t)-1) return false;
    *out = t;
    return true;
}

bool nv_date_rel(const char *s, time_t base, time_t *out)
{
    if (!s || !out) return false;
    const char *p = skip(s);
    if (*p == '@') {                                        // seconds since the epoch
        char *end;
        const long long v = strtoll(p + 1, &end, 10);
        if (end == p + 1 || *skip(end)) return false;
        *out = (time_t)v;
        return true;
    }
    static const struct { const char *w; long d; } WORD[] = {
        { "now", 0 }, { "today", 0 }, { "tomorrow", 1 }, { "yesterday", -1 }, };
    for (size_t i = 0; i < sizeof WORD / sizeof WORD[0]; i++) {
        const size_t n = strlen(WORD[i].w);
        if (!strncasecmp(p, WORD[i].w, n) && !*skip(p + n)) { *out = base + (time_t)WORD[i].d * 86400; return true; }
    }
    long sign = 1;                                          // "next week" / "last month"
    if (!strncasecmp(p, "next ", 5) || !strncasecmp(p, "last ", 5)) {
        sign = tolower((unsigned char)p[0]) == 'n' ? 1 : -1;
        int ul;
        const long unit = unit_of(skip(p + 5), &ul);
        if (unit == -2 || *skip(skip(p + 5) + ul)) return false;
        return shift(base, sign, unit, out);
    }
    if (*p == '+' || *p == '-') { if (*p == '-') sign = -1; p = skip(p + 1); }
    long n = 1;                                             // "day ago" = 1 day ago
    if (isdigit((unsigned char)*p)) {
        char *end;
        n = strtol(p, &end, 10);
        if (n < 0 || n > 1000000) return false;
        p = skip(end);
    }
    int ul;
    const long unit = unit_of(p, &ul);
    if (unit == -2) return false;
    p = skip(p + ul);
    if (!strncasecmp(p, "ago", 3) && !*skip(p + 3)) { sign = -sign; p = skip(p + 3); }
    if (*p) return false;
    return shift(base, sign * n, unit, out);
}
