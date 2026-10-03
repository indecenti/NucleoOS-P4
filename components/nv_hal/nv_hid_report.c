// nv_hid_report — see include/nv_hid_report.h.
#include "nv_hid_report.h"

#include <string.h>

#define MAX_USAGES 32
#define STACK_MAX  4

typedef struct {
    uint16_t page;
    int32_t  lmin, lmax;
    uint8_t  size;
    uint16_t count;
    uint8_t  id;
} globals_t;

static nv_hidr_report_t *report_for(nv_hidr_layout_t *l, uint16_t *offs, uint8_t id, uint16_t **off) {
    for (int i = 0; i < l->n; i++)
        if (l->r[i].id == id) { *off = &offs[i]; return &l->r[i]; }
    if (l->n >= NV_HIDR_MAX_REPORTS) return NULL;
    nv_hidr_report_t *r = &l->r[l->n];
    memset(r, 0, sizeof *r);
    r->id = id;
    offs[l->n] = 0;
    *off = &offs[l->n];
    l->n++;
    return r;
}

static void on_input(nv_hidr_layout_t *l, uint16_t *offs, const globals_t *g, uint32_t flags,
                     const uint32_t *usages, int nu, bool range, uint32_t umin, uint32_t umax) {
    uint16_t *off;
    nv_hidr_report_t *r = report_for(l, offs, g->id, &off);
    if (!r) return;
    const uint32_t bits = (uint32_t)g->size * g->count;
    const bool constant = flags & 1, variable = flags & 2, relative = flags & 4;
    const bool sign = g->lmin < 0;
    if (constant || !g->size || !g->count) { *off = (uint16_t)(*off + bits); return; }

    if (variable) {
        for (uint16_t f = 0; f < g->count; f++) {
            uint32_t u = 0;
            if (range) { u = umin + f; if (umax && u > umax) u = umax; }
            else if (nu) u = usages[f < nu ? f : nu - 1];
            const uint16_t page = (uint16_t)(u >> 16), use = (uint16_t)u;
            const nv_hidr_field_t fld = { (uint16_t)(*off + f * g->size), g->size, sign };
            if (page == 0x09 && g->size == 1 && use >= 1) {                       // buttons
                if (!r->nbtn) { r->btn = fld; r->btn.sign = false; r->nbtn = 1; }
                else if (fld.off == r->btn.off + r->nbtn && r->nbtn < 8) r->nbtn++;
            } else if (page == 0x01 && relative && use == 0x30) r->x = fld;      // X
            else if (page == 0x01 && relative && use == 0x31) r->y = fld;        // Y
            else if (page == 0x01 && use == 0x38) r->wheel = fld;                // wheel
            else if (page == 0x0C && use == 0x238) r->pan = fld;                 // AC Pan (tilt)
            else if (page == 0x07 && g->size == 1) {                             // keyboard bits
                if (use == 0xE0) { r->mods = fld; r->mods.size = 8; r->mods.sign = false; r->roles |= NV_HIDR_KEYBOARD; }
                else if (use < 0xE0 && use > 0) {                                // NKRO bitmap
                    if (!r->keybits_n) { r->keybits = fld; r->keybits.sign = false; r->keybits_umin = (uint8_t)use; r->keybits_n = 1; }
                    else if (fld.off == r->keybits.off + r->keybits_n) r->keybits_n++;
                    r->roles |= NV_HIDR_KEYBOARD;
                }
            } else if (page == 0x0C && g->size == 1 && use && r->ncbits < NV_HIDR_CONS_BITS) {   // media key bits
                r->cbits[r->ncbits].off = fld.off;
                r->cbits[r->ncbits].usage = use;
                r->ncbits++;
                r->roles |= NV_HIDR_CONSUMER;
            }
        }
    } else {                                                                     // array of usages
        const uint32_t first = range ? umin : (nu ? usages[0] : 0);
        const uint16_t page = (uint16_t)(first >> 16);
        const nv_hidr_field_t fld = { *off, g->size, false };
        if (page == 0x07 && !r->nkeys) {
            r->keys = fld; r->nkeys = g->count > 32 ? 32 : (uint8_t)g->count;
            r->keys_umin = (uint8_t)first; r->keys_lmin = g->lmin;
            r->roles |= NV_HIDR_KEYBOARD;
        } else if (page == 0x0C && !r->ncons) {
            r->cons = fld; r->ncons = g->count > 8 ? 8 : (uint8_t)g->count;
            r->cons_umin = (uint16_t)first; r->cons_lmin = g->lmin;
            r->roles |= NV_HIDR_CONSUMER;
        }
    }
    *off = (uint16_t)(*off + bits);
}

