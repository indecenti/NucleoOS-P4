// nv_hid_report — generic HID report-descriptor parser + decoder for pointing devices, keyboards and
// consumer controls (media keys), shared by USB and Bluetooth LE (HOGP) input.
//
// The boot protocol (8-bit mouse deltas, 6-key keyboard, no wheel, no media keys) is a BIOS fallback:
// in it many Bluetooth mice lower their rate and resolution and saturate on fast moves. Running devices
// in report protocol needs their own report format, read from the report map: this module walks it
// (global/local items, Push/Pop, Report IDs, extended usages) and keeps, per input report, where the
// fields we act on live. Pure C, no allocation, no dependencies.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NV_HIDR_MAX_REPORTS 8
#define NV_HIDR_CONS_BITS   16

enum { NV_HIDR_MOUSE = 1, NV_HIDR_KEYBOARD = 2, NV_HIDR_CONSUMER = 4 };

typedef struct { uint16_t off; uint8_t size; bool sign; } nv_hidr_field_t;   // bit offset after the ID byte

typedef struct {
    uint8_t  id;                       // Report ID (0 = the device uses none)
    uint8_t  roles;                    // NV_HIDR_* found in this report
    // pointer (relative): buttons 1..nbtn as consecutive bits, X / Y / wheel / horizontal pan
    nv_hidr_field_t btn, x, y, wheel, pan;
    uint8_t  nbtn;
    // keyboard: modifier bitmap (E0..E7), key array (6KRO style) and/or key bitmap (NKRO)
    nv_hidr_field_t mods, keys, keybits;
    uint8_t  nkeys, keys_umin, keybits_umin;
    int32_t  keys_lmin;
    uint16_t keybits_n;
    // consumer control: an array of usages, and/or single-bit usages (Volume Up, Mute...)
    nv_hidr_field_t cons;
    uint8_t  ncons;
    uint16_t cons_umin;
    int32_t  cons_lmin;
    uint8_t  ncbits;
    struct { uint16_t off; uint16_t usage; } cbits[NV_HIDR_CONS_BITS];
} nv_hidr_report_t;

typedef struct {
    uint8_t n;                         // input reports described
    bool    ids;                       // the device numbers its reports
    uint8_t roles;                     // union of every report's roles
    nv_hidr_report_t r[NV_HIDR_MAX_REPORTS];
} nv_hidr_layout_t;

// Parse a report descriptor. True when at least one mouse / keyboard / consumer report was found.
bool nv_hidr_parse(const uint8_t *desc, size_t n, nv_hidr_layout_t *out);
// The input report with this ID (`id` 0 on a device without IDs), NULL when unknown.
const nv_hidr_report_t *nv_hidr_find(const nv_hidr_layout_t *l, uint8_t id);

// Decoders; `d` / `len` are the report payload WITHOUT the Report ID byte.
void nv_hidr_mouse(const nv_hidr_report_t *r, const uint8_t *d, size_t len,
                   uint8_t *buttons, int32_t *dx, int32_t *dy, int32_t *wheel, int32_t *pan);
// The keyboard state as a boot report: [0] modifiers, [1] 0, [2..7] up to six pressed usages.
void nv_hidr_keyboard(const nv_hidr_report_t *r, const uint8_t *d, size_t len, uint8_t boot[8]);
// Consumer usages held now (0x00E9 Volume Up, 0x00CD Play/Pause...); returns how many (<= max).
int  nv_hidr_consumer(const nv_hidr_report_t *r, const uint8_t *d, size_t len, uint16_t *usages, int max);

#ifdef __cplusplus
}
#endif
