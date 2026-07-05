// nv_time — system time service. See nv_time.h.
#include "nv_time.h"
#include "nv_log.h"
#include "nv_config.h"

#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"

#include <cstring>
#include <cstdio>
#include <sys/time.h>

static const char *TAG = "time";

namespace {

// Named time-zone table (display label + POSIX TZ rule with DST). Settings > Date & time
// picks by index; the index is persisted ("tz_ix") and applied live via setenv+tzset.
struct TzEntry { const char *name; const char *tz; };
constexpr TzEntry kTz[] = {
    {"UTC",                  "UTC0"},
    {"London / Lisbon",      "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Rome / Berlin / Paris","CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Athens / Helsinki",    "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Moscow",               "MSK-3"},
    {"Dubai",                "<+04>-4"},
    {"New Delhi",            "IST-5:30"},
    {"Bangkok",              "<+07>-7"},
    {"Beijing / Singapore",  "CST-8"},
    {"Tokyo / Seoul",        "JST-9"},
    {"Sydney",               "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"New York",             "EST5EDT,M3.2.0,M11.1.0"},
    {"Chicago",              "CST6CDT,M3.2.0,M11.1.0"},
    {"Denver",               "MST7MDT,M3.2.0,M11.1.0"},
    {"Los Angeles",          "PST8PDT,M3.2.0,M11.1.0"},
};
constexpr int kTzCount   = sizeof(kTz) / sizeof(kTz[0]);
constexpr int kTzDefIdx  = 2;   // Rome / Berlin / Paris (previous hardcoded default)

int s_tz = kTzDefIdx;

bool s_synced = false;
bool s_24h = true;   // cached "clk24": the getter runs on 1 Hz UI timers — must stay NVS-free

// Firmware build instant (__DATE__ "Mmm d yyyy" + __TIME__ "hh:mm:ss") -> epoch, used to seed
// the clock so the header shows a plausible time before the first NTP sync.
time_t build_epoch(void) {
    static const char *kMonths = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = {0};
    int day = 1, year = 2025, hh = 0, mm = 0, ss = 0;
    sscanf(__DATE__, "%3s %d %d", mon, &day, &year);
    sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss);
    const char *p = strstr(kMonths, mon);
    struct tm t = {};
    t.tm_year = year - 1900;
    t.tm_mon  = p ? (int)((p - kMonths) / 3) : 0;
    t.tm_mday = day;
    t.tm_hour = hh; t.tm_min = mm; t.tm_sec = ss;
    t.tm_isdst = -1;
    return mktime(&t);
}

void on_sync(struct timeval *) {
    s_synced = true;
    NV_LOGI(TAG, "clock synced via NTP");
}

}  // namespace

void nv_time_init(void) {
    s_tz = nv_config_get_int("tz_ix", kTzDefIdx);
    if (s_tz < 0 || s_tz >= kTzCount) s_tz = kTzDefIdx;   // clamp stale/corrupt values
    setenv("TZ", kTz[s_tz].tz, 1);
    tzset();
    s_24h = nv_config_get_bool("clk24", true);

    // Seed from build time if the RTC is unset (year < 2020) so the header isn't stuck at 1970.
    time_t now = time(nullptr);
    struct tm cur;
    localtime_r(&now, &cur);
    if (cur.tm_year + 1900 < 2020) {
        struct timeval bt = { build_epoch(), 0 };
        settimeofday(&bt, nullptr);
    }

    // SNTP: non-blocking, smooth-adjusts, auto-retries. esp_netif_init is idempotent; SNTP binds
    // to the default interface once the network (C6 Wi-Fi / Ethernet) is up.
    esp_netif_init();
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.start   = true;
    cfg.sync_cb = on_sync;
    if (esp_netif_sntp_init(&cfg) != ESP_OK)
        NV_LOGW(TAG, "SNTP init failed (will show local clock only)");

    char b[32];
    nv_time_format(b, sizeof(b), "%Y-%m-%d %H:%M");
    NV_LOGI(TAG, "time service ready (TZ=%s, clock=%s)", kTz[s_tz].tz, b);
}

void nv_time_notify_online(void) {
    // SNTP was started at init and binds to whatever interface is up; restarting forces an
    // immediate poll now that we actually have connectivity. Harmless if it isn't running yet.
    esp_sntp_restart();
}

bool nv_time_is_synced(void) { return s_synced; }

void nv_time_now(struct tm *out) {
    if (!out) return;
    time_t t = time(nullptr);
    localtime_r(&t, out);
}

size_t nv_time_format(char *out, size_t n, const char *fmt) {
    time_t t = time(nullptr);
    struct tm lt;
    localtime_r(&t, &lt);
    return strftime(out, n, fmt, &lt);
}

void nv_time_set_24h(bool on) { s_24h = on; nv_config_set_bool("clk24", on); }
bool nv_time_is_24h(void)     { return s_24h; }   // RAM read (status bar polls this at 1 Hz)

int nv_time_tz_count(void) { return kTzCount; }
const char *nv_time_tz_name(int index) {
    return (index >= 0 && index < kTzCount) ? kTz[index].name : "";
}
int nv_time_get_tz(void) { return s_tz; }
void nv_time_set_tz(int index) {
    if (index < 0 || index >= kTzCount || index == s_tz) return;
    s_tz = index;
    setenv("TZ", kTz[s_tz].tz, 1);   // live: the status-bar clock re-formats on its next tick
    tzset();
    nv_config_set_int("tz_ix", index);
    NV_LOGI(TAG, "time zone -> %s (%s)", kTz[s_tz].name, kTz[s_tz].tz);
}
