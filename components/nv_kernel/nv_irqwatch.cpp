// nv_irqwatch — interrupt-storm sentinel for CPU0. See nv_irqwatch.h.
#include "nv_irqwatch.h"
#include "nv_log.h"

#include <cstdio>
#include <cstring>

#include "esp_attr.h"
#include "esp_freertos_hooks.h"
#include "esp_timer.h"
#include "soc/soc.h"
#include "soc/interrupts.h"
#include "soc/interrupt_core0_reg.h"
#include "soc/interrupt_core1_reg.h"
#include "soc/i2c_struct.h"
#include "soc/dw_gdma_struct.h"
#include "soc/dma2d_struct.h"

static const char *TAG = "irqwatch";

namespace {

constexpr uint32_t kMagic   = 0x49525157;   // "IRQW"
constexpr uint32_t kStallMs = 50;           // tick hooks run at CONFIG_FREERTOS_HZ = 1000
constexpr uint32_t kLateMs  = 250;          // just before the 300 ms interrupt watchdog

// RTC no-init survives the watchdog/panic reset (not a power cycle): the report of a storm that
// reset the chip is read back on the next boot. `sum` guards against power-on garbage.
struct Store {
    uint32_t magic;
    nv_irqwatch_report_t r;
    uint32_t sum;
};
RTC_NOINIT_ATTR Store s_store;

nv_irqwatch_report_t s_prev{};      // copied out of s_store at boot
bool                 s_have_prev = false;
uint32_t             s_logged = 0;  // this-boot events already logged by nv_irqwatch_poll

// Each CPU's tick hook counts its own ticks and watches the other CPU's count: whichever core
// stops ticking, the other one takes the snapshot.
volatile uint32_t s_ticks[2];       // tick hook count per CPU
uint32_t          s_seen[2];        // [c]: the other CPU's count as last seen by CPU c
uint32_t          s_stall[2];       // [c]: CPU c's ticks since the other CPU's count last moved

uint32_t IRAM_ATTR store_sum(const Store &s) {
    const uint32_t *w = reinterpret_cast<const uint32_t *>(&s.r);
    uint32_t x = kMagic;
    for (size_t i = 0; i < sizeof(s.r) / 4; i++) x = (x ^ w[i]) * 16777619u;
    return x;
}

inline bool IRAM_ATTR asserted(const uint32_t src[4], int n) {
    return n >= 0 && n < 128 && (src[n >> 5] & (1u << (n & 31)));
}

void IRAM_ATTR read_src(int cpu, uint32_t dst[4]) {
    const uint32_t base = cpu ? INTERRUPT_CORE1_INTR_STATUS_REG_0_REG : INTERRUPT_CORE0_INTR_STATUS_REG_0_REG;
    for (int i = 0; i < 4; i++) dst[i] = REG_READ(base + 4 * i);
}

// Peripheral registers are read only when the matrix says that source is asserted: an asserted
// source is clocked, while reading a clock-gated AXI/AHB peripheral could stall the bus.
void IRAM_ATTR first_snapshot(int stalled, uint32_t stall_ms) {
    nv_irqwatch_report_t &r = s_store.r;
    r.cpu = (uint8_t)stalled;
    r.stall_ms = stall_ms;
    r.t_us = esp_timer_get_time();
    read_src(stalled, r.src);
    if (asserted(r.src, ETS_I2C0_INTR_SOURCE)) {
        r.i2c0[0] = I2C0.int_raw.val;
        r.i2c0[1] = I2C0.int_ena.val;
        r.i2c0[2] = I2C0.int_status.val;
    }
    if (asserted(r.src, ETS_DW_GDMA_INTR_SOURCE)) {
        r.dwg[0] = DW_GDMA.int_st0.val;
        for (int c = 0; c < 4; c++) {
            r.dwg[1 + 2 * c] = DW_GDMA.ch[c].int_st0.val;
            r.dwg[2 + 2 * c] = DW_GDMA.ch[c].int_sig_ena0.val;
        }
    }
    // Sources are contiguous in soc/interrupts.h (no const tables here: .rodata is in flash, and
    // this runs from a tick hook that can fire while the flash cache is disabled).
    for (int c = 0; c < 3; c++) {
        if (!asserted(r.src, ETS_DMA2D_OUT_CH0_INTR_SOURCE + c)) continue;
        r.d2d_out[2 * c] = DMA2D.out_channel[c].out_int_st.val;
        r.d2d_out[2 * c + 1] = DMA2D.out_channel[c].out_int_ena.val;
    }
    for (int c = 0; c < 2; c++) {
        if (!asserted(r.src, ETS_DMA2D_IN_CH0_INTR_SOURCE + c)) continue;
        r.d2d_in[2 * c] = DMA2D.in_channel[c].in_int_st.val;
        r.d2d_in[2 * c + 1] = DMA2D.in_channel[c].in_int_ena.val;
    }
}

inline void IRAM_ATTR tick_watch(int self) {
    s_ticks[self] = s_ticks[self] + 1;
    const int other = self ^ 1;
    const uint32_t t = s_ticks[other];
    if (t != s_seen[self]) { s_seen[self] = t; s_stall[self] = 0; return; }
    const uint32_t n = ++s_stall[self];
    if (n == kStallMs) {
        if (s_store.r.events++ == 0) first_snapshot(other, n);   // keep the first event of the boot
        s_store.magic = kMagic;
        s_store.sum = store_sum(s_store);
    } else if (n == kLateMs && s_store.r.events == 1 && s_store.r.cpu == other) {
        read_src(other, s_store.r.src_late);
        s_store.sum = store_sum(s_store);
    }
}

void IRAM_ATTR tick_cpu0(void) { tick_watch(0); }
void IRAM_ATTR tick_cpu1(void) { tick_watch(1); }

void log_report(const nv_irqwatch_report_t &r, const char *when) {
    char now[96], late[96];
    nv_irqwatch_sources(r.src, now, sizeof now);
    nv_irqwatch_sources(r.src_late, late, sizeof late);
    NV_LOGW(TAG, "%s: CPU%u stopped ticking (%lu event(s)); asserted at %lu ms: %s; at %lu ms: %s",
            when, (unsigned)r.cpu, (unsigned long)r.events, (unsigned long)kStallMs, now,
            (unsigned long)kLateMs, late);
    NV_LOGW(TAG, "  I2C0 raw/ena/st %08lx/%08lx/%08lx  DW_GDMA common %08lx ch0 %08lx/%08lx ch1 %08lx/%08lx",
            (unsigned long)r.i2c0[0], (unsigned long)r.i2c0[1], (unsigned long)r.i2c0[2],
            (unsigned long)r.dwg[0], (unsigned long)r.dwg[1], (unsigned long)r.dwg[2],
            (unsigned long)r.dwg[3], (unsigned long)r.dwg[4]);
    NV_LOGW(TAG, "  DMA2D out st/ena %08lx/%08lx %08lx/%08lx %08lx/%08lx  in %08lx/%08lx %08lx/%08lx",
            (unsigned long)r.d2d_out[0], (unsigned long)r.d2d_out[1], (unsigned long)r.d2d_out[2],
            (unsigned long)r.d2d_out[3], (unsigned long)r.d2d_out[4], (unsigned long)r.d2d_out[5],
            (unsigned long)r.d2d_in[0], (unsigned long)r.d2d_in[1], (unsigned long)r.d2d_in[2],
            (unsigned long)r.d2d_in[3]);
}

}  // namespace

