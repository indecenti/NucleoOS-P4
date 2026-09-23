#pragma once
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>
typedef int BaseType_t; typedef uint32_t TickType_t; typedef void *TaskHandle_t;
#define pdPASS 1
#define pdFAIL 0
#define pdTRUE 1
#define portMAX_DELAY 0xffffffffu
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
static inline void vTaskDelay(TickType_t t) { usleep(t * 1000); }
