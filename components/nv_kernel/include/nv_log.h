// nv_log — NucleoOS Anima logging (Phase 0: levels + tags + PSRAM ring + serial sink).
// Backed by a lock-light ring in PSRAM so the hot path never touches SD or malloc.
#pragma once
#ifdef __cplusplus
#include <cstdarg>
#include <cstddef>
#else
#include <stdarg.h>
#include <stddef.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NV_LOG_ERROR = 0,
    NV_LOG_WARN,
    NV_LOG_INFO,
    NV_LOG_DEBUG,
    NV_LOG_TRACE,
} nv_log_level_t;

// Init the ring (call once, early in app_main). Safe to call before/after PSRAM is up.
void nv_log_init(void);

// Write one entry (serial sink immediately + ring). printf-style.
void nv_log_write(nv_log_level_t level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

// Dump the retained ring (Phase 0: to stdout). Later: to viewer / net / SD.
void nv_log_dump(void);

// Format the retained ring into dst (one "L ts tag: msg" line per entry, oldest first,
// NUL-terminated, truncated to cap). Returns chars written (excl. NUL). Thread-safe; used by
// the network log sink (web console) and the on-device viewer.
size_t nv_log_snapshot(char *dst, size_t cap);

// Set the minimum level actually recorded (runtime-adjustable, no reflash).
void nv_log_set_level(nv_log_level_t level);

#ifdef __cplusplus
}
#endif

#define NV_LOGE(tag, fmt, ...) nv_log_write(NV_LOG_ERROR, tag, fmt, ##__VA_ARGS__)
#define NV_LOGW(tag, fmt, ...) nv_log_write(NV_LOG_WARN,  tag, fmt, ##__VA_ARGS__)
#define NV_LOGI(tag, fmt, ...) nv_log_write(NV_LOG_INFO,  tag, fmt, ##__VA_ARGS__)
#if defined(NV_LOG_STRIP_VERBOSE)
#define NV_LOGD(tag, fmt, ...) ((void)0)
#define NV_LOGT(tag, fmt, ...) ((void)0)
#else
#define NV_LOGD(tag, fmt, ...) nv_log_write(NV_LOG_DEBUG, tag, fmt, ##__VA_ARGS__)
#define NV_LOGT(tag, fmt, ...) nv_log_write(NV_LOG_TRACE, tag, fmt, ##__VA_ARGS__)
#endif
