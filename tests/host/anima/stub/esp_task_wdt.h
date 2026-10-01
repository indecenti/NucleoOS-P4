#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
esp_err_t esp_task_wdt_reset(void); esp_err_t esp_task_wdt_status(TaskHandle_t);
