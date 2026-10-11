// Host stand-in for nv_log.h.
#pragma once
#include <stdio.h>
#define NV_LOGE(tag, fmt, ...) fprintf(stderr, "E %s: " fmt "\n", tag, ##__VA_ARGS__)
#define NV_LOGW(tag, fmt, ...) fprintf(stderr, "W %s: " fmt "\n", tag, ##__VA_ARGS__)
#define NV_LOGI(tag, fmt, ...) ((void)0)
