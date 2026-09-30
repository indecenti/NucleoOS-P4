// nv_auth — device side: session store in nv_config, pairing code, SHA-256, RNG. The decisions
// themselves (parsing, lockouts, list edits) are nv_auth_core, shared with the host tests.
#include "nv_auth.h"
#include "nv_auth_core.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"

#include "nv_config.h"
#include "nv_log.h"
#include "nv_seclog.h"
#include "nv_mem_attr.h"   // NV_PSRAM_BSS: the session table is task-context only

namespace core = nv_auth_core;

namespace {

const char *TAG = "auth";
constexpr const char *kKey = "web_sess";     // nv_config key (NVS: <= 15 chars)
constexpr size_t kStoreCap = 2048;           // 12 × ~110 B fits with room

SemaphoreHandle_t s_mtx = nullptr;           // guards s_sess/s_n/s_pin
SemaphoreHandle_t s_persist = nullptr;       // one NVS write at a time, in order
NV_PSRAM_BSS core::Session s_sess[core::kMaxSessions];
int s_n = 0;
NV_PSRAM_BSS core::Pin s_pin;

int64_t now_ms() { return esp_timer_get_time() / 1000; }

void sha256(const char *s, uint8_t out[core::kHashLen]) {
    mbedtls_sha256(reinterpret_cast<const unsigned char *>(s), strlen(s), out, 0);
}

struct Lock {
    Lock() { xSemaphoreTake(s_mtx, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(s_mtx); }
};

// Snapshot the list under the lock and write it outside it (NVS writes are slow).
void persist() {
    char *buf = static_cast<char *>(heap_caps_malloc(kStoreCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buf) { NV_LOGE(TAG, "persist: oom"); return; }
    xSemaphoreTake(s_persist, portMAX_DELAY);
    size_t len;
    {
        Lock l;
        len = core::sessions_format(s_sess, s_n, buf, kStoreCap);
    }
    if (len) nv_config_set_str(kKey, buf);
    else NV_LOGE(TAG, "persist: list does not fit");
    xSemaphoreGive(s_persist);
    heap_caps_free(buf);
}

uint32_t random_below_million() {
    // Rejection sampling: 4 294 000 000 is the largest multiple of 10^6 below 2^32.
    for (;;) {
        const uint32_t r = esp_random();
        if (r < 4294000000u) return r;
    }
}

}  // namespace

extern "C" {

void nv_auth_init(void) {
    if (s_mtx) return;
    s_mtx = xSemaphoreCreateMutex();
    s_persist = xSemaphoreCreateMutex();
    core::pin_reset(s_pin);
    char *buf = static_cast<char *>(heap_caps_malloc(kStoreCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buf) { NV_LOGE(TAG, "init: oom"); return; }
    nv_config_get_str(kKey, "", buf, kStoreCap);
    s_n = core::sessions_parse(buf, s_sess, core::kMaxSessions);
    heap_caps_free(buf);
    NV_LOGI(TAG, "%d paired web client(s)", s_n);
}

bool nv_auth_check_token(const char *token) {
    if (!s_mtx || !token || !core::is_token(token, strlen(token))) return false;
    uint8_t h[core::kHashLen];
    sha256(token, h);
    Lock l;
    return core::sessions_find(s_sess, s_n, h) >= 0;
}

bool nv_auth_check_headers(const char *authorization, const char *cookie) {
    char tok[core::kTokenHex + 1];
    return core::token_from_headers(authorization, cookie, tok) && nv_auth_check_token(tok);
}

void nv_auth_pair_request(const char *who) {
    if (!s_mtx) return;
    char code[core::kPinLen + 1] = "";
    bool fresh = false;
    int left_s = 0;
    {
        Lock l;
        const int64_t now = now_ms();
        fresh = core::pin_issue(s_pin, now, random_below_million(), who);
        if (core::pin_active(s_pin, now)) {
            memcpy(code, s_pin.code, sizeof code);
            left_s = (int)((s_pin.expires_ms - now) / 1000);
        }
    }
    if (!code[0]) return;
    if (fresh) NV_LOGI(TAG, "pairing requested by %s", who ? who : "?");
    // The code goes to the screen (SystemUI polls nv_auth_pair_pending) and to the serial console
    // ONLY: plain printf, never NV_LOG, since the log ring is readable over the web API. Repeated
    // for every request while it is valid, so a tool reading the console can pick it up late.
    printf("\n[nv_auth] pairing code %s (valid %d s, requested by %s)\n", code, left_s, who ? who : "?");
}

nv_auth_pair_result_t nv_auth_pair_finish(const char *code, const char *name, char *token_out) {
    if (!s_mtx) return NV_AUTH_PAIR_NO_CODE;
    core::PinResult r;
    char who[core::kWhoMax];
    {
        Lock l;
        snprintf(who, sizeof who, "%s", s_pin.who);   // pin_try may burn the code (and its label)
        r = core::pin_try(s_pin, now_ms(), code);
    }
    switch (r) {
    case core::PinResult::Wrong:
        nv_seclog_add(NV_SEC_PAIR_WRONG, who);
        return NV_AUTH_PAIR_WRONG;
    case core::PinResult::Locked:
        nv_seclog_add(NV_SEC_PAIR_LOCKED, who);
        return NV_AUTH_PAIR_LOCKED;
    case core::PinResult::NoPin:  return NV_AUTH_PAIR_NO_CODE;
    case core::PinResult::Ok:     break;
    }
    uint8_t raw[core::kTokenHex / 2];
    esp_fill_random(raw, sizeof raw);
    static const char kHex[] = "0123456789abcdef";
    char tok[core::kTokenHex + 1];
    for (size_t i = 0; i < sizeof raw; i++) {
        tok[2 * i] = kHex[raw[i] >> 4];
        tok[2 * i + 1] = kHex[raw[i] & 15];
    }
    tok[core::kTokenHex] = '\0';
    core::Session e = {};
    sha256(tok, e.hash);
    const time_t t = time(nullptr);
    e.created = t > 1600000000 ? (uint32_t)t : 0;
    core::sanitize_name(name, e.name);
    {
        Lock l;
        s_n = core::sessions_add(s_sess, s_n, core::kMaxSessions, e);
    }
    persist();
    NV_LOGI(TAG, "paired \"%s\" (%d session(s))", e.name, s_n);
    nv_seclog_add(NV_SEC_PAIR_OK, e.name);
    if (token_out) memcpy(token_out, tok, sizeof tok);
    return NV_AUTH_PAIR_OK;
}

bool nv_auth_pair_pending(char *code, char *who, uint32_t *secs_left) {
    if (!s_mtx) return false;
    Lock l;
    const int64_t now = now_ms();
    if (!core::pin_active(s_pin, now)) return false;
    if (code) memcpy(code, s_pin.code, core::kPinLen + 1);
    if (who) snprintf(who, NV_AUTH_WHO_MAX, "%s", s_pin.who);
    if (secs_left) *secs_left = (uint32_t)((s_pin.expires_ms - now + 999) / 1000);
    return true;
}

void nv_auth_pair_deny(void) {
    if (!s_mtx) return;
    Lock l;
    core::pin_deny(s_pin, now_ms());
}

int nv_auth_session_count(void) {
    if (!s_mtx) return 0;
    Lock l;
    return s_n;
}

bool nv_auth_session_get(int index, nv_auth_session_t *out) {
    if (!s_mtx || !out) return false;
    Lock l;
    if (index < 0 || index >= s_n) return false;
    snprintf(out->name, sizeof out->name, "%s", s_sess[index].name);
    out->created = s_sess[index].created;
    return true;
}

void nv_auth_session_revoke(int index) {
    if (!s_mtx) return;
    char name[core::kNameMax] = "";
    {
        Lock l;
        if (index >= 0 && index < s_n) snprintf(name, sizeof name, "%s", s_sess[index].name);
        s_n = core::sessions_remove(s_sess, s_n, index);
    }
    persist();
    if (name[0]) nv_seclog_add(NV_SEC_SESSION_REVOKED, name);
}

void nv_auth_session_revoke_all(void) {
    if (!s_mtx) return;
    {
        Lock l;
        s_n = 0;
    }
    persist();
    nv_seclog_add(NV_SEC_SESSION_REVOKED, "all");
}

}  // extern "C"
