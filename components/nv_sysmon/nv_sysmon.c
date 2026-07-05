// nv_sysmon — see header. Delta-based CPU sampling with concurrency-safe coalescing.
#include "nv_sysmon.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"

#include "nv_service_mgr.h"
#include "nv_hal.h"        // nv_hal_temp_read

#include <string.h>
#include <stdlib.h>

// Coalesce window: calls closer together than this reuse the cached snapshot instead of
// recomputing a delta, so two consumers polling at ~1 Hz never split each other's baseline.
#define NV_SYSMON_MIN_US   400000   // 0.4 s

#define NV_SYSMON_MAX_TASKS 48      // per-task baseline table + cached rows capacity

// One mutex guards all shared sampler state (perf baseline + cached perf, task baseline +
// cached rows). Held only for the (short) sampling work; callers copy out under it.
static SemaphoreHandle_t s_mtx;

// ---- perf (per-core load) baseline + cache ----
static bool     s_perf_primed;
static uint32_t s_perf_prev_idle[2];
static uint32_t s_perf_prev_us;
static uint64_t s_perf_last_sample_us;
static nv_sys_perf_t s_perf_cache;

// ---- per-task cpu baseline + cached rows ----
static bool          s_task_primed;
static uint64_t      s_task_last_sample_us;
static TaskHandle_t  s_prev_h[NV_SYSMON_MAX_TASKS];
static uint32_t      s_prev_rt[NV_SYSMON_MAX_TASKS];
static int           s_prev_n;
static nv_task_row_t s_task_cache[NV_SYSMON_MAX_TASKS];
static int           s_task_cache_n;

static void ensure_init(void) {
    if (!s_mtx) {
        // First-caller lazy init. A race here is benign: xSemaphoreCreateMutex is cheap and a
        // duplicate would just leak one handle, but in practice the first telemetry read happens
        // long after the scheduler is single-threaded through app open.
        s_mtx = xSemaphoreCreateMutex();
    }
}

static uint32_t prev_rt_of(TaskHandle_t h) {
    for (int i = 0; i < s_prev_n; i++)
        if (s_prev_h[i] == h) return s_prev_rt[i];
    return 0;
}

void nv_sysmon_perf(nv_sys_perf_t *out) {
    if (!out) return;
    ensure_init();
    xSemaphoreTake(s_mtx, portMAX_DELAY);

    const uint64_t now = (uint64_t)esp_timer_get_time();
    const bool fresh = s_perf_primed && (now - s_perf_last_sample_us) < NV_SYSMON_MIN_US;

    if (!fresh) {
        nv_sys_perf_t p;
        memset(&p, 0, sizeof p);
        p.freq_mhz   = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
        p.uptime_s   = now / 1000000ULL;
        p.task_count = uxTaskGetNumberOfTasks();
        p.core_load[0] = p.core_load[1] = -1.0f;
        p.load_avg = -1.0f;

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
        const uint32_t now_us = (uint32_t)now;
        uint32_t idle[2];
        for (int c = 0; c < 2; c++)
            idle[c] = (uint32_t)ulTaskGetIdleRunTimeCounterForCore(c);
        if (s_perf_primed) {
            const uint32_t elapsed = now_us - s_perf_prev_us;
            if (elapsed > 0) {
                float sum = 0; int nvalid = 0;
                for (int c = 0; c < 2; c++) {
                    const uint32_t d = idle[c] - s_perf_prev_idle[c];
                    float load = 100.0f - (float)((uint64_t)d * 100 / elapsed);
                    if (load < 0) load = 0;
                    if (load > 100) load = 100;
                    p.core_load[c] = load;
                    sum += load; nvalid++;
                }
                p.load_avg = nvalid ? sum / nvalid : -1.0f;
            }
        }
        s_perf_prev_idle[0] = idle[0];
        s_perf_prev_idle[1] = idle[1];
        s_perf_prev_us = now_us;
#endif
        float t;
        if (nv_hal_temp_read(&t)) { p.temp_c = t; p.temp_valid = true; }

        s_perf_cache = p;
        s_perf_primed = true;
        s_perf_last_sample_us = now;
    }

    *out = s_perf_cache;
    xSemaphoreGive(s_mtx);
}

void nv_sysmon_mem(nv_sys_mem_t *out) {
    if (!out) return;
    static const uint32_t caps[2] = {
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
        MALLOC_CAP_SPIRAM   | MALLOC_CAP_8BIT,
    };
    nv_mem_pool_t *pool[2] = {&out->internal, &out->psram};
    for (int i = 0; i < 2; i++) {
        const size_t total = heap_caps_get_total_size(caps[i]);
        const size_t freeb = heap_caps_get_free_size(caps[i]);
        const size_t large = heap_caps_get_largest_free_block(caps[i]);
        const size_t minf  = heap_caps_get_minimum_free_size(caps[i]);
        pool[i]->total      = total;
        pool[i]->free_bytes = freeb;
        pool[i]->used       = total > freeb ? total - freeb : 0;
        pool[i]->largest    = large;
        pool[i]->min_free   = minf;
        pool[i]->frag_pct   = freeb ? (uint8_t)(100 - (large * 100 / freeb)) : 0;
    }
}

