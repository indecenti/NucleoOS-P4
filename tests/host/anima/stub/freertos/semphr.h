#pragma once
#include "FreeRTOS.h"
SemaphoreHandle_t xSemaphoreCreateMutex(void); SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t); BaseType_t xSemaphoreGive(SemaphoreHandle_t);
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t, TickType_t); BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t); void vSemaphoreDelete(SemaphoreHandle_t);
