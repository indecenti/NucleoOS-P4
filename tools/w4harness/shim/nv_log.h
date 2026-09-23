#pragma once
#include <stdio.h>
#define NV_LOGE(t, f, ...) fprintf(stderr, "E %s: " f "\n", t, ##__VA_ARGS__)
#define NV_LOGW(t, f, ...) fprintf(stderr, "W %s: " f "\n", t, ##__VA_ARGS__)
#define NV_LOGI(t, f, ...) fprintf(stderr, "I %s: " f "\n", t, ##__VA_ARGS__)