bool nv_hidr_parse(const uint8_t *d, size_t n, nv_hidr_layout_t *out) {
    memset(out, 0, sizeof *out);
    uint16_t offs[NV_HIDR_MAX_REPORTS] = {0};
    globals_t g = {0}, stack[STACK_MAX];
    int sp = 0;
    uint32_t usages[MAX_USAGES];
    int nu = 0;
    uint32_t umin = 0, umax = 0;
    bool range = false;

    for (size_t i = 0; i < n;) {
        const uint8_t b = d[i];
        if (b == 0xFE) {                                         // long item: skip
            if (i + 2 >= n) break;
            i += 3 + (size_t)d[i + 1];
            continue;
        }
        const uint8_t sz = (b & 3) == 3 ? 4 : (b & 3);
        if (i + 1 + sz > n) break;
        uint32_t v = 0;
        for (int k = 0; k < sz; k++) v |= (uint32_t)d[i + 1 + k] << (8 * k);
        int32_t sv = (int32_t)v;                                 // sign-extended for logical min/max
        if (sz == 1) sv = (int8_t)v; else if (sz == 2) sv = (int16_t)v;
        // A 1-2 byte usage takes the current usage page; a 4-byte one carries its own.
        const uint32_t ext = sz == 4 ? v : ((uint32_t)g.page << 16) | (v & 0xFFFF);
        switch (b & 0xFC) {
        // global items
        case 0x04: g.page = (uint16_t)v; break;
        case 0x14: g.lmin = sv; break;
        case 0x24: g.lmax = sv; break;
        case 0x74: g.size = (uint8_t)(v > 32 ? 32 : v); break;
        case 0x94: g.count = (uint16_t)v; break;
        case 0x84: g.id = (uint8_t)v; out->ids = true; break;
        case 0xA4: if (sp < STACK_MAX) stack[sp++] = g; break;   // Push
        case 0xB4: if (sp > 0) g = stack[--sp]; break;           // Pop
        // local items
        case 0x08: if (nu < MAX_USAGES) usages[nu++] = ext; break;
        case 0x18: umin = ext; range = true; break;
        case 0x28: umax = ext; range = true; break;
        // main items
        case 0x80:
            on_input(out, offs, &g, v, usages, nu, range, umin, umax);
            nu = 0; range = false; umin = umax = 0;
            break;
        case 0x90: case 0xB0: case 0xA0: case 0xC0:              // Output / Feature / Collection / End
            nu = 0; range = false; umin = umax = 0;
            break;
        default: break;
        }
        i += 1 + sz;
    }
    // Roles: a pointer needs relative X and Y; drop reports we cannot act on.
    int k = 0;
    for (int i = 0; i < out->n; i++) {
        nv_hidr_report_t *r = &out->r[i];
        if (r->x.size && r->y.size && r->x.size <= 32 && r->y.size <= 32) r->roles |= NV_HIDR_MOUSE;
        if (!r->roles) continue;
        if (k != i) out->r[k] = *r;
        out->roles |= out->r[k].roles;
        k++;
    }
    out->n = (uint8_t)k;
    return k > 0;
}

const nv_hidr_report_t *nv_hidr_find(const nv_hidr_layout_t *l, uint8_t id) {
    for (int i = 0; i < l->n; i++) if (l->r[i].id == id) return &l->r[i];
    return NULL;
}

static int32_t get(const uint8_t *d, size_t len, uint16_t off, uint8_t size, bool sign) {
    if (!size || (size_t)off + size > len * 8) return 0;
    uint32_t v = 0;
    for (int b = 0; b < size; b++)
        if (d[(off + b) >> 3] & (1u << ((off + b) & 7))) v |= 1u << b;
    if (sign && size < 32 && (v & (1u << (size - 1)))) v |= ~0u << size;
    return (int32_t)v;
}
static int32_t fget(const uint8_t *d, size_t len, nv_hidr_field_t f) { return get(d, len, f.off, f.size, f.sign); }

void nv_hidr_mouse(const nv_hidr_report_t *r, const uint8_t *d, size_t len,
                   uint8_t *buttons, int32_t *dx, int32_t *dy, int32_t *wheel, int32_t *pan) {
    *buttons = (uint8_t)get(d, len, r->btn.off, r->nbtn, false);
    *dx = fget(d, len, r->x);
    *dy = fget(d, len, r->y);
    *wheel = fget(d, len, r->wheel);
    *pan = fget(d, len, r->pan);
}

void nv_hidr_keyboard(const nv_hidr_report_t *r, const uint8_t *d, size_t len, uint8_t boot[8]) {
    memset(boot, 0, 8);
    if (r->mods.size) boot[0] = (uint8_t)get(d, len, r->mods.off, 8, false);
    int k = 2;
    for (int i = 0; i < r->nkeys; i++) {
        const int32_t v = get(d, len, (uint16_t)(r->keys.off + i * r->keys.size), r->keys.size, false);
        if (!v) continue;
        const int32_t u = r->keys_umin + (v - r->keys_lmin);
        if (u == 1) { boot[2] = 1; return; }                     // ErrorRollOver: too many keys
        if (u <= 3) continue;                                    // reserved / POST fail / undefined
        if (u >= 0xE0 && u <= 0xE7) { boot[0] |= (uint8_t)(1u << (u - 0xE0)); continue; }
        if (k < 8 && u < 0x100) boot[k++] = (uint8_t)u;
    }
    for (int i = 0; i < r->keybits_n && k < 8; i++) {
        if (!get(d, len, (uint16_t)(r->keybits.off + i), 1, false)) continue;
        const int u = r->keybits_umin + i;
        if (u >= 0xE0 && u <= 0xE7) boot[0] |= (uint8_t)(1u << (u - 0xE0));
        else if (u > 3) boot[k++] = (uint8_t)u;
    }
}

int nv_hidr_consumer(const nv_hidr_report_t *r, const uint8_t *d, size_t len, uint16_t *usages, int max) {
    int n = 0;
    for (int i = 0; i < r->ncons && n < max; i++) {
        const int32_t v = get(d, len, (uint16_t)(r->cons.off + i * r->cons.size), r->cons.size, false);
        if (!v && r->cons_lmin == 0 && r->cons_umin == 0) continue;   // 0 = nothing pressed
        const int32_t u = r->cons_umin + (v - r->cons_lmin);
        if (u > 0 && u < 0x10000) usages[n++] = (uint16_t)u;
    }
    for (int i = 0; i < r->ncbits && n < max; i++)
        if (get(d, len, r->cbits[i].off, 1, false)) usages[n++] = r->cbits[i].usage;
    return n;
}
