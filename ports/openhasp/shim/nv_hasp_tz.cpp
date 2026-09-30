/* nv_hasp_tz.cpp — local time for openHASP's clocks (label "%H:%M" templates, calendar, the
 * "time" field of statusupdate). wasi-libc has no time zones: localtime() is UTC. openHASP on the
 * ESP32 takes the zone from its time settings; here it comes from config.json in the app folder:
 *
 *     {"time": {"zone": "CET-1CEST,M3.5.0,M10.5.0/3"}}      (a POSIX TZ string)
 *
 * or, without it, a default picked from the OS language (it/de/fr/es -> Central European Time,
 * anything else -> UTC). The build links with -Wl,--wrap=localtime,--wrap=localtime_r, so every
 * localtime() call in openHASP and LVGL lands here.
 *
 * Supported TZ syntax: std offset [dst [offset] [,Mm.w.d[/time],Mm.w.d[/time]]], names as
 * letters or <...>. Julian-day rules (Jn / n) are not supported (they fall back to no DST).
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ArduinoJson.h"
#include "nucleo_sdk.h"

namespace {

struct Rule {
    int month, week, wday, secs; // Mm.w.d/time
};

struct Zone {
    long std_off = 0; // seconds WEST of UTC (POSIX sign), local = UTC - off
    long dst_off = 0;
    bool has_dst = false;
    Rule start{3, 5, 0, 7200}, end{10, 5, 0, 10800};
} g_zone;

const char* parse_name(const char* p)
{
    if(*p == '<') {
        while(*p && *p != '>') p++;
        return *p ? p + 1 : p;
    }
    while(isalpha((unsigned char)*p)) p++;
    return p;
}

const char* parse_time(const char* p, long* out, bool* ok)
{
    int sign = 1;
    if(*p == '+' || *p == '-') sign = (*p++ == '-') ? -1 : 1;
    if(!isdigit((unsigned char)*p)) {
        *ok = false;
        return p;
    }
    long h = strtol(p, (char**)&p, 10), m = 0, s = 0;
    if(*p == ':') m = strtol(p + 1, (char**)&p, 10);
    if(*p == ':') s = strtol(p + 1, (char**)&p, 10);
    *out = sign * (h * 3600 + m * 60 + s);
    *ok  = true;
    return p;
}

const char* parse_rule(const char* p, Rule* r, bool* ok)
{
    *ok = false;
    if(*p != 'M') return p;
    r->month = (int)strtol(p + 1, (char**)&p, 10);
    if(*p++ != '.') return p;
    r->week = (int)strtol(p, (char**)&p, 10);
    if(*p++ != '.') return p;
    r->wday = (int)strtol(p, (char**)&p, 10);
    r->secs = 7200;
    if(*p == '/') {
        long t;
        bool tok;
        p = parse_time(p + 1, &t, &tok);
        if(!tok) return p;
        r->secs = (int)t;
    }
    *ok = r->month >= 1 && r->month <= 12 && r->week >= 1 && r->week <= 5 && r->wday >= 0 && r->wday <= 6;
    return p;
}

bool parse_tz(const char* tz, Zone* z)
{
    Zone n;
    bool ok;
    const char* p = parse_name(tz);
    if(p == tz) return false;
    p = parse_time(p, &n.std_off, &ok);
    if(!ok) return false;
    if(*p) {
        const char* q = parse_name(p);
        if(q != p) {
            n.has_dst = true;
            n.dst_off = n.std_off - 3600;
            p         = q;
            if(*p && *p != ',') {
                p = parse_time(p, &n.dst_off, &ok);
                if(!ok) return false;
            }
            if(*p == ',') {
                bool a, b;
                p = parse_rule(p + 1, &n.start, &a);
                if(*p == ',') p = parse_rule(p + 1, &n.end, &b);
                else b = false;
                if(!a || !b) n.has_dst = false; // unsupported rule syntax
            }
        }
    }
    *z = n;
    return true;
}

// days since 1970-01-01 of y-m-d (proleptic Gregorian; H. Hinnant's days_from_civil)
long days_from_civil(long y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const long era      = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe  = (unsigned)(y - era * 400);
    const unsigned doy  = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe  = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

// seconds since the epoch, in local wall time of the rule's zone, of the rule's moment in year y
long long rule_time(long y, const Rule& r)
{
    static const int mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    long first      = days_from_civil(y, (unsigned)r.month, 1);
    int wday_first  = (int)((first % 7 + 11) % 7); // 1970-01-01 was a Thursday (4)
    int day         = 1 + (r.wday - wday_first + 7) % 7 + (r.week - 1) * 7;
    int dim         = mdays[r.month - 1] + (r.month == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0));
    while(day > dim) day -= 7;
    return (long long)(first + day - 1) * 86400 + r.secs;
}

// seconds WEST of UTC in effect at UTC time t
long offset_at(time_t t, int* isdst)
{
    *isdst = 0;
    if(!g_zone.has_dst) return g_zone.std_off;
    struct tm g;
    time_t ls = t - g_zone.std_off;
    gmtime_r(&ls, &g);
    long y           = g.tm_year + 1900;
    long long start  = rule_time(y, g_zone.start) + g_zone.std_off; // UTC
    long long end    = rule_time(y, g_zone.end) + g_zone.dst_off;   // UTC
    bool dst         = start < end ? (t >= start && t < end) : !(t >= end && t < start);
    *isdst           = dst;
    return dst ? g_zone.dst_off : g_zone.std_off;
}

} // namespace

extern "C" struct tm* __wrap_localtime_r(const time_t* t, struct tm* out)
{
    int isdst;
    long off    = offset_at(*t, &isdst);
    time_t loc  = *t - off;
    gmtime_r(&loc, out);
    out->tm_isdst = isdst;
    return out;
}

extern "C" struct tm* __wrap_localtime(const time_t* t)
{
    static struct tm tmv;
    return __wrap_localtime_r(t, &tmv);
}

extern "C" void nv_hasp_tz_init(void)
{
    char zone[96] = {0};
    FILE* f       = fopen("config.json", "rb");
    if(f) {
        static char buf[16384];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = '\0';
        StaticJsonDocument<64> filter;
        filter["time"]["zone"] = true;
        DynamicJsonDocument doc(512);
        if(!deserializeJson(doc, (const char*)buf, DeserializationOption::Filter(filter))) {
            const char* z = doc["time"]["zone"] | "";
            strncpy(zone, z, sizeof(zone) - 1);
        }
    }
    if(!zone[0]) {
        char lang[8] = {0};
        nv_lang(lang, sizeof(lang));
        if(!strncmp(lang, "it", 2) || !strncmp(lang, "de", 2) || !strncmp(lang, "fr", 2) || !strncmp(lang, "es", 2))
            strcpy(zone, "CET-1CEST,M3.5.0,M10.5.0/3");
        else
            strcpy(zone, "UTC0");
    }
    if(!parse_tz(zone, &g_zone)) parse_tz("UTC0", &g_zone);
}
