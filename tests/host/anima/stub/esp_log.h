// Host stub: engine logs go to stderr only when ANIMA_TEST_LOG is set (quiet test output).
#pragma once
#include <stdio.h>
#include <stdlib.h>
#define ANIMA_HLOG_(lvl, tag, ...) do { if (getenv("ANIMA_TEST_LOG")) { fprintf(stderr, "%s %s: ", lvl, tag); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define ESP_LOGE(tag, ...) ANIMA_HLOG_("E", tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) ANIMA_HLOG_("W", tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) ANIMA_HLOG_("I", tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) ((void)(tag))
#define ESP_LOGV(tag, ...) ((void)(tag))
