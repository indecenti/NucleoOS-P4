// nv_seclog — security event ring. See nv_seclog.h.
#include "nv_seclog.h"
#include "nv_log.h"
#include "nv_mem_attr.h"   // NV_PSRAM_BSS: the ring is task-context only

#include "freertos/FreeRTOS.h"
#include "esp_timer.h"

#include <cstdio>
#include <cstring>
#include <ctime>

static const char *TAG = "sec";

namespace {

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
NV_PSRAM_BSS nv_sec_entry_t s_ring[NV_SECLOG_MAX];   // ~1.6 KB, off internal RAM (CI budget)
int s_head = 0;    // next slot to write
int s_count = 0;
volatile uint32_t s_gen = 0;

constexpr const char *kIds[NV_SEC_EVENT_COUNT] = {
    "pair_wrong", "pair_locked", "pair_ok", "session_revoked",
    "firmware_refused", "app_refused", "settings_encrypted", "settings_plaintext",
    "unlock_locked",
};

}  // namespace

void nv_seclog_add(nv_sec_event_t code, const char *detail) {
    if ((int)code < 0 || code >= NV_SEC_EVENT_COUNT) return;
    nv_sec_entry_t e = {};
    const time_t now = time(nullptr);
    e.unix_time = now > 1700000000 ? (uint32_t)now : 0;   // before SNTP/RTC the clock reads 1970
    e.uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    e.code = (uint8_t)code;
    snprintf(e.detail, sizeof e.detail, "%s", detail ? detail : "");
    portENTER_CRITICAL_SAFE(&s_mux);
    s_ring[s_head] = e;
    s_head = (s_head + 1) % NV_SECLOG_MAX;
    if (s_count < NV_SECLOG_MAX) s_count++;
    s_gen = s_gen + 1;
    portEXIT_CRITICAL_SAFE(&s_mux);
    NV_LOGW(TAG, "%s %s", kIds[code], e.detail);
}

int nv_seclog_count(void) { return s_count; }

bool nv_seclog_get(int index, nv_sec_entry_t *out) {
    if (!out) return false;
    bool ok = false;
    portENTER_CRITICAL_SAFE(&s_mux);
    if (index >= 0 && index < s_count) {
        *out = s_ring[(s_head - 1 - index + NV_SECLOG_MAX) % NV_SECLOG_MAX];
        ok = true;
    }
    portEXIT_CRITICAL_SAFE(&s_mux);
    return ok;
}

uint32_t nv_seclog_generation(void) { return s_gen; }

const char *nv_seclog_code_id(nv_sec_event_t code) {
    return ((int)code >= 0 && code < NV_SEC_EVENT_COUNT) ? kIds[code] : "unknown";
}