void nv_irqwatch_sources(const uint32_t src[4], char *buf, int len) {
    int o = 0;
    buf[0] = '\0';
    for (int n = 0; n < 128 && n < ETS_MAX_INTR_SOURCE && o < len - 1; n++) {
        if (!asserted(src, n)) continue;
        const char *name = esp_isr_names[n] ? esp_isr_names[n] : "?";
        o += snprintf(buf + o, len - o, "%s%s", o ? " " : "", name);
    }
    if (!buf[0]) snprintf(buf, len, "none");
}

void nv_irqwatch_init(void) {
    if (s_store.magic == kMagic && s_store.sum == store_sum(s_store) && s_store.r.events) {
        s_prev = s_store.r;
        s_prev.from_last_boot = true;
        s_have_prev = true;
        log_report(s_prev, "before the last reset");
    }
    memset(&s_store, 0, sizeof s_store);
    if (esp_register_freertos_tick_hook_for_cpu(tick_cpu0, 0) != ESP_OK ||
        esp_register_freertos_tick_hook_for_cpu(tick_cpu1, 1) != ESP_OK)
        NV_LOGE(TAG, "tick hooks unavailable: storm sentinel off");
}

void nv_irqwatch_poll(void) {
    const uint32_t ev = s_store.r.events;
    if (ev && ev != s_logged) {
        s_logged = ev;
        nv_irqwatch_report_t r = s_store.r;
        log_report(r, "this boot");
    }
}

bool nv_irqwatch_get(nv_irqwatch_report_t *out) {
    if (s_store.r.events) {                 // a storm this boot outranks the previous boot's report
        if (out) { *out = s_store.r; out->from_last_boot = false; }
        return true;
    }
    if (s_have_prev && out) *out = s_prev;
    return s_have_prev;
}
