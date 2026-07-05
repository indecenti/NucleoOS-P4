// nv_rtc — RX8130 RTC bridge. See nv_rtc.h. Best-effort + defensive: a bad/garbage RTC never
// blocks boot or overrides SNTP; it only helps when offline.
#include "nv_rtc.h"
#include "nv_hal.h"     // nv_hal_i2c_bus()
#include "nv_time.h"    // nv_time_is_synced(), nv_time_now()
#include "nv_log.h"

#include "driver/i2c_master.h"
#include "esp_timer.h"

#include <time.h>
#include <sys/time.h>
#include <cstring>

static const char *TAG = "rtc";

namespace {

// RX8130CE: time block is contiguous from 0x10; FLAG register at 0x1E carries the VLF bit that
// means "oscillator stopped / data invalid".
constexpr uint8_t kAddr    = 0x14;
constexpr uint8_t kRegSec  = 0x10;   // SEC,MIN,HOUR,WEEK,DAY,MONTH,YEAR = 0x10..0x16 (BCD)
constexpr uint8_t kRegFlag = 0x1E;
constexpr uint8_t kFlagVLF = 0x02;

i2c_master_dev_handle_t s_dev = nullptr;
esp_timer_handle_t s_persist_timer = nullptr;
int s_persist_tries = 0;

uint8_t bcd2bin(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
uint8_t bin2bcd(int v)     { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

bool dev(void) {
    if (s_dev) return true;
    i2c_master_bus_handle_t bus = nv_hal_i2c_bus();
    if (!bus) return false;
    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address  = kAddr;
    cfg.scl_speed_hz    = 400000;
    return i2c_master_bus_add_device(bus, &cfg, &s_dev) == ESP_OK;
}
bool rd(uint8_t reg, uint8_t *buf, size_t n) {
    return dev() && i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 100) == ESP_OK;
}
bool wr(uint8_t reg, const uint8_t *data, size_t n) {
    if (!dev() || n + 1 > 8) return false;
    uint8_t tmp[8];
    tmp[0] = reg;
    memcpy(&tmp[1], data, n);
    return i2c_master_transmit(s_dev, tmp, n + 1, 100) == ESP_OK;
}

// Write the current localtime back to the RTC and clear the VLF flag.
void store_now(void) {
    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year < 120) return;                 // never persist a pre-2020 (unset) time
    uint8_t b[7] = {
        bin2bcd(tm.tm_sec), bin2bcd(tm.tm_min), bin2bcd(tm.tm_hour),
        (uint8_t)(1 << (tm.tm_wday & 7)),         // WEEK: one-hot day-of-week
        bin2bcd(tm.tm_mday), bin2bcd(tm.tm_mon + 1), bin2bcd(tm.tm_year - 100),
    };
    wr(kRegSec, b, sizeof(b));
    uint8_t flag = 0;
    if (rd(kRegFlag, &flag, 1)) { flag &= (uint8_t)~kFlagVLF; wr(kRegFlag, &flag, 1); }
    NV_LOGI(TAG, "persisted %02d:%02d to RX8130", tm.tm_hour, tm.tm_min);
}

// Poll for the first SNTP sync (a few times), then persist the accurate time and stop.
void persist_cb(void *) {
    if (nv_time_is_synced()) {
        store_now();
        esp_timer_stop(s_persist_timer);
        return;
    }
    if (++s_persist_tries >= 120) esp_timer_stop(s_persist_timer);   // give up after ~10 min
}

}  // namespace

void nv_rtc_sync(void) {
    uint8_t flag = 0;
    if (rd(kRegFlag, &flag, 1) && (flag & kFlagVLF)) {
        NV_LOGW(TAG, "RX8130 data loss (VLF) — clock will come from SNTP");
    } else {
        uint8_t b[7];
        if (rd(kRegSec, b, sizeof(b))) {
            struct tm tm = {};
            tm.tm_sec  = bcd2bin(b[0] & 0x7F);
            tm.tm_min  = bcd2bin(b[1] & 0x7F);
            tm.tm_hour = bcd2bin(b[2] & 0x3F);
            tm.tm_mday = bcd2bin(b[4] & 0x3F);
            tm.tm_mon  = bcd2bin(b[5] & 0x1F) - 1;
            tm.tm_year = 100 + bcd2bin(b[6]);
            tm.tm_isdst = -1;
            const bool ok = tm.tm_sec <= 59 && tm.tm_min <= 59 && tm.tm_hour <= 23 &&
                            tm.tm_mday >= 1 && tm.tm_mday <= 31 && tm.tm_mon >= 0 &&
                            tm.tm_mon <= 11 && tm.tm_year >= 120 && tm.tm_year <= 199;
            if (ok) {
                time_t e = mktime(&tm);
                struct timeval tv = { e, 0 };
                settimeofday(&tv, nullptr);
                NV_LOGI(TAG, "seeded clock from RX8130: %04d-%02d-%02d %02d:%02d",
                        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
            } else {
                NV_LOGW(TAG, "RX8130 time implausible — clock will come from SNTP");
            }
        } else {
            NV_LOGW(TAG, "RX8130 not responding on I2C");
        }
    }

    // Persist SNTP-corrected time back to the RTC once it lands (poll every 5s).
    const esp_timer_create_args_t a = { persist_cb, nullptr, ESP_TIMER_TASK, "rtcpersist", true };
    if (esp_timer_create(&a, &s_persist_timer) == ESP_OK)
        esp_timer_start_periodic(s_persist_timer, 5 * 1000 * 1000);
}
