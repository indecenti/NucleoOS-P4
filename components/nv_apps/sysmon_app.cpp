// sysmon_app — System Monitor v3 (an evolved, space-filling live-device dashboard).
// Left rail -> three tabs, each laid out to use the full panel:
//   Performance : a KPI strip + a two-column dashboard (left = per-core CPU arcs over a growing
//                 history chart; right = linear RAM/PSRAM meters + a system facts block).
//   Processes   : sort toolbar + live task table with inline CPU bars (in-place update).
//   Services    : summary header + a two-column grid of service tiles (in-place update).
// All data comes from nv_sysmon — this file is pure presentation.
//
// UX/perf invariants:
//  * Read-only. FreeRTOS has no process isolation, so there is NO kill button.
//  * Process/Service lists update IN PLACE (stable widget pool) and only rebuild on count change,
//    so the 1 Hz refresh never flickers or resets scroll.
//  * Charts/arcs/bars only — none of the P4 software-renderer draw-layer traps (shadow/opacity/
//    transform/clip_corner). See the LVGL draw-layers note.
//  * One 1 Hz lv_timer, created in build(), freed on LV_EVENT_DELETE.
//  * The ESP32-P4 exposes two FreeRTOS-scheduled HP RISC-V cores (0/1). Its LP core is unscheduled
//    and the Wi-Fi ESP32-C6 is a separate SoC — neither is measurable from here.
#include "apps_internal.h"

#include "nv_app.h"
#include "nv_ui_kit.h"
#include "nv_icons.h"
#include "nv_i18n.h"
#include "nv_theme.h"
#include "nv_fonts.h"
#include "nv_sysmon.h"

#include "lvgl.h"
#include <cstdio>
#include <cstring>
#include <cstdint>

namespace {

constexpr int kHistory  = 90;    // chart points (== seconds of history at 1 Hz)
constexpr int kMaxRows  = 48;    // task/service rows rendered
constexpr int kLowStack = 512;   // bytes: stack-free below this is flagged red

#define CORE0_COLOR lv_color_hex(0x8B6CE0)   // violet
#define CORE1_COLOR lv_color_hex(0x3AC0E0)   // cyan

enum Tab  { TAB_PERF = 0, TAB_PROC, TAB_SVC };
enum Sort { SORT_CPU = 0, SORT_NAME, SORT_STACK };

int         s_tab   = TAB_PERF;
int         s_sort  = SORT_CPU;
lv_timer_t *s_timer = nullptr;
lv_obj_t   *s_panel = nullptr;
lv_obj_t   *s_rail_btn[3] = {nullptr, nullptr, nullptr};

// ---- 3-tier load color (green < 60 <= amber < 85 <= red) ----
lv_color_t load_color(int pct) {
    const NvTheme *th = nv_theme_get();
    if (pct >= 85) return th->danger;
    if (pct >= 60) return lv_color_hex(0xE0A020);
    return th->success_solid;
}
lv_color_t temp_color(float c) {
    const NvTheme *th = nv_theme_get();
    if (c >= 70) return th->danger;
    if (c >= 55) return lv_color_hex(0xE0A020);
    return th->success_solid;
}

const char *task_state_str(uint8_t st) {
    switch (st) {
        case 0:  return nv_tr(NV_STR_RUNNING);
        case 1:  return nv_tr(NV_STR_SM_READY);
        case 2:  return nv_tr(NV_STR_SM_BLOCKED);
        case 3:  return nv_tr(NV_STR_SM_SUSPENDED);
        default: return "-";
    }
}
const char *svc_state_str(uint8_t st) {
    switch (st) {
        case 1:  return nv_tr(NV_STR_RUNNING);
        case 2:  return nv_tr(NV_STR_SM_SUSPENDED);
        default: return nv_tr(NV_STR_SM_STOPPED);
    }
}

lv_obj_t *card_base(lv_obj_t *parent) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_style_bg_color(c, th->surface, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, NV_RAD_MD, 0);
    lv_obj_set_style_pad_all(c, NV_SP_3, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, th->divider, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

lv_obj_t *section_title(lv_obj_t *parent, const char *tx) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *t = lv_label_create(parent);
    lv_label_set_text(t, tx);
    lv_obj_set_style_text_font(t, &nv_font_20, 0);
    lv_obj_set_style_text_color(t, th->text_strong, 0);
    return t;
}

// ==================================================================== Performance tab
struct Gauge { lv_obj_t *arc; lv_obj_t *pct; lv_obj_t *sub; };
Gauge s_g_cpu0, s_g_cpu1;
lv_obj_t *s_cpu_chart = nullptr; lv_chart_series_t *s_cpu_s0 = nullptr, *s_cpu_s1 = nullptr;
lv_obj_t *s_sys_val = nullptr;
// KPI strip value labels.
lv_obj_t *s_kpi_cpu = nullptr, *s_kpi_sram = nullptr, *s_kpi_psram = nullptr,
         *s_kpi_temp = nullptr, *s_kpi_up = nullptr;
// Linear memory meters.
lv_obj_t *s_sram_bar = nullptr, *s_sram_lbl = nullptr, *s_psram_bar = nullptr, *s_psram_lbl = nullptr;

void clear_perf_refs() {
    s_g_cpu0 = s_g_cpu1 = {nullptr, nullptr, nullptr};
    s_cpu_chart = nullptr; s_cpu_s0 = s_cpu_s1 = nullptr;
    s_sys_val = nullptr;
    s_kpi_cpu = s_kpi_sram = s_kpi_psram = s_kpi_temp = s_kpi_up = nullptr;
    s_sram_bar = s_sram_lbl = s_psram_bar = s_psram_lbl = nullptr;
}

// A compact KPI tile: small caption over a big colored value. Returns the value label.
lv_obj_t *make_kpi(lv_obj_t *parent, const char *caption) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *tile = card_base(parent);
    lv_obj_set_flex_grow(tile, 1);
    lv_obj_set_height(tile, lv_pct(100));
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(tile, NV_SP_2, 0);
    lv_obj_set_style_pad_row(tile, 2, 0);

