// Device platform for the heavy-work arbiter: FreeRTOS mutex, esp_timer clock, internal-heap
// probe, and a "system.busy" event so the shell can show a banner / gate client-side apps.
// Compiled out of the host build (which supplies its own arb_plat_* via Win32). See arb_plat.h.
#ifndef ARB_HOST

#include "arb_plat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdio.h>

static SemaphoreHandle_t s_mtx;

void arb_plat_init(void)
{
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
}

void arb_plat_lock(void)
{
    if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY);   // held microseconds only -> cannot stall a task
}

void arb_plat_unlock(void)
{
    if (s_mtx) xSemaphoreGive(s_mtx);
}

uint32_t arb_plat_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void arb_plat_sleep_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1));            // 0 -> at least one tick yield
}

size_t arb_plat_heap_free(void)
{
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

void arb_plat_on_busy(bool busy, const char *job)
{
    // NucleoV2: no web-shell busy banner yet — keep the edge visible in the log. Wire this to
    // nv_event_bus when the web console grows a busy indicator.
    ESP_LOGI("nucleo_arb", "busy=%d job=%s", (int)busy, (busy && job) ? job : "");
}

#endif // !ARB_HOST
