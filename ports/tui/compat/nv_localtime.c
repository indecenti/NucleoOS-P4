// nv_localtime.c — local time for the NucleoOS full-screen ports (link with
// -Wl,--wrap=localtime,--wrap=localtime_r,--wrap=mktime).
//
// wasi-libc has no time zones: localtime() is gmtime() and mktime() is timegm(), so programs show
// UTC (sc-im's @now, the dates it reads from a workbook) while the status bar shows local time.
// The OS passes the local offset of the moment the program starts, DST included, as
// NUCLEO_UTC_OFFSET (seconds east of UTC): local time is UTC plus that offset, both ways, and
// tm_gmtoff carries it so programs that compute their own offset (sc-im) get it right too.
#define _GNU_SOURCE
#include <stdlib.h>
#include <time.h>

static long nv_utc_offset(void) {
    const char *s = getenv("NUCLEO_UTC_OFFSET");
    return s ? atol(s) : 0;
}

struct tm *__wrap_localtime_r(const time_t *t, struct tm *r) {
    const long off = nv_utc_offset();
    const time_t x = *t + (time_t)off;
    struct tm *p = gmtime_r(&x, r);
    if (p) {
        p->tm_isdst = 0;
        p->tm_gmtoff = off;
        p->tm_zone = off ? "LT" : "UTC";
    }
    return p;
}

struct tm *__wrap_localtime(const time_t *t) {
    static struct tm r;
    return __wrap_localtime_r(t, &r);
}

time_t __wrap_mktime(struct tm *tm) {
    const time_t utc = timegm(tm);              // the fields read as UTC, normalised
    if (utc == (time_t)-1) return utc;
    const time_t t = utc - (time_t)nv_utc_offset();
    __wrap_localtime_r(&t, tm);                 // mktime normalises the fields, in local time
    return t;
}
