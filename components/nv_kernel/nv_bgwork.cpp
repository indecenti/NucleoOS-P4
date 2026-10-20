#include "nv_bgwork.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"

#include "nv_log.h"
#include "nv_mem_attr.h"

#include <atomic>

static const char *TAG = "bgwork";

namespace {

struct Job { nv_bgwork_fn fn; void *arg; };

constexpr int kQueueDepth = 8;   // best-effort: overflow drops, never blocks the caller

QueueHandle_t s_q = nullptr;
NV_PSRAM_BSS std::atomic<uint8_t> s_init;   // 0 = no worker, 1 = being created, 2 = ready (task context only)

void worker_task(void *) {
    Job j;
    for (;;) {
        if (xQueueReceive(s_q, &j, portMAX_DELAY) != pdTRUE) continue;
        if (j.fn) j.fn(j.arg);
    }
}

// First-use init. Submits come from the LVGL thread, the esp_timer task (nv_eth), nv_bt and
// nv_telemetry, so two first submits can race: one caller creates the worker, the others wait for
// it (a lost race used to start a second worker on a queue that was then overwritten).
bool ensure_worker(void) {
    uint8_t st = s_init.load(std::memory_order_acquire);
    if (st == 2) return true;
    if (!s_init.compare_exchange_strong(st, 1)) {
        while ((st = s_init.load(std::memory_order_acquire)) == 1) vTaskDelay(1);
        return st == 2;
    }
    QueueHandle_t q = xQueueCreate(kQueueDepth, sizeof(Job));
    if (!q) { s_init.store(0); return false; }
    s_q = q;
    // Priority 3: above idle, below LVGL/render (4+) and every latency-sensitive driver task —
    // a thumbnail build must lose every contention. PSRAM stack (SD + heap work only; the
    // header contract forbids flash/NVS-writing jobs, which PSRAM-stacked tasks must not do).
    if (xTaskCreateWithCaps(worker_task, "nv_bgwork", 12 * 1024, nullptr, 3, nullptr,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        vQueueDelete(q);
        s_q = nullptr;
        s_init.store(0);
        NV_LOGE(TAG, "worker task create failed");
        return false;
    }
    s_init.store(2, std::memory_order_release);
    return true;
}

}  // namespace

bool nv_bgwork_submit(nv_bgwork_fn fn, void *arg) {
    if (!fn || !ensure_worker()) return false;
    Job j = {fn, arg};
    if (xQueueSend(s_q, &j, 0) != pdTRUE) {
        NV_LOGW(TAG, "queue full — job dropped");
        return false;
    }
    return true;
}
