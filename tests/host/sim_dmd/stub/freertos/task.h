#pragma once
#include "FreeRTOS.h"
// host: no background tasks (the Wi-Fi request to Popper simply fails)
static inline BaseType_t xTaskCreatePinnedToCoreWithCaps(void (*fn)(void *), const char *n, unsigned st, void *a,
                                                         int pr, TaskHandle_t *h, int core, unsigned caps) {
    (void)fn; (void)n; (void)st; (void)a; (void)pr; (void)h; (void)core; (void)caps;
    return pdFAIL;
}
static inline void vTaskDeleteWithCaps(TaskHandle_t t) { (void)t; }
