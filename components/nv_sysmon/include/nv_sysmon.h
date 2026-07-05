// nv_sysmon — NucleoOS system telemetry core (the "Task Manager" data layer).
// ONE source of truth for CPU load, heap by region, the live FreeRTOS task table, and the
// service registry snapshot. The System Monitor app renders it, the web companion serves it,
// and KeyDeck reads it — no component re-implements sampling. Pure C, no LVGL.
//
// Sampling model: perf (per-core load) and per-task CPU% are DELTAS between calls, so a caller
// must poll at a steady cadence (~1 Hz). To keep concurrent consumers (app + web) from splitting
// each other's baseline, results are cached and coalesced: calls closer than a minimum interval
// return the previous snapshot instead of recomputing the delta. Mem and services are cheap,
// stateless snapshots and are always sampled fresh.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float    core_load[2];   // per-core load 0..100; -1 if run-time stats unavailable
    float    load_avg;       // mean of the valid cores; -1 if none
    uint32_t freq_mhz;       // configured CPU frequency
    float    temp_c;         // on-die temperature; valid only if temp_valid
    bool     temp_valid;     // false when the sensor is absent/unreadable
    uint64_t uptime_s;       // seconds since boot
    uint32_t task_count;     // uxTaskGetNumberOfTasks()
} nv_sys_perf_t;

typedef struct {
    size_t   total;          // region size (bytes)
    size_t   free_bytes;     // currently free
    size_t   used;           // total - free
    size_t   largest;        // largest allocatable block (fragmentation proxy)
    size_t   min_free;       // lifetime low-water free
    uint8_t  frag_pct;       // 100 * (1 - largest/free); 0 when free == 0
} nv_mem_pool_t;

typedef struct {
    nv_mem_pool_t internal;  // on-chip SRAM — the scarce resource that actually matters
    nv_mem_pool_t psram;     // external OCT PSRAM — big buffers
} nv_sys_mem_t;

// eCurrentState values (mirror FreeRTOS eTaskState): 0=Running 1=Ready 2=Blocked
// 3=Suspended 4=Deleted 5=Invalid.
typedef struct {
    char     name[16];       // task name (truncated)
    uint8_t  state;          // eTaskState
    uint8_t  prio;           // current (possibly inherited) priority
    uint8_t  base_prio;      // base priority
    int8_t   core;           // 0/1 pinned core, or -1 = no affinity / unknown
    uint32_t stack_free;     // stack high-water free (BYTES) — near 0 == near overflow
    float    cpu_pct;        // share of total CPU time since the previous sample (0..100)
} nv_task_row_t;

typedef struct {
    char    name[24];        // service name
    uint8_t state;           // nv_service_state_t (0=stopped 1=running 2=suspended)
    bool    essential;       // never auto-suspended by the Memory Broker
} nv_svc_row_t;

// Instantaneous performance. Non-blocking; core load is the delta since the previous call
// (coalesced — see the sampling note above). Safe from any task.
void nv_sysmon_perf(nv_sys_perf_t *out);

// Both heap regions, sampled fresh.
void nv_sysmon_mem(nv_sys_mem_t *out);

// Fill up to `max` rows with the live task table, sorted by cpu_pct descending. Returns the
// number of rows written. Per-task cpu_pct is a delta since the previous call (coalesced).
int nv_sysmon_tasks(nv_task_row_t *buf, int max);

// Fill up to `max` rows with the registered services. Returns the number written.
int nv_sysmon_services(nv_svc_row_t *buf, int max);

#ifdef __cplusplus
}
#endif
