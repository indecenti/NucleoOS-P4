#pragma once
#include "FreeRTOS.h"
// The harness runs carts silent: no audio task is created.
static inline BaseType_t xTaskCreateWithCaps(void (*)(void *), const char *, uint32_t, void *, int, TaskHandle_t *, uint32_t) { return pdFAIL; }
static inline uint32_t ulTaskNotifyTake(BaseType_t, TickType_t) { return 0; }
static inline void xTaskNotifyGive(TaskHandle_t) {}
