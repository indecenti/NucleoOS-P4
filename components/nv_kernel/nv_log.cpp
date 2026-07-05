#include "nv_log.h"

#include <cstdio>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

namespace {

constexpr int kRingEntries = 256;
constexpr int kMsgMax = 160;
constexpr int kTagMax = 16;

struct Entry {
    int64_t ts_us;
    uint8_t level;
    char tag[kTagMax];
    char msg[kMsgMax];
};

Entry *s_ring = nullptr;
uint32_t s_head = 0;   // next write slot
uint32_t s_count = 0;  // retained entries
SemaphoreHandle_t s_mtx = nullptr;
nv_log_level_t s_min_level = NV_LOG_TRACE;

const char kLvl[] = {'E', 'W', 'I', 'D', 'T'};
inline char lvl_char(uint8_t l) { return (l <= NV_LOG_TRACE) ? kLvl[l] : '?'; }

}  // namespace

void nv_log_init(void) {
    if (s_ring) return;
    // Prefer PSRAM; fall back to internal if PSRAM isn't up yet.
    s_ring = static_cast<Entry *>(
        heap_caps_malloc(sizeof(Entry) * kRingEntries, MALLOC_CAP_SPIRAM));
    if (!s_ring) {
        s_ring = static_cast<Entry *>(
            heap_caps_malloc(sizeof(Entry) * kRingEntries, MALLOC_CAP_INTERNAL));
    }
    s_mtx = xSemaphoreCreateMutex();
    s_head = 0;
    s_count = 0;
}

void nv_log_set_level(nv_log_level_t level) { s_min_level = level; }

void nv_log_write(nv_log_level_t level, const char *tag, const char *fmt, ...) {
    if (level > s_min_level) return;

    char buf[kMsgMax];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    const int64_t ts = esp_timer_get_time();

    // Serial sink — immediate.
    printf("%c (%lld) %s: %s\n", lvl_char(level), (long long)(ts / 1000), tag, buf);

    // Ring sink — non-blocking (drop on contention rather than stall the caller).
    if (s_ring && s_mtx && xSemaphoreTake(s_mtx, 0) == pdTRUE) {
        Entry *e = &s_ring[s_head % kRingEntries];
        e->ts_us = ts;
        e->level = static_cast<uint8_t>(level);
        strlcpy(e->tag, tag, sizeof(e->tag));
        strlcpy(e->msg, buf, sizeof(e->msg));
        s_head++;
        if (s_count < kRingEntries) s_count++;
        xSemaphoreGive(s_mtx);
    }
}

void nv_log_dump(void) {
    if (!s_ring || !s_mtx) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    const uint32_t n = s_count;
    const uint32_t start = (s_head >= n) ? (s_head - n) : 0;
    printf("--- nv_log ring (%lu entries) ---\n", (unsigned long)n);
    for (uint32_t i = 0; i < n; i++) {
        const Entry *e = &s_ring[(start + i) % kRingEntries];
        printf("  %c %lld %s: %s\n", lvl_char(e->level), (long long)(e->ts_us / 1000),
               e->tag, e->msg);
    }
    xSemaphoreGive(s_mtx);
}

size_t nv_log_snapshot(char *dst, size_t cap) {
    if (!dst || !cap) return 0;
    dst[0] = '\0';
    if (!s_ring || !s_mtx) return 0;
    size_t o = 0;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    const uint32_t n = s_count;
    const uint32_t start = (s_head >= n) ? (s_head - n) : 0;
    for (uint32_t i = 0; i < n && o + 1 < cap; i++) {
        const Entry *e = &s_ring[(start + i) % kRingEntries];
        const int k = snprintf(dst + o, cap - o, "%c %8lld %s: %s\n",
                               lvl_char(e->level), (long long)(e->ts_us / 1000), e->tag, e->msg);
        if (k <= 0) break;
        if ((size_t)k >= cap - o) { o = cap - 1; break; }   // truncated: stop here
        o += (size_t)k;
    }
    xSemaphoreGive(s_mtx);
    dst[o] = '\0';
    return o;
}
