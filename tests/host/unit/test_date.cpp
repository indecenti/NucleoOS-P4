// nv_date_rel: the strings the Terminal's `date -d` understands (the agent's date arithmetic).
#include "check.h"
#include <cstdlib>
#include <cstring>
#include <ctime>
extern "C" {
#include "nv_date_rel.h"
}

static time_t at(const char *s, time_t base)
{
    time_t t = (time_t)-12345;
    return nv_date_rel(s, base, &t) ? t : (time_t)-12345;
}

static bool ymd(time_t t, int y, int m, int d)
{
    struct tm tm;
    localtime_r(&t, &tm);
    return tm.tm_year + 1900 == y && tm.tm_mon + 1 == m && tm.tm_mday == d;
}

int main()
{
    setenv("TZ", "UTC0", 1);
    tzset();
    const time_t base = 1791144000;                 // Sun 2026-10-04 20:00:00 UTC
    const time_t day = 86400;

    CHECK(at("now", base) == base && at("today", base) == base && at("  now  ", base) == base);
    CHECK(at("tomorrow", base) == base + day && at("Yesterday", base) == base - day);
    CHECK(at("@1700000000", base) == 1700000000 && at("@0", base) == 0);
    CHECK(at("+10 days", base) == base + 10 * day && at("10 days", base) == base + 10 * day);
    CHECK(at("-2 hours", base) == base - 7200 && at("- 2 hours", base) == base - 7200);
    CHECK(at("3 weeks ago", base) == base - 21 * day && at("1 day ago", base) == base - day && at("day ago", base) == base - day);
    CHECK(at("next week", base) == base + 7 * day && at("last week", base) == base - 7 * day);
    CHECK(at("90 minutes", base) == base + 5400 && at("30 sec", base) == base + 30 && at("1 fortnight", base) == base + 14 * day);
    CHECK(at("+1 DAY", base) == base + day);

    // the question that went wrong: Sunday + 10 days is a Wednesday
    struct tm tm;
    const time_t t10 = at("+10 days", base);
    localtime_r(&t10, &tm);
    CHECK(tm.tm_wday == 3 && ymd(t10, 2026, 10, 14));

    // months and years move the calendar, as GNU date (31 Jan + 1 month = 3 March in a common year)
    CHECK(ymd(at("+1 month", base), 2026, 11, 4) && ymd(at("2 months ago", base), 2026, 8, 4));
    CHECK(ymd(at("next year", base), 2027, 10, 4) && ymd(at("-1 year", base), 2025, 10, 4));
    CHECK(ymd(at("+1 month", 1801396800 /* 2027-01-31 12:00 UTC */), 2027, 3, 3));

    // anything else is refused, never guessed
    static const char *const bad[] = { "", "   ", "soon", "10", "+", "@", "@12x", "10 lightyears", "tomorrow please",
        "next", "3 days ahead", "-5000000 days", "next tomorrow", nullptr };
    for (int i = 0; bad[i]; i++) {
        const bool refused = at(bad[i], base) == (time_t)-12345;
        CHECK(refused);
        if (!refused) std::fprintf(stderr, "  accepted [%s]\n", bad[i]);
    }
    CHECK(!nv_date_rel(nullptr, base, nullptr) && !nv_date_rel("now", base, nullptr));
    return TEST_DONE("date");
}
