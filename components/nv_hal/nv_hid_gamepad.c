// nv_hid_gamepad — see nv_hid_gamepad.h. Item encoding per the USB HID 1.11 spec, section 6.2.2.
//
// Axis order follows the Linux input layer (hid-input.c), which is also what SDL's Linux, Mac and
// DirectInput backends end up with for HID pads: the SDL_GameControllerDB "aN" indices count the
// present axes sorted by that code.
#include "nv_hid_gamepad.h"

#include <string.h>

enum { TYPE_MAIN = 0, TYPE_GLOBAL = 1, TYPE_LOCAL = 2 };
enum { PAGE_DESKTOP = 0x01, PAGE_SIM = 0x02, PAGE_BUTTON = 0x09 };
enum { U_JOYSTICK = 0x04, U_GAMEPAD = 0x05, U_MULTIAXIS = 0x08, U_X = 0x30, U_WHEEL = 0x38,
       U_HAT = 0x39, U_DPAD_UP = 0x90 };   // D-pad: 0x90 up, 0x91 down, 0x92 right, 0x93 left

typedef struct { uint16_t page; int32_t lmin, lmax; uint32_t rsize, rcount; uint8_t rid; } Globals;

#define MAX_USAGES 16
#define MAX_IDS    16
#define MAX_PUSH   4
#define ABS_MISC   0x28

static int32_t item_signed(const uint8_t *d, int n) {
    if (n == 1) return (int8_t)d[0];
    if (n == 2) return (int16_t)(d[0] | d[1] << 8);
    if (n == 4) return (int32_t)((uint32_t)d[0] | (uint32_t)d[1] << 8 | (uint32_t)d[2] << 16 | (uint32_t)d[3] << 24);
    return 0;
}

static uint32_t item_unsigned(const uint8_t *d, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v |= (uint32_t)d[i] << (8 * i);
    return v;
}

// Linux ABS code of an axis usage, -1 = not an axis we map.
static int abs_code(uint16_t page, uint16_t id) {
    if (page == PAGE_DESKTOP && id >= U_X && id <= U_WHEEL) return id - U_X;   // X..Rz 0-5, Slider 6, Dial 7, Wheel 8
    if (page == PAGE_SIM) {
        switch (id) {
        case 0xBA: return 7;    // Rudder -> ABS_RUDDER
        case 0xBB: return 6;    // Throttle -> ABS_THROTTLE
        case 0xC4: return 9;    // Accelerator -> ABS_GAS (Xbox BLE right trigger)
        case 0xC5: return 10;   // Brake -> ABS_BRAKE (Xbox BLE left trigger)
        case 0xC8: return 8;    // Steering -> ABS_WHEEL
        default: return -1;
        }
    }
    return -1;
}

