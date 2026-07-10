#include "nv_bgwork.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"

#include "nv_log.h"

static const char *TAG = "bgwork";

namespace {

struct Job { nv_bgwork_fn fn; void *arg; };

constexpr int kQueueDepth = 8;   // best-effort: overflow drops, never blocks the caller

QueueHandle_t s_q = nullptr;

void worker_task(void *) {
    Job j;
    for (;;) {
        if (xQueueReceive(s_q, &j, portMAX_DELAY) != pdTRUE) continue;
        if (j.fn) j.fn(j.arg);
    }
}

// First-use init. Racing submits are LVGL-thread-only today, but keep it safe anyway: the queue
// is the publication point, and a lost race just leaks one 12 KB task that immediately blocks.
bool ensure_worker(void) {
    if (s_q) return true;
    QueueHandle_t q = xQueueCreate(kQueueDepth, sizeof(Job));
    if (!q) return false;
    s_q = q;
    // Priority 3: above idle, below LVGL/render (4+) and every latency-sensitive driver task —
    // a thumbnail build must lose every contention. PSRAM stack (SD + heap work only; the
    // header contract forbids flash/NVS-writing jobs, which PSRAM-stacked tasks must not do).
    if (xTaskCreateWithCaps(worker_task, "nv_bgwork", 12 * 1024, nullptr, 3, nullptr,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        vQueueDelete(q);
        s_q = nullptr;
        NV_LOGE(TAG, "worker task create failed");
        return false;
    }
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