int nv_sysmon_tasks(nv_task_row_t *buf, int max) {
    if (!buf || max <= 0) return 0;
    ensure_init();
    xSemaphoreTake(s_mtx, portMAX_DELAY);

    const uint64_t now = (uint64_t)esp_timer_get_time();
    const bool fresh = s_task_primed && (now - s_task_last_sample_us) < NV_SYSMON_MIN_US;

    if (!fresh) {
        UBaseType_t cap = uxTaskGetNumberOfTasks();
        if (cap > NV_SYSMON_MAX_TASKS) cap = NV_SYSMON_MAX_TASKS;
        TaskStatus_t *st = (TaskStatus_t *)malloc(cap * sizeof(TaskStatus_t));
        if (st) {
            uint32_t total_rt = 0;
            UBaseType_t n = uxTaskGetSystemState(st, cap, &total_rt);

            // First pass: sum per-task deltas for normalization.
            uint64_t sum_delta = 0;
            for (UBaseType_t i = 0; i < n; i++) {
                const uint32_t prev = prev_rt_of(st[i].xHandle);
                sum_delta += (uint32_t)(st[i].ulRunTimeCounter - prev);
            }
            if (sum_delta == 0) sum_delta = 1;  // avoid divide-by-zero on the priming call

            int rn = 0;
            for (UBaseType_t i = 0; i < n && rn < NV_SYSMON_MAX_TASKS; i++, rn++) {
                nv_task_row_t *r = &s_task_cache[rn];
                strncpy(r->name, st[i].pcTaskName ? st[i].pcTaskName : "?", sizeof r->name - 1);
                r->name[sizeof r->name - 1] = '\0';
                r->state     = (uint8_t)st[i].eCurrentState;
                r->prio      = (uint8_t)st[i].uxCurrentPriority;
                r->base_prio = (uint8_t)st[i].uxBasePriority;
                r->stack_free = (uint32_t)st[i].usStackHighWaterMark;  // bytes on this port
#if defined(configTASKLIST_INCLUDE_COREID) && (configTASKLIST_INCLUDE_COREID == 1)
                r->core = (st[i].xCoreID == tskNO_AFFINITY) ? -1 : (int8_t)st[i].xCoreID;
#else
                r->core = -1;
#endif
                const uint32_t prev = prev_rt_of(st[i].xHandle);
                const uint32_t d = (uint32_t)(st[i].ulRunTimeCounter - prev);
                r->cpu_pct = s_task_primed ? (float)((uint64_t)d * 100 / sum_delta) : 0.0f;
            }
            s_task_cache_n = rn;

            // Save this snapshot as the next baseline.
            s_prev_n = 0;
            for (UBaseType_t i = 0; i < n && s_prev_n < NV_SYSMON_MAX_TASKS; i++, s_prev_n++) {
                s_prev_h[s_prev_n]  = st[i].xHandle;
                s_prev_rt[s_prev_n] = st[i].ulRunTimeCounter;
            }
            free(st);

            // Sort cached rows by cpu_pct descending (small n — insertion sort).
            for (int i = 1; i < s_task_cache_n; i++) {
                nv_task_row_t key = s_task_cache[i];
                int j = i - 1;
                while (j >= 0 && s_task_cache[j].cpu_pct < key.cpu_pct) {
                    s_task_cache[j + 1] = s_task_cache[j];
                    j--;
                }
                s_task_cache[j + 1] = key;
            }

            s_task_primed = true;
            s_task_last_sample_us = now;
        }
    }

    int out_n = s_task_cache_n < max ? s_task_cache_n : max;
    memcpy(buf, s_task_cache, out_n * sizeof(nv_task_row_t));
    xSemaphoreGive(s_mtx);
    return out_n;
}

int nv_sysmon_services(nv_svc_row_t *buf, int max) {
    if (!buf || max <= 0) return 0;
    const int n = nv_service_count();
    int out_n = 0;
    for (int id = 0; id < n && out_n < max; id++, out_n++) {
        nv_svc_row_t *r = &buf[out_n];
        const char *nm = nv_service_name(id);
        strncpy(r->name, nm ? nm : "?", sizeof r->name - 1);
        r->name[sizeof r->name - 1] = '\0';
        r->state     = (uint8_t)nv_service_state(id);
        r->essential = nv_service_essential(id);
    }
    return out_n;
}