    lv_obj_t *cap = lv_label_create(tile);
    lv_label_set_text(cap, caption);
    lv_obj_set_style_text_font(cap, &nv_font_14, 0);
    lv_obj_set_style_text_color(cap, th->text_dim, 0);

    lv_obj_t *val = lv_label_create(tile);
    lv_label_set_text(val, "-");
    lv_obj_set_style_text_font(val, &nv_font_28, 0);
    lv_obj_set_style_text_color(val, th->text_strong, 0);
    return val;
}

Gauge make_gauge(lv_obj_t *parent, const char *title) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_flex_grow(box, 1);
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, NV_SP_1, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(box);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, &nv_font_14, 0);
    lv_obj_set_style_text_color(t, th->text_dim, 0);

    lv_obj_t *arc = lv_arc_create(box);
    lv_obj_set_size(arc, 118, 118);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 0);
    lv_obj_remove_style(arc, nullptr, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(arc, 11, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 11, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, th->surface3, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, th->success_solid, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);

    lv_obj_t *pct = lv_label_create(arc);
    lv_label_set_text(pct, "0%");
    lv_obj_set_style_text_font(pct, &nv_font_28, 0);
    lv_obj_set_style_text_color(pct, th->text_strong, 0);
    lv_obj_align(pct, LV_ALIGN_CENTER, 0, -8);

    lv_obj_t *sub = lv_label_create(arc);
    lv_label_set_text(sub, "");
    lv_obj_set_style_text_font(sub, &nv_font_14, 0);
    lv_obj_set_style_text_color(sub, th->text_dim, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 18);
    return {arc, pct, sub};
}

void gauge_set(Gauge &g, int pct, const char *sub) {
    if (!g.arc) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    lv_arc_set_value(g.arc, pct);
    lv_obj_set_style_arc_color(g.arc, load_color(pct), LV_PART_INDICATOR);
    char b[12]; snprintf(b, sizeof b, "%d%%", pct);
    lv_label_set_text(g.pct, b);
    lv_label_set_text(g.sub, sub);
}

// A labelled linear meter row: "<name> ............ <value>" over a full-width bar.
void make_meter(lv_obj_t *parent, const char *name, lv_obj_t **bar, lv_obj_t **lbl) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_width(box, lv_pct(100));
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, NV_SP_1, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = lv_obj_create(box);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_width(hdr, lv_pct(100));
    lv_obj_set_height(hdr, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *nm = lv_label_create(hdr);
    lv_label_set_text(nm, name);
    lv_obj_set_flex_grow(nm, 1);
    lv_obj_set_style_text_color(nm, th->text, 0);
    lv_obj_t *vl = lv_label_create(hdr);
    lv_label_set_text(vl, "-");
    lv_obj_set_style_text_color(vl, th->text_dim, 0);
    *lbl = vl;

    lv_obj_t *b = lv_bar_create(box);
    lv_obj_set_width(b, lv_pct(100));
    lv_obj_set_height(b, 16);
    lv_bar_set_range(b, 0, 100);
    lv_obj_set_style_radius(b, 8, LV_PART_MAIN);
    lv_obj_set_style_radius(b, 8, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(b, th->surface3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, th->accent, LV_PART_INDICATOR);
    *bar = b;
}

lv_obj_t *make_chart(lv_obj_t *card, lv_color_t c0, lv_chart_series_t **s0,
                     lv_color_t c1, lv_chart_series_t **s1) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *ch = lv_chart_create(card);
    lv_obj_set_width(ch, lv_pct(100));
    lv_obj_set_flex_grow(ch, 1);          // fill remaining vertical space
    lv_obj_set_style_bg_opa(ch, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ch, 0, 0);
    lv_obj_set_style_pad_all(ch, 0, 0);
    lv_obj_set_style_line_color(ch, th->divider, LV_PART_MAIN);
    lv_obj_set_style_size(ch, 0, 0, LV_PART_INDICATOR);
    lv_chart_set_type(ch, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(ch, kHistory);
    lv_chart_set_range(ch, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_div_line_count(ch, 5, 0);
    lv_chart_set_update_mode(ch, LV_CHART_UPDATE_MODE_SHIFT);
    *s0 = lv_chart_add_series(ch, c0, LV_CHART_AXIS_PRIMARY_Y);
    if (s1) *s1 = lv_chart_add_series(ch, c1, LV_CHART_AXIS_PRIMARY_Y);
    lv_obj_set_style_line_width(ch, 2, LV_PART_ITEMS);
    return ch;
}

lv_obj_t *legend_dot(lv_obj_t *parent, lv_color_t c, const char *tx) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_radius(dot, 5, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, c, 0);
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, tx);
    lv_obj_set_style_text_font(l, &nv_font_14, 0);
    lv_obj_set_style_text_color(l, th->text_dim, 0);
    lv_obj_set_style_pad_right(l, NV_SP_2, 0);
    return l;
}

