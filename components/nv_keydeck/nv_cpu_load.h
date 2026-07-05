// nv_cpu_load — per-core CPU load sampler for KeyDeck telemetry (component-private).
//
// Computes load% per core from the delta of the idle task's run-time counter between two
// calls, against the elapsed wall time (esp_timer, which is also the run-time-stats clock
// source). First call — and any call without CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS —
// reports -1 ("unknown"). Intended to be sampled by ONE caller at ~1 Hz (the telemetry
// loop): the previous-sample state is a single static snapshot, not per-caller.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Fill load percentages (0..100) for core 0 and 1, or -1 when unavailable.
void nv_cpu_load_get(int *cpu0, int *cpu1);

#ifdef __cplusplus
}
#endif