bool nv_hid_pad_parse(const uint8_t *desc, size_t len, nv_hid_pad_layout_t *out) {
    memset(out, 0, sizeof *out);
    Globals g = {0}, stack[MAX_PUSH];
    int sp = 0;
    uint32_t usages[MAX_USAGES];   // (page << 16) | usage
    int n_usages = 0;
    uint32_t umin = 0, umax = 0;
    bool range = false;
    struct { uint8_t id; uint16_t bits; } offs[MAX_IDS];   // input bit offset per report ID
    int n_offs = 0;
    int depth = 0, pad_depth = -1;   // collection nesting; depth of the gamepad collection
    bool have_id = false;            // the pad's report ID is fixed by its first recorded field

    // Collected unsorted, then ordered: axes by ABS code, buttons by usage.
    nv_hid_field_t axes[NV_HID_MAX_AXES];
    uint8_t codes[NV_HID_MAX_AXES];
    int n_axes = 0;
    nv_hid_field_t btns[NV_HID_MAX_BUTTONS];
    uint16_t bids[NV_HID_MAX_BUTTONS];
    int n_btns = 0;

    for (size_t i = 0; i < len;) {
        const uint8_t p = desc[i];
        if (p == 0xfe) {                                   // long item: skip
            if (i + 2 >= len) break;
            i += 3 + desc[i + 1];
            continue;
        }
        const int n = (p & 3) == 3 ? 4 : (p & 3);
        const int type = (p >> 2) & 3, tag = p >> 4;
        if (i + 1 + n > len) break;
        const uint8_t *d = desc + i + 1;
        i += 1 + n;

        if (type == TYPE_GLOBAL) {
            switch (tag) {
            case 0: g.page = (uint16_t)item_unsigned(d, n); break;
            case 1: g.lmin = item_signed(d, n); break;
            case 2:
                g.lmax = item_signed(d, n);
                // A common descriptor bug: 255 written in one byte reads as -1. The range is
                // unsigned when the minimum is.
                if (g.lmax < g.lmin && g.lmin >= 0) g.lmax = (int32_t)item_unsigned(d, n);
                break;
            case 7: g.rsize = item_unsigned(d, n); break;
            case 8: g.rid = (uint8_t)item_unsigned(d, n); break;
            case 9: g.rcount = item_unsigned(d, n); break;
            case 10: if (sp < MAX_PUSH) stack[sp++] = g; break;
            case 11: if (sp > 0) g = stack[--sp]; break;
            default: break;
            }
            continue;
        }
        if (type == TYPE_LOCAL) {
            const uint32_t u = n == 4 ? item_unsigned(d, n) : ((uint32_t)g.page << 16 | item_unsigned(d, n));
            if (tag == 0 && n_usages < MAX_USAGES) usages[n_usages++] = u;
            else if (tag == 1) { umin = u; range = true; }
            else if (tag == 2) { umax = u; range = true; }
            continue;
        }
        if (type != TYPE_MAIN) continue;

        if (tag == 10) {                                   // Collection
            const uint32_t u = n_usages ? usages[0] : 0;
            if (pad_depth < 0 && item_unsigned(d, n) == 1 && (u >> 16) == PAGE_DESKTOP &&
                ((u & 0xffff) == U_JOYSTICK || (u & 0xffff) == U_GAMEPAD || (u & 0xffff) == U_MULTIAXIS))
                pad_depth = depth;
            depth++;
        } else if (tag == 12) {                            // End Collection
            if (depth > 0) depth--;
            if (pad_depth >= 0 && depth == pad_depth) break;   // the first gamepad is enough
        } else if (tag == 8) {                             // Input
            int k = 0;
            while (k < n_offs && offs[k].id != g.rid) k++;
            if (k == n_offs) {
                if (n_offs == MAX_IDS) break;
                offs[n_offs].id = g.rid;
                offs[n_offs++].bits = 0;
            }
            const uint32_t flags = item_unsigned(d, n);
            const bool usable = pad_depth >= 0 && !(flags & 1) && (flags & 2) &&   // data, variable
                                g.rsize >= 1 && g.rsize <= 32 && (!have_id || g.rid == out->report_id);
            for (uint32_t f = 0; f < g.rcount && f < 256; f++) {
                const uint16_t off = (uint16_t)(offs[k].bits + f * g.rsize);
                uint32_t u = 0;
                if (n_usages) u = usages[f < (uint32_t)n_usages ? f : (uint32_t)n_usages - 1];
                else if (range) u = umin + f <= umax ? umin + f : umax;
                if (!usable || !u) continue;
                const nv_hid_field_t fld = { off, (uint8_t)g.rsize, g.lmin, g.lmax };
                const uint16_t page = (uint16_t)(u >> 16), id = (uint16_t)(u & 0xffff);
                bool took = false;
                const int code = abs_code(page, id);
                if (code >= 0 && n_axes < NV_HID_MAX_AXES) {
                    // Linux gives a repeated usage the next free code from ABS_MISC on.
                    int c = code;
                    bool taken = false;
                    for (int a = 0; a < n_axes; a++) if (codes[a] == c) taken = true;
                    if (taken) {
                        c = ABS_MISC;
                        for (int a = 0; a < n_axes; a++) if (codes[a] >= c) c = codes[a] + 1;
                    }
                    axes[n_axes] = fld;
                    codes[n_axes++] = (uint8_t)c;
                    if (page == PAGE_SIM && (id == 0xC4 || id == 0xC5)) out->sim_triggers = true;
                    took = true;
                } else if (page == PAGE_DESKTOP && id == U_HAT && out->n_hats < NV_HID_MAX_HATS) {
                    out->hat[out->n_hats++] = fld;
                    took = true;
                } else if (page == PAGE_DESKTOP && id >= U_DPAD_UP && id < U_DPAD_UP + 4) {
                    if (!out->dpad[id - U_DPAD_UP].size) { out->dpad[id - U_DPAD_UP] = fld; took = true; }
                } else if (page == PAGE_BUTTON && id >= 1 && n_btns < NV_HID_MAX_BUTTONS) {
                    bool dup = false;
                    for (int b = 0; b < n_btns; b++) if (bids[b] == id) dup = true;
                    if (!dup) { btns[n_btns] = fld; bids[n_btns++] = id; took = true; }
                }
                if (took) {
                    out->report_id = g.rid;
                    have_id = true;
                }
            }
            offs[k].bits = (uint16_t)(offs[k].bits + g.rcount * g.rsize);
        }
        n_usages = 0;                                      // locals end with every main item
        range = false;
        umin = umax = 0;
    }

    // Insertion sorts: axes by ABS code, buttons by usage (both tiny).
    for (int a = 1; a < n_axes; a++)
        for (int b = a; b > 0 && codes[b - 1] > codes[b]; b--) {
            const nv_hid_field_t f = axes[b]; axes[b] = axes[b - 1]; axes[b - 1] = f;
            const uint8_t c = codes[b]; codes[b] = codes[b - 1]; codes[b - 1] = c;
        }
    for (int a = 1; a < n_btns; a++)
        for (int b = a; b > 0 && bids[b - 1] > bids[b]; b--) {
            const nv_hid_field_t f = btns[b]; btns[b] = btns[b - 1]; btns[b - 1] = f;
            const uint16_t c = bids[b]; bids[b] = bids[b - 1]; bids[b - 1] = c;
        }
    memcpy(out->axis, axes, sizeof axes[0] * n_axes);
    memcpy(out->axis_code, codes, (size_t)n_axes);
    memcpy(out->btn, btns, sizeof btns[0] * n_btns);
    out->n_axes = (uint8_t)n_axes;
    out->n_buttons = (uint8_t)n_btns;
    // A D-pad made of usages acts as hat 0 when there is no real hat.
    if (!out->n_hats && out->dpad[0].size) out->n_hats = 1;

    const bool steer = n_axes >= 2 || out->n_hats;
    return steer && n_btns > 0;
}

