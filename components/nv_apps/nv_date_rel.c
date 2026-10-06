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

// Days since 1970-01-01 of a proleptic Gregorian date (Howard Hinnant's days_from_civil).
static long long days_from_civil(long long y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

static int month_of(const char *p, int *len)
{
    static const char *const M[] = { "january", "february", "march", "april", "may", "june", "july",
        "august", "september", "october", "november", "december" };
    for (int i = 0; i < 12; i++) {
        const size_t full = strlen(M[i]);
        if (!strncasecmp(p, M[i], full) && !isalpha((unsigned char)p[full])) { *len = (int)full; return i + 1; }
        if (!strncasecmp(p, M[i], 3) && !isalpha((unsigned char)p[3])) { *len = 3 + (p[3] == '.'); return i + 1; }
    }
    return 0;
}

static const char *num(const char *p, int digits_max, long *v)
{
    int k = 0;
    long x = 0;
    while (isdigit((unsigned char)p[k]) && k < digits_max) x = x * 10 + (p[k++] - '0');
    if (!k) return NULL;
    *v = x;
    return p + k;
}

// An absolute moment at the start of p: 2026-12-25, 2026/12/25, "2026-12-25 14:30[:05]", "2026-12-25T14:30Z",
// "December 25 2026", "Dec 25, 2026", "25 December 2026", "25th Dec 2026". *rest = what follows.
static bool absolute(const char *p, time_t *out, const char **rest)
{
    long y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    const char *q;
    int ml;
    if ((q = num(p, 4, &y)) && q - p == 4 && (*q == '-' || *q == '/')) {
        const char sep = *q;
        if (!(q = num(q + 1, 2, &mo)) || *q != sep || !(q = num(q + 1, 2, &d))) return false;
    } else if ((mo = month_of(p, &ml)) != 0) {                // December 25[th][,] 2026
        if (!(q = num(skip(p + ml), 2, &d))) return false;
        if (!strncasecmp(q, "st", 2) || !strncasecmp(q, "nd", 2) || !strncasecmp(q, "rd", 2) || !strncasecmp(q, "th", 2)) q += 2;
        if (*q == ',') q++;
        const char *yq = skip(q);
        if (!(q = num(yq, 4, &y)) || q - yq != 4) return false;
    } else if ((q = num(p, 2, &d))) {                          // 25[th] December 2026
        if (!strncasecmp(q, "st", 2) || !strncasecmp(q, "nd", 2) || !strncasecmp(q, "rd", 2) || !strncasecmp(q, "th", 2)) q += 2;
        if (!(mo = month_of(skip(q), &ml))) return false;
        const char *yq = skip(skip(q) + ml);
        if (*yq == ',') yq = skip(yq + 1);
        if (!(q = num(yq, 4, &y)) || q - yq != 4) return false;
    } else {
        return false;
    }
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return false;
    const char *t = (*q == 'T' || *q == ' ') ? q + 1 : NULL;
    if (t && isdigit((unsigned char)*t) && (t = num(t, 2, &h)) && *t == ':' && (t = num(t + 1, 2, &mi))) {
        if (*t == ':' && !(t = num(t + 1, 2, &se))) return false;
        q = t;
    }
    if (h > 23 || mi > 59 || se > 60) return false;
    if (*q == 'Z' || *q == 'z') {                              // UTC
        *out = (time_t)(days_from_civil(y, (unsigned)mo, (unsigned)d) * 86400LL + h * 3600 + mi * 60 + se);
        q++;
    } else {
        struct tm tm = { 0 };
        tm.tm_year = (int)y - 1900; tm.tm_mon = (int)mo - 1; tm.tm_mday = (int)d;
        tm.tm_hour = (int)h; tm.tm_min = (int)mi; tm.tm_sec = (int)se; tm.tm_isdst = -1;
        const time_t v = mktime(&tm);
        if (v == (time_t)-1) return false;
        *out = v;
    }
    if (*q && *q != ' ' && *q != '+' && *q != '-') return false;
    *rest = skip(q);
    return true;
}

static bool relative(const char *p, time_t base, time_t *out);

bool nv_date_rel(const char *s, time_t base, time_t *out)
{
    if (!s || !out) return false;
    const char *p = skip(s);
    time_t abs;
    const char *rest;
    if (absolute(p, &abs, &rest)) {                            // "2026-12-25", "2026-12-25 +10 days"
        if (!*rest) { *out = abs; return true; }
        return relative(rest, abs, out);
    }
    return relative(p, base, out);
}

static bool relative(const char *p, time_t base, time_t *out)
{
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