void build_perf(lv_obj_t *panel) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *rootc = lv_obj_create(panel);
    lv_obj_remove_style_all(rootc);
    lv_obj_set_size(rootc, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(rootc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(rootc, NV_SP_3, 0);
    lv_obj_clear_flag(rootc, LV_OBJ_FLAG_SCROLLABLE);

    // ---- KPI strip ----
    lv_obj_t *kpi = lv_obj_create(rootc);
    lv_obj_remove_style_all(kpi);
    lv_obj_set_width(kpi, lv_pct(100));
    lv_obj_set_height(kpi, 84);
    lv_obj_set_flex_flow(kpi, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(kpi, NV_SP_2, 0);
    lv_obj_clear_flag(kpi, LV_OBJ_FLAG_SCROLLABLE);
    s_kpi_cpu   = make_kpi(kpi, "CPU");
    s_kpi_sram  = make_kpi(kpi, nv_tr(NV_STR_SM_INTERNAL));
    s_kpi_psram = make_kpi(kpi, "PSRAM");
    s_kpi_temp  = make_kpi(kpi, nv_tr(NV_STR_TEMPERATURE));
    s_kpi_up    = make_kpi(kpi, nv_tr(NV_STR_SM_UPTIME));

    // ---- main dashboard grid (fills remaining height) ----
    lv_obj_t *grid = lv_obj_create(rootc);
    lv_obj_remove_style_all(grid);
    lv_obj_set_width(grid, lv_pct(100));
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(grid, NV_SP_3, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    // LEFT: CPU (two core arcs over a growing history chart).
    lv_obj_t *cpu = card_base(grid);
    lv_obj_set_flex_grow(cpu, 3);
    lv_obj_set_height(cpu, lv_pct(100));
    lv_obj_set_flex_flow(cpu, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(cpu, NV_SP_2, 0);
    section_title(cpu, "CPU  \xC2\xB7  ESP32-P4 (2\xC3\x97 RISC-V)");

    lv_obj_t *arcs = lv_obj_create(cpu);
    lv_obj_remove_style_all(arcs);
    lv_obj_set_width(arcs, lv_pct(100));
    lv_obj_set_height(arcs, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(arcs, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(arcs, LV_OBJ_FLAG_SCROLLABLE);
    s_g_cpu0 = make_gauge(arcs, "Core 0");
    s_g_cpu1 = make_gauge(arcs, "Core 1");

    lv_obj_t *legend = lv_obj_create(cpu);
    lv_obj_remove_style_all(legend);
    lv_obj_set_size(legend, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(legend, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(legend, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(legend, NV_SP_2, 0);
    lv_obj_clear_flag(legend, LV_OBJ_FLAG_SCROLLABLE);
    legend_dot(legend, CORE0_COLOR, "Core 0");
    legend_dot(legend, CORE1_COLOR, "Core 1  \xC2\xB7  90s");
    s_cpu_chart = make_chart(cpu, CORE0_COLOR, &s_cpu_s0, CORE1_COLOR, &s_cpu_s1);

    // RIGHT: Memory + System.
    lv_obj_t *right = card_base(grid);
    lv_obj_set_flex_grow(right, 2);
    lv_obj_set_height(right, lv_pct(100));
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(right, NV_SP_2, 0);
    section_title(right, nv_tr(NV_STR_SET_MEMORY));
    make_meter(right, nv_tr(NV_STR_SM_INTERNAL), &s_sram_bar, &s_sram_lbl);
    make_meter(right, "PSRAM", &s_psram_bar, &s_psram_lbl);

    lv_obj_t *div = lv_obj_create(right);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, lv_pct(100), 1);
    lv_obj_set_style_bg_color(div, th->divider, 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_set_style_margin_ver(div, NV_SP_1, 0);

    section_title(right, nv_tr(NV_STR_SM_SYSTEM));
    s_sys_val = lv_label_create(right);
    lv_obj_set_style_text_color(s_sys_val, th->text_dim, 0);
    lv_obj_set_width(s_sys_val, lv_pct(100));
    lv_label_set_text(s_sys_val, "");
}

void tick_perf() {
    nv_sys_perf_t p; nv_sysmon_perf(&p);
    nv_sys_mem_t m;  nv_sysmon_mem(&m);
    const NvTheme *th = nv_theme_get();

    int c0 = p.core_load[0] < 0 ? 0 : (int)(p.core_load[0] + 0.5f);
    int c1 = p.core_load[1] < 0 ? 0 : (int)(p.core_load[1] + 0.5f);
    int avg = p.load_avg < 0 ? 0 : (int)(p.load_avg + 0.5f);
    char s0[40], s1[40];
    snprintf(s0, sizeof s0, "%u MHz", (unsigned)p.freq_mhz);
    if (p.temp_valid) snprintf(s1, sizeof s1, "%.0f\xC2\xB0""C", (double)p.temp_c);
    else              snprintf(s1, sizeof s1, "%u MHz", (unsigned)p.freq_mhz);
    gauge_set(s_g_cpu0, c0, s0);
    gauge_set(s_g_cpu1, c1, s1);

    int spct = m.internal.total ? (int)((uint64_t)m.internal.used * 100 / m.internal.total) : 0;
    int ppct = m.psram.total ? (int)((uint64_t)m.psram.used * 100 / m.psram.total) : 0;

    if (s_cpu_chart) {
        lv_chart_set_next_value(s_cpu_chart, s_cpu_s0, c0);
        lv_chart_set_next_value(s_cpu_chart, s_cpu_s1, c1);
    }

    // KPI strip.
    auto set_kpi = [](lv_obj_t *l, const char *txt, lv_color_t col) {
        if (!l) return;
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_color(l, col, 0);
    };
    char b[32];
    snprintf(b, sizeof b, "%d%%", avg);              set_kpi(s_kpi_cpu, b, load_color(avg));
    snprintf(b, sizeof b, "%d%%", spct);             set_kpi(s_kpi_sram, b, load_color(spct));
    snprintf(b, sizeof b, "%d%%", ppct);             set_kpi(s_kpi_psram, b, load_color(ppct));
    if (p.temp_valid) { snprintf(b, sizeof b, "%.0f\xC2\xB0", (double)p.temp_c); set_kpi(s_kpi_temp, b, temp_color(p.temp_c)); }
    else              set_kpi(s_kpi_temp, "-", th->text_dim);
    { unsigned uh = (unsigned)(p.uptime_s / 3600), um = (unsigned)((p.uptime_s % 3600) / 60);
      snprintf(b, sizeof b, "%uh%02um", uh, um); set_kpi(s_kpi_up, b, th->text_strong); }

    // Memory meters.
    if (s_sram_bar) {
        lv_bar_set_value(s_sram_bar, spct, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_sram_bar, load_color(spct), LV_PART_INDICATOR);
        char t[48]; snprintf(t, sizeof t, "%u / %u KB",
                             (unsigned)(m.internal.used / 1024), (unsigned)(m.internal.total / 1024));
        lv_label_set_text(s_sram_lbl, t);
    }
    if (s_psram_bar) {
        lv_bar_set_value(s_psram_bar, ppct, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_psram_bar, load_color(ppct), LV_PART_INDICATOR);
        char t[48]; snprintf(t, sizeof t, "%u / %u MB",
                             (unsigned)(m.psram.used / (1024 * 1024)), (unsigned)(m.psram.total / (1024 * 1024)));
        lv_label_set_text(s_psram_lbl, t);
    }

    if (s_sys_val) {
        unsigned d = (unsigned)(p.uptime_s / 86400), h = (unsigned)((p.uptime_s % 86400) / 3600);
        unsigned mi = (unsigned)((p.uptime_s % 3600) / 60), se = (unsigned)(p.uptime_s % 60);
        char t[224];
        snprintf(t, sizeof t,
                 "%s: %ud %uh %um %us\n%s: %u\n%s: %u MHz\n%s: %u KB",
                 nv_tr(NV_STR_SM_UPTIME), d, h, mi, se,
                 nv_tr(NV_STR_APP_TASKS), (unsigned)p.task_count,
                 nv_tr(NV_STR_SM_FREQ), (unsigned)p.freq_mhz,
                 nv_tr(NV_STR_SM_LARGEST), (unsigned)(m.internal.largest / 1024));
        lv_label_set_text(s_sys_val, t);
    }
}

// ==================================================================== Processes tab
struct ProcRow { lv_obj_t *row, *name, *bar, *cpu, *state, *core, *prio, *stack; };
lv_obj_t *s_proc_list = nullptr, *s_proc_count = nullptr;
ProcRow   s_proc_pool[kMaxRows];
int       s_proc_n = 0;
lv_obj_t *s_sort_chip[3] = {nullptr, nullptr, nullptr};

lv_obj_t *cell(lv_obj_t *row, int grow, lv_color_t col, const lv_font_t *f) {
    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, "");
    if (grow) lv_obj_set_flex_grow(l, grow); else lv_obj_set_width(l, LV_SIZE_CONTENT);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(l, col, 0);
    if (f) lv_obj_set_style_text_font(l, f, 0);
    return l;
}

ProcRow make_proc_row(lv_obj_t *parent) {
    const NvTheme *th = nv_theme_get();
    ProcRow pr;
    pr.row = lv_obj_create(parent);
    lv_obj_remove_style_all(pr.row);
    lv_obj_set_size(pr.row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(pr.row, th->surface, 0);
    lv_obj_set_style_bg_opa(pr.row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pr.row, NV_RAD_SM, 0);
    lv_obj_set_style_pad_ver(pr.row, NV_SP_2, 0);
    lv_obj_set_style_pad_hor(pr.row, NV_SP_3, 0);
    lv_obj_set_style_pad_column(pr.row, NV_SP_2, 0);
    lv_obj_set_flex_flow(pr.row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pr.row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(pr.row, LV_OBJ_FLAG_SCROLLABLE);

    pr.name = cell(pr.row, 3, th->text_strong, nullptr);

    lv_obj_t *cpubox = lv_obj_create(pr.row);
    lv_obj_remove_style_all(cpubox);
    lv_obj_set_flex_grow(cpubox, 3);
    lv_obj_set_height(cpubox, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cpubox, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cpubox, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(cpubox, NV_SP_2, 0);
    lv_obj_clear_flag(cpubox, LV_OBJ_FLAG_SCROLLABLE);
    pr.bar = lv_bar_create(cpubox);
    lv_obj_set_flex_grow(pr.bar, 1);
    lv_obj_set_height(pr.bar, 8);
    lv_bar_set_range(pr.bar, 0, 100);
    lv_obj_set_style_radius(pr.bar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(pr.bar, 4, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(pr.bar, th->surface3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(pr.bar, th->accent, LV_PART_INDICATOR);
    pr.cpu = lv_label_create(cpubox);
    lv_obj_set_width(pr.cpu, 46);
    lv_obj_set_style_text_color(pr.cpu, th->text, 0);

    pr.state = cell(pr.row, 2, th->text_dim, nullptr);
    pr.core  = cell(pr.row, 1, th->text_dim, nullptr);
    pr.prio  = cell(pr.row, 1, th->text_dim, nullptr);
    pr.stack = cell(pr.row, 2, th->text_dim, nullptr);
    return pr;
}

void sort_rows(nv_task_row_t *rows, int n) {
    for (int i = 1; i < n; i++) {
        nv_task_row_t key = rows[i];
        int j = i - 1;
        auto less = [&](const nv_task_row_t &a, const nv_task_row_t &b) {
            if (s_sort == SORT_NAME)  return strcmp(a.name, b.name) > 0;
            if (s_sort == SORT_STACK) return a.stack_free > b.stack_free;
            return a.cpu_pct < b.cpu_pct;
        };
        while (j >= 0 && less(rows[j], key)) { rows[j + 1] = rows[j]; j--; }
        rows[j + 1] = key;
    }
}

void tick_proc() {
    if (!s_proc_list) return;
    const NvTheme *th = nv_theme_get();
    static nv_task_row_t rows[kMaxRows];
    int n = nv_sysmon_tasks(rows, kMaxRows);
    sort_rows(rows, n);

    if (s_proc_count) {
        char c[32]; snprintf(c, sizeof c, "%d %s", n, nv_tr(NV_STR_APP_TASKS));
        lv_label_set_text(s_proc_count, c);
    }

    if (n != s_proc_n) {
        for (int i = s_proc_n; i < n; i++) s_proc_pool[i] = make_proc_row(s_proc_list);
        for (int i = n; i < s_proc_n; i++) { lv_obj_delete(s_proc_pool[i].row); s_proc_pool[i].row = nullptr; }
        s_proc_n = n;
    }

    for (int i = 0; i < n; i++) {
        const nv_task_row_t &t = rows[i];
        ProcRow &pr = s_proc_pool[i];
        int cpct = (int)(t.cpu_pct + 0.5f);
        char cpu[8], core[6], prio[6], stk[12];
        snprintf(cpu, sizeof cpu, "%d%%", cpct);
        if (t.core < 0) snprintf(core, sizeof core, "-"); else snprintf(core, sizeof core, "%d", t.core);
        snprintf(prio, sizeof prio, "%u", (unsigned)t.prio);
        if (t.stack_free >= 1024) snprintf(stk, sizeof stk, "%uK", (unsigned)(t.stack_free / 1024));
        else                      snprintf(stk, sizeof stk, "%uB", (unsigned)t.stack_free);

        lv_label_set_text(pr.name, t.name);
        lv_bar_set_value(pr.bar, cpct, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(pr.bar, load_color(cpct), LV_PART_INDICATOR);
        lv_label_set_text(pr.cpu, cpu);
        lv_label_set_text(pr.state, task_state_str(t.state));
        lv_label_set_text(pr.core, core);
        lv_label_set_text(pr.prio, prio);
        lv_label_set_text(pr.stack, stk);
        lv_obj_set_style_text_color(pr.stack, t.stack_free < (uint32_t)kLowStack ? th->danger : th->text_dim, 0);
    }
}

void restyle_sort_chips() {
    const NvTheme *th = nv_theme_get();
    for (int i = 0; i < 3; i++) {
        if (!s_sort_chip[i]) continue;
        bool on = (s_sort == i);
        lv_obj_set_style_bg_color(s_sort_chip[i], on ? th->primary : th->surface3, 0);
        lv_obj_t *l = lv_obj_get_child(s_sort_chip[i], 0);
        if (l) lv_obj_set_style_text_color(l, on ? th->on_primary : th->text, 0);
    }
}
void sort_chip_cb(lv_event_t *e) {
    s_sort = (int)(intptr_t)lv_event_get_user_data(e);
    restyle_sort_chips();
    s_proc_n = 0;
    if (s_proc_list) lv_obj_clean(s_proc_list);
    for (int i = 0; i < kMaxRows; i++) s_proc_pool[i].row = nullptr;
    tick_proc();
}
lv_obj_t *sort_chip(lv_obj_t *parent, const char *label, int which) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(b, th->surface3, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, NV_RAD_SM, 0);
    lv_obj_set_style_pad_hor(b, NV_SP_3, 0);
    lv_obj_set_style_pad_ver(b, NV_SP_1, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, &nv_font_14, 0);
    lv_obj_set_style_text_color(l, th->text, 0);
    lv_obj_add_event_cb(b, sort_chip_cb, LV_EVENT_CLICKED, (void *)(intptr_t)which);
    return b;
}

void build_proc(lv_obj_t *panel) {
    const NvTheme *th = nv_theme_get();
    s_proc_n = 0;
    for (int i = 0; i < kMaxRows; i++) s_proc_pool[i].row = nullptr;

    lv_obj_t *wrap = lv_obj_create(panel);
    lv_obj_remove_style_all(wrap);
    lv_obj_set_size(wrap, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(wrap, NV_SP_2, 0);
    lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);

    // Toolbar: title + live count (left) | sort chips (right).
    lv_obj_t *tb = lv_obj_create(wrap);
    lv_obj_remove_style_all(tb);
    lv_obj_set_size(tb, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tb, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tb, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(tb, NV_SP_2, 0);
    lv_obj_clear_flag(tb, LV_OBJ_FLAG_SCROLLABLE);
    section_title(tb, nv_tr(NV_STR_SM_PROC));
    s_proc_count = lv_label_create(tb);
    lv_obj_set_style_text_color(s_proc_count, th->text_dim, 0);
    lv_obj_set_flex_grow(s_proc_count, 1);
    s_sort_chip[0] = sort_chip(tb, "CPU", SORT_CPU);
    s_sort_chip[1] = sort_chip(tb, "A-Z", SORT_NAME);
    s_sort_chip[2] = sort_chip(tb, nv_tr(NV_STR_SM_STACK), SORT_STACK);
    restyle_sort_chips();

    // Column header.
    lv_obj_t *hd = lv_obj_create(wrap);
    lv_obj_remove_style_all(hd);
    lv_obj_set_size(hd, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(hd, NV_SP_3, 0);
    lv_obj_set_style_pad_column(hd, NV_SP_2, 0);
    lv_obj_set_flex_flow(hd, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(hd, LV_OBJ_FLAG_SCROLLABLE);
    auto hcell = [&](const char *tx, int grow) {
        lv_obj_t *l = lv_label_create(hd);
        lv_label_set_text(l, tx);
        if (grow) lv_obj_set_flex_grow(l, grow); else lv_obj_set_width(l, LV_SIZE_CONTENT);
        lv_obj_set_style_text_font(l, &nv_font_14, 0);
        lv_obj_set_style_text_color(l, th->text_dim, 0);
    };
    hcell("Name", 3); hcell("CPU", 3); hcell(nv_tr(NV_STR_SM_STATE), 2);
    hcell(nv_tr(NV_STR_SM_CORE), 1); hcell(nv_tr(NV_STR_SM_PRIO), 1); hcell(nv_tr(NV_STR_SM_STACK), 2);

    s_proc_list = nv_kit_scroll_column(wrap);
    lv_obj_set_flex_grow(s_proc_list, 1);
    lv_obj_set_style_pad_row(s_proc_list, NV_SP_1, 0);
}

// ==================================================================== Services tab
struct SvcRow { lv_obj_t *tile, *dot, *name, *badge, *state; };
lv_obj_t *s_svc_list = nullptr, *s_svc_sum = nullptr;
SvcRow    s_svc_pool[kMaxRows];
int       s_svc_n = 0;

SvcRow make_svc_tile(lv_obj_t *parent) {
    const NvTheme *th = nv_theme_get();
    SvcRow sr;
    sr.tile = lv_obj_create(parent);
    lv_obj_remove_style_all(sr.tile);
    lv_obj_set_width(sr.tile, lv_pct(48));
    lv_obj_set_height(sr.tile, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(sr.tile, th->surface, 0);
    lv_obj_set_style_bg_opa(sr.tile, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(sr.tile, NV_RAD_SM, 0);
    lv_obj_set_style_border_width(sr.tile, 1, 0);
    lv_obj_set_style_border_color(sr.tile, th->divider, 0);
    lv_obj_set_style_pad_all(sr.tile, NV_SP_3, 0);
    lv_obj_set_style_pad_column(sr.tile, NV_SP_2, 0);
    lv_obj_set_flex_flow(sr.tile, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sr.tile, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(sr.tile, LV_OBJ_FLAG_SCROLLABLE);

    sr.dot = lv_obj_create(sr.tile);
    lv_obj_remove_style_all(sr.dot);
    lv_obj_set_size(sr.dot, 12, 12);
    lv_obj_set_style_radius(sr.dot, 6, 0);
    lv_obj_set_style_bg_opa(sr.dot, LV_OPA_COVER, 0);

    sr.name = lv_label_create(sr.tile);
    lv_obj_set_flex_grow(sr.name, 1);
    lv_label_set_long_mode(sr.name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(sr.name, th->text_strong, 0);

    sr.badge = lv_label_create(sr.tile);
    lv_label_set_text(sr.badge, nv_tr(NV_STR_SM_ESSENTIAL));
    lv_obj_set_style_text_font(sr.badge, &nv_font_14, 0);
    lv_obj_set_style_text_color(sr.badge, th->accent, 0);

    sr.state = lv_label_create(sr.tile);
    lv_obj_set_style_text_font(sr.state, &nv_font_14, 0);
    lv_obj_set_style_text_color(sr.state, th->text_dim, 0);
    return sr;
}

void build_svc(lv_obj_t *panel) {
    const NvTheme *th = nv_theme_get();
    s_svc_n = 0;
    for (int i = 0; i < kMaxRows; i++) s_svc_pool[i].tile = nullptr;

    lv_obj_t *wrap = lv_obj_create(panel);
    lv_obj_remove_style_all(wrap);
    lv_obj_set_size(wrap, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(wrap, NV_SP_2, 0);
    lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = lv_obj_create(wrap);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hdr, NV_SP_2, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    section_title(hdr, nv_tr(NV_STR_SM_SERVICES));
    s_svc_sum = lv_label_create(hdr);
    lv_obj_set_style_text_color(s_svc_sum, th->text_dim, 0);

    // Two-column wrapping grid of service tiles.
    s_svc_list = lv_obj_create(wrap);
    lv_obj_remove_style_all(s_svc_list);
    lv_obj_set_width(s_svc_list, lv_pct(100));
    lv_obj_set_flex_grow(s_svc_list, 1);
    lv_obj_set_flex_flow(s_svc_list, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(s_svc_list, NV_SP_2, 0);
    lv_obj_set_style_pad_column(s_svc_list, NV_SP_2, 0);
    lv_obj_set_scroll_dir(s_svc_list, LV_DIR_VER);
}

void tick_svc() {
    if (!s_svc_list) return;
    const NvTheme *th = nv_theme_get();
    static nv_svc_row_t rows[kMaxRows];
    int n = nv_sysmon_services(rows, kMaxRows);

    int running = 0;
    for (int i = 0; i < n; i++) if (rows[i].state == 1) running++;
    if (s_svc_sum) {
        char c[48]; snprintf(c, sizeof c, "%d %s \xC2\xB7 %d", running, nv_tr(NV_STR_RUNNING), n);
        lv_label_set_text(s_svc_sum, c);
    }

    if (n != s_svc_n) {
        for (int i = s_svc_n; i < n; i++) s_svc_pool[i] = make_svc_tile(s_svc_list);
        for (int i = n; i < s_svc_n; i++) { lv_obj_delete(s_svc_pool[i].tile); s_svc_pool[i].tile = nullptr; }
        s_svc_n = n;
    }
    for (int i = 0; i < n; i++) {
        const nv_svc_row_t &s = rows[i];
        SvcRow &sr = s_svc_pool[i];
        lv_obj_set_style_bg_color(sr.dot,
            s.state == 1 ? th->success_solid : (s.state == 2 ? th->accent : th->text_disabled), 0);
        lv_label_set_text(sr.name, s.name);
        if (s.essential) lv_obj_clear_flag(sr.badge, LV_OBJ_FLAG_HIDDEN);
        else             lv_obj_add_flag(sr.badge, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(sr.state, svc_state_str(s.state));
    }
}

// ==================================================================== tabs + timer
void tick(lv_timer_t *) {
    switch (s_tab) {
        case TAB_PERF: tick_perf(); break;
        case TAB_PROC: tick_proc(); break;
        case TAB_SVC:  tick_svc();  break;
    }
}

void rail_style(int idx, bool active) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *b = s_rail_btn[idx];
    if (!b) return;
    lv_obj_set_style_bg_color(b, active ? th->primary : th->surface, 0);
    lv_obj_t *lbl = lv_obj_get_child(b, 0);
    if (lbl) lv_obj_set_style_text_color(lbl, active ? th->on_primary : th->text, 0);
}

void reset_tab_refs() {
    clear_perf_refs();
    s_proc_list = nullptr; s_proc_count = nullptr; s_proc_n = 0;
    s_svc_list = nullptr;  s_svc_sum = nullptr; s_svc_n = 0;
    for (int i = 0; i < kMaxRows; i++) { s_proc_pool[i].row = nullptr; s_svc_pool[i].tile = nullptr; }
    s_sort_chip[0] = s_sort_chip[1] = s_sort_chip[2] = nullptr;
}

void show_tab(int tab) {
    s_tab = tab;
    reset_tab_refs();
    if (s_panel) lv_obj_clean(s_panel);
    for (int i = 0; i < 3; i++) rail_style(i, i == tab);
    switch (tab) {
        case TAB_PERF: build_perf(s_panel); break;
        case TAB_PROC: build_proc(s_panel); break;
        case TAB_SVC:  build_svc(s_panel);  break;
    }
    tick(nullptr);
}

void rail_cb(lv_event_t *e) { show_tab((int)(intptr_t)lv_event_get_user_data(e)); }

void page_deleted(lv_event_t *) {
    if (s_timer) { lv_timer_delete(s_timer); s_timer = nullptr; }
    reset_tab_refs();
    s_panel = nullptr;
    s_rail_btn[0] = s_rail_btn[1] = s_rail_btn[2] = nullptr;
}

void rail_button(lv_obj_t *rail, const char *label, int idx) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *b = lv_obj_create(rail);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(b, th->surface, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, NV_RAD_SM, 0);
    lv_obj_set_style_pad_all(b, NV_SP_3, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_color(l, th->text, 0);
    lv_obj_add_event_cb(b, rail_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    s_rail_btn[idx] = b;
}

void sysmon_build(lv_obj_t *content) {
    s_tab = TAB_PERF;
    reset_tab_refs();

    lv_obj_t *root = lv_obj_create(content);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(root, NV_SP_3, 0);
    lv_obj_set_style_pad_column(root, NV_SP_3, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, page_deleted, LV_EVENT_DELETE, nullptr);

    lv_obj_t *rail = lv_obj_create(root);
    lv_obj_remove_style_all(rail);
    lv_obj_set_size(rail, 150, lv_pct(100));
    lv_obj_set_style_pad_row(rail, NV_SP_2, 0);
    lv_obj_set_flex_flow(rail, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(rail, LV_OBJ_FLAG_SCROLLABLE);
    rail_button(rail, nv_tr(NV_STR_SM_PERF), TAB_PERF);
    rail_button(rail, nv_tr(NV_STR_SM_PROC), TAB_PROC);
    rail_button(rail, nv_tr(NV_STR_SM_SERVICES), TAB_SVC);

    s_panel = lv_obj_create(root);
    lv_obj_remove_style_all(s_panel);
    lv_obj_set_flex_grow(s_panel, 1);
    lv_obj_set_height(s_panel, lv_pct(100));
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    show_tab(TAB_PERF);
    s_timer = lv_timer_create(tick, 1000, nullptr);
}

const NvApp kSysmonApp = {"sysmon", "System Monitor", &nv_icon_sysmon, 768u << 10, sysmon_build,
                          NV_STR_APP_SYSMON, nullptr};

}  // namespace

void sysmon_app_register(void) { nv_app_register(&kSysmonApp); }
