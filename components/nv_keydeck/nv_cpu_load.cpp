// nv_cpu_load — see header. Load% = 100 - idle share of the elapsed interval, per core.
#include "nv_cpu_load.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS

// With the default esp_timer stats clock both the idle counters and esp_timer_get_time()
// tick in microseconds, so the two deltas are directly comparable. 32-bit unsigned
// subtraction stays correct across counter wrap as long as samples are < ~71 min apart.
void nv_cpu_load_get(int *cpu0, int *cpu1)
{
    static uint32_t s_prev_idle[2] = {0, 0};
    static uint32_t s_prev_us = 0;
    static bool s_primed = false;

    const uint32_t now_us = (uint32_t)esp_timer_get_time();
    uint32_t idle[2];
    for (int c = 0; c < 2; c++)
        idle[c] = (uint32_t)ulTaskGetIdleRunTimeCounterForCore(c);

    int out[2] = {-1, -1};
    if (s_primed) {
        const uint32_t elapsed = now_us - s_prev_us;
        if (elapsed > 0) {
            for (int c = 0; c < 2; c++) {
                const uint32_t d = idle[c] - s_prev_idle[c];
                int load = 100 - (int)((uint64_t)d * 100 / elapsed);
                if (load < 0) load = 0;
                if (load > 100) load = 100;
                out[c] = load;
            }
        }
    }
    s_prev_idle[0] = idle[0];
    s_prev_idle[1] = idle[1];
    s_prev_us = now_us;
    s_primed = true;

    if (cpu0) *cpu0 = out[0];
    if (cpu1) *cpu1 = out[1];
}

#else  // run-time stats disabled — report "unknown"

void nv_cpu_load_get(int *cpu0, int *cpu1)
{
    if (cpu0) *cpu0 = -1;
    if (cpu1) *cpu1 = -1;
}

#endif
