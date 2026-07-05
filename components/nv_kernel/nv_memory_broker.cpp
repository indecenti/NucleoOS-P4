#include "nv_memory_broker.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"

#include "nv_event_bus.h"
#include "nv_log.h"

static const char *TAG = "mem";

// s_bmtx makes nv_mem_request / nv_mem_release mutually exclusive and keeps the
// s_request_active flag atomic with the suspend/resume it drives. The service manager has its
// own lock and never calls back into the broker, so the lock order (broker -> service) is safe.
namespace {

SemaphoreHandle_t s_bmtx = nullptr;
size_t s_low_threshold = 48 * 1024;  // free internal heap alarm level
bool s_watchdog_running = false;
bool s_request_active = false;  // guarded by s_bmtx

void watchdog_task(void *) {
    bool alarm = false;
    for (;;) {
        const size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        if (free_int < s_low_threshold && !alarm) {
            alarm = true;
            NV_LOGW(TAG, "LOW MEMORY: free SRAM %u KB < %u KB",
                    (unsigned)(free_int / 1024), (unsigned)(s_low_threshold / 1024));
            nv_event_publish(NV_EV_LOW_MEMORY, &free_int);
        } else if (free_int >= s_low_threshold + 8 * 1024) {
            alarm = false;  // hysteresis
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// Assumes s_bmtx is held.
void release_locked(void) {
    if (!s_request_active) return;
    nv_service_resume_suspended();
    s_request_active = false;
    NV_LOGI(TAG, "released — services resumed");
}

}  // namespace

void nv_mem_broker_init(void) {
    if (!s_bmtx) s_bmtx = xSemaphoreCreateMutex();
    const size_t ps = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    const size_t in = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    NV_LOGI(TAG, "memory broker init — PSRAM total=%u KB, SRAM total=%u KB",
            (unsigned)(ps / 1024), (unsigned)(in / 1024));
    if (!s_watchdog_running) {
        s_watchdog_running =
            xTaskCreateWithCaps(watchdog_task, "nv_lowmem", 3072, nullptr, 5, nullptr,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;   // stack in PSRAM
        if (!s_watchdog_running) NV_LOGE(TAG, "low-mem watchdog task create failed");
    }
}

size_t nv_mem_free_internal(void) { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL); }
size_t nv_mem_free_psram(void) { return heap_caps_get_free_size(MALLOC_CAP_SPIRAM); }
size_t nv_mem_largest_internal(void) {
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
}

void nv_mem_set_low_threshold(size_t bytes) {
    if (!s_bmtx) { s_low_threshold = bytes; return; }
    xSemaphoreTake(s_bmtx, portMAX_DELAY);
    s_low_threshold = bytes;
    xSemaphoreGive(s_bmtx);
}

bool nv_mem_request(size_t budget, const nv_service_id_t *keep, int keep_n) {
    if (!s_bmtx) return false;
    xSemaphoreTake(s_bmtx, portMAX_DELAY);
    if (s_request_active) {
        NV_LOGW(TAG, "nv_mem_request while another is active — releasing previous first");
        release_locked();
    }
    nv_service_suspend_nonessential(keep, keep_n);
    s_request_active = true;

    const size_t avail = nv_mem_free_internal() + nv_mem_free_psram();
    const bool fits = avail >= budget;
    NV_LOGI(TAG, "request %u KB: available %u KB after freeing -> %s",
            (unsigned)(budget / 1024), (unsigned)(avail / 1024), fits ? "OK" : "REFUSED");
    xSemaphoreGive(s_bmtx);
    return fits;
}

void nv_mem_release(void) {
    if (!s_bmtx) return;
    xSemaphoreTake(s_bmtx, portMAX_DELAY);
    release_locked();
    xSemaphoreGive(s_bmtx);
}
