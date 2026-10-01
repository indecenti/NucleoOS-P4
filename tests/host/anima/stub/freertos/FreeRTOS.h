#pragma once
#include <stdint.h>
typedef int BaseType_t; typedef unsigned UBaseType_t; typedef uint32_t TickType_t; typedef void* TaskHandle_t; typedef void* QueueHandle_t; typedef void* SemaphoreHandle_t;
typedef struct { int x; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0}
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY 0xffffffffu
#define pdMS_TO_TICKS(x) ((TickType_t)(x))
#define portTICK_PERIOD_MS 1
