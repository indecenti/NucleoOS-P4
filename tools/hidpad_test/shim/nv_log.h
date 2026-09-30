// Host shim of nv_log.h for tools/hidpad_test.
#pragma once
#include <stdio.h>
#define NV_LOGI(tag, ...) (printf("I %s: ", tag), printf(__VA_ARGS__), printf("\n"))
#define NV_LOGW NV_LOGI
#define NV_LOGE NV_LOGI
