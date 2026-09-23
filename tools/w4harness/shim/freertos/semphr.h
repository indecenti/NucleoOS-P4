#pragma once
#include "FreeRTOS.h"
#include <stdlib.h>
typedef pthread_mutex_t *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { pthread_mutex_t *m = (pthread_mutex_t *)malloc(sizeof *m); pthread_mutex_init(m, 0); return m; }
static inline int xSemaphoreTake(SemaphoreHandle_t m, TickType_t) { return pthread_mutex_lock(m) == 0; }
static inline int xSemaphoreGive(SemaphoreHandle_t m) { return pthread_mutex_unlock(m) == 0; }
