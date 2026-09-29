// nv_irqwatch — interrupt-storm sentinel for both CPUs.
//
// An interrupt source that stays asserted with nobody clearing it (a shared handler freed or disabled
// while its event is pending, a status mask that skips the raised bit...) makes the CPU re-enter the
// ISR forever: the FreeRTOS tick starves and the interrupt watchdog resets the chip ~300 ms later,
// with a core dump that only shows `shared_intr_isr` — not WHICH source it was.
//
// Each CPU's tick hook counts its own ticks and watches the other's. When one CPU has not ticked for
// kStallMs, the other snapshots the interrupt-matrix status of the stalled CPU (the asserted
// peripheral sources) plus the interrupt registers of the peripherals on the suspect shared line,
// into RTC no-init memory that survives the watchdog reset. The next boot logs it and /api/crash
// reports it.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t events;        // CPU stalls detected during that boot
    uint8_t  cpu;           // the CPU that stopped ticking (first event)
    uint32_t stall_ms;      // how long that CPU had not ticked at the first snapshot
    int64_t  t_us;          // esp_timer time of the first snapshot
    uint32_t src[4];        // its interrupt-matrix source status at the first snapshot (bit = source)
    uint32_t src_late[4];   // same, ~250 ms into the stall (what is STILL asserted)
    uint32_t i2c0[3];       // int_raw, int_ena, int_status        (read only if I2C0 is asserted)
    uint32_t dwg[1 + 4 * 2];// common intstatus, then per channel {int_st0, int_sig_ena0}
    uint32_t d2d_out[3 * 2];// per OUT channel {int_st, int_ena}   (read only if asserted)
    uint32_t d2d_in[2 * 2]; // per IN channel {int_st, int_ena}
    bool     from_last_boot;
} nv_irqwatch_report_t;

// Arm the sentinel (registers the two tick hooks) and pick up a report left by the previous boot.
void nv_irqwatch_init(void);

// Log a storm detected during this boot (one that resolved before the watchdog). Call periodically
// from task context (the boot heartbeat does).
void nv_irqwatch_poll(void);

// Latest report: from the previous boot (the reset it caused) or from this boot (a storm that
// resolved itself before the watchdog). False when none.
bool nv_irqwatch_get(nv_irqwatch_report_t *out);

// Human-readable names of the asserted sources in `src` ("I2C0 DW_GDMA ..."), into buf.
void nv_irqwatch_sources(const uint32_t src[4], char *buf, int len);

#ifdef __cplusplus
}
#endif