static uint32_t get_bits(const uint8_t *p, size_t len, unsigned off, unsigned size) {
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) {
        const unsigned b = off + i;
        if (b / 8 >= len) return 0;
        if ((p[b / 8] >> (b % 8)) & 1) v |= 1u << i;
    }
    return v;
}

static int32_t field_value(const nv_hid_field_t *f, const uint8_t *p, size_t len) {
    uint32_t v = get_bits(p, len, f->off, f->size);
    if (f->lmin < 0 && f->size < 32 && (v >> (f->size - 1)) & 1) v |= ~0u << f->size;   // sign-extend
    return (int32_t)v;
}

// Logical range -> -32768..32767 (clamped: some pads report past their declared range).
static int16_t axis_value(const nv_hid_field_t *f, const uint8_t *p, size_t len) {
    const int64_t range = (int64_t)f->lmax - f->lmin;
    if (!f->size || range <= 0) return 0;
    int64_t v = (int64_t)field_value(f, p, len) - f->lmin;
    if (v < 0) v = 0;
    if (v > range) v = range;
    return (int16_t)(v * 65535 / range - 32768);
}

static uint8_t hat_mask(const nv_hid_field_t *f, const uint8_t *p, size_t len) {
    static const uint8_t k8[8] = { 1, 1 | 2, 2, 4 | 2, 4, 4 | 8, 8, 1 | 8 };
    static const uint8_t k4[4] = { 1, 2, 4, 8 };
    const int64_t count = (int64_t)f->lmax - f->lmin + 1;
    const int64_t v = (int64_t)field_value(f, p, len) - f->lmin;   // out of range = centred
    if (count == 8 && v >= 0 && v < 8) return k8[v];
    if (count == 4 && v >= 0 && v < 4) return k4[v];
    return 0;
}

bool nv_hid_pad_decode(const nv_hid_pad_layout_t *l, const uint8_t *report, size_t len, nv_hid_raw_t *out) {
    if (l->report_id) {
        if (len < 1 || report[0] != l->report_id) return false;
        report++;
        len--;
    }
    memset(out, 0, sizeof *out);
    for (int i = 0; i < l->n_axes; i++) out->axis[i] = axis_value(&l->axis[i], report, len);
    for (int i = 0; i < l->n_hats; i++) {
        if (l->hat[i].size) { out->hat[i] = hat_mask(&l->hat[i], report, len); continue; }
        static const uint8_t kd[4] = { 1, 4, 2, 8 };   // D-pad usages up, down, right, left
        for (int k = 0; k < 4; k++)
            if (l->dpad[k].size && get_bits(report, len, l->dpad[k].off, l->dpad[k].size)) out->hat[i] |= kd[k];
    }
    for (int i = 0; i < l->n_buttons; i++)
        if (get_bits(report, len, l->btn[i].off, l->btn[i].size)) out->buttons |= 1ull << i;
    return true;
}
