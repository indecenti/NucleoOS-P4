// nv_hid_gamepad — see nv_hid_gamepad.h. Item encoding per the USB HID 1.11 spec, section 6.2.2.
#include "nv_hid_gamepad.h"

#include <string.h>

enum { TYPE_MAIN = 0, TYPE_GLOBAL = 1, TYPE_LOCAL = 2 };
enum { PAGE_DESKTOP = 0x01, PAGE_BUTTON = 0x09 };
enum { U_JOYSTICK = 0x04, U_GAMEPAD = 0x05, U_MULTIAXIS = 0x08, U_X = 0x30, U_Y = 0x31,
       U_HAT = 0x39, U_DPAD_UP = 0x90 };   // D-pad: 0x90 up, 0x91 down, 0x92 right, 0x93 left

typedef struct { uint16_t page; int32_t lmin, lmax; uint32_t rsize, rcount; uint8_t rid; } Globals;

#define MAX_USAGES 16
#define MAX_IDS    16
#define MAX_PUSH   4

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
                nv_hid_field_t *dst = NULL;
                if (page == PAGE_DESKTOP) {
                    if (id == U_X) dst = &out->x;
                    else if (id == U_Y) dst = &out->y;
                    else if (id == U_HAT) dst = &out->hat;
                    else if (id >= U_DPAD_UP && id < U_DPAD_UP + 4) dst = &out->dpad[id - U_DPAD_UP];
                } else if (page == PAGE_BUTTON && id >= 1 && id <= NV_HID_PAD_BUTTONS) {
                    dst = &out->btn[id - 1];
                    if (id > out->n_buttons) out->n_buttons = (uint8_t)id;
                }
                if (dst && !dst->size) {
                    *dst = fld;
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

    const bool steer = (out->x.size && out->y.size) || out->hat.size || out->dpad[0].size;
    return steer && out->n_buttons > 0;
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

// -1 / 0 / +1 for a stick axis, with a dead zone of 40% of the half range around the centre.
static int axis_dir(const nv_hid_field_t *f, const uint8_t *p, size_t len) {
    const int64_t range = (int64_t)f->lmax - f->lmin;
    if (!f->size || range <= 0) return 0;
    const int64_t v = field_value(f, p, len), c2 = (int64_t)f->lmin + f->lmax;   // 2 x centre
    const int64_t dz2 = range * 4 / 10;                                           // 2 x dead zone
    if (2 * v < c2 - dz2) return -1;
    if (2 * v > c2 + dz2) return 1;
    return 0;
}

bool nv_hid_pad_decode(const nv_hid_pad_layout_t *l, const uint8_t *report, size_t len,
                       uint8_t *dirs, uint32_t *buttons) {
    if (l->report_id) {
        if (len < 1 || report[0] != l->report_id) return false;
        report++;
        len--;
    }
    uint8_t d = 0;
    const int ax = axis_dir(&l->x, report, len), ay = axis_dir(&l->y, report, len);
    if (ax < 0) d |= NV_PAD_LEFT;
    if (ax > 0) d |= NV_PAD_RIGHT;
    if (ay < 0) d |= NV_PAD_UP;
    if (ay > 0) d |= NV_PAD_DOWN;
    if (l->hat.size) {
        static const uint8_t k8[8] = { NV_PAD_UP, NV_PAD_UP | NV_PAD_RIGHT, NV_PAD_RIGHT,
                                       NV_PAD_DOWN | NV_PAD_RIGHT, NV_PAD_DOWN, NV_PAD_DOWN | NV_PAD_LEFT,
                                       NV_PAD_LEFT, NV_PAD_UP | NV_PAD_LEFT };
        static const uint8_t k4[4] = { NV_PAD_UP, NV_PAD_RIGHT, NV_PAD_DOWN, NV_PAD_LEFT };
        const int64_t count = (int64_t)l->hat.lmax - l->hat.lmin + 1;
        const int64_t v = (int64_t)field_value(&l->hat, report, len) - l->hat.lmin;   // out of range = centred
        if (count == 8 && v >= 0 && v < 8) d |= k8[v];
        if (count == 4 && v >= 0 && v < 4) d |= k4[v];
    }
    static const uint8_t kd[4] = { NV_PAD_UP, NV_PAD_DOWN, NV_PAD_RIGHT, NV_PAD_LEFT };
    for (int i = 0; i < 4; i++)
        if (l->dpad[i].size && get_bits(report, len, l->dpad[i].off, l->dpad[i].size)) d |= kd[i];
    uint32_t b = 0;
    for (int i = 0; i < l->n_buttons; i++)
        if (l->btn[i].size && get_bits(report, len, l->btn[i].off, l->btn[i].size)) b |= 1u << i;
    *dirs = d;
    *buttons = b;
    return true;
}
