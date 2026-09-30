// chip8.c — CHIP-8 / SUPER-CHIP / XO-CHIP interpreter core (see chip8.h). Octo semantics.
#include "chip8.h"

// Octo's small (5-byte) hex font at 0x000 and big (10-byte) font at 0x050.
static const uint8_t k_font[16 * 5] = {
    0xF0, 0x90, 0x90, 0x90, 0xF0, 0x20, 0x60, 0x20, 0x20, 0x70, 0xF0, 0x10, 0xF0, 0x80, 0xF0,
    0xF0, 0x10, 0xF0, 0x10, 0xF0, 0x90, 0x90, 0xF0, 0x10, 0x10, 0xF0, 0x80, 0xF0, 0x10, 0xF0,
    0xF0, 0x80, 0xF0, 0x90, 0xF0, 0xF0, 0x10, 0x20, 0x40, 0x40, 0xF0, 0x90, 0xF0, 0x90, 0xF0,
    0xF0, 0x90, 0xF0, 0x10, 0xF0, 0xF0, 0x90, 0xF0, 0x90, 0x90, 0xE0, 0x90, 0xE0, 0x90, 0xE0,
    0xF0, 0x80, 0x80, 0x80, 0xF0, 0xE0, 0x90, 0x90, 0x90, 0xE0, 0xF0, 0x80, 0xF0, 0x80, 0xF0,
    0xF0, 0x80, 0xF0, 0x80, 0x80,
};
static const uint8_t k_bigfont[16 * 10] = {
    0xFF, 0xFF, 0xC3, 0xC3, 0xC3, 0xC3, 0xC3, 0xC3, 0xFF, 0xFF,   // 0
    0x18, 0x78, 0x78, 0x18, 0x18, 0x18, 0x18, 0x18, 0xFF, 0xFF,   // 1
    0xFF, 0xFF, 0x03, 0x03, 0xFF, 0xFF, 0xC0, 0xC0, 0xFF, 0xFF,   // 2
    0xFF, 0xFF, 0x03, 0x03, 0xFF, 0xFF, 0x03, 0x03, 0xFF, 0xFF,   // 3
    0xC3, 0xC3, 0xC3, 0xC3, 0xFF, 0xFF, 0x03, 0x03, 0x03, 0x03,   // 4
    0xFF, 0xFF, 0xC0, 0xC0, 0xFF, 0xFF, 0x03, 0x03, 0xFF, 0xFF,   // 5
    0xFF, 0xFF, 0xC0, 0xC0, 0xFF, 0xFF, 0xC3, 0xC3, 0xFF, 0xFF,   // 6
    0xFF, 0xFF, 0x03, 0x03, 0x06, 0x0C, 0x18, 0x18, 0x18, 0x18,   // 7
    0xFF, 0xFF, 0xC3, 0xC3, 0xFF, 0xFF, 0xC3, 0xC3, 0xFF, 0xFF,   // 8
    0xFF, 0xFF, 0xC3, 0xC3, 0xFF, 0xFF, 0x03, 0x03, 0xFF, 0xFF,   // 9
    0x7E, 0xFF, 0xC3, 0xC3, 0xC3, 0xFF, 0xFF, 0xC3, 0xC3, 0xC3,   // A
    0xFC, 0xFC, 0xC3, 0xC3, 0xFC, 0xFC, 0xC3, 0xC3, 0xFC, 0xFC,   // B
    0x3C, 0xFF, 0xC3, 0xC0, 0xC0, 0xC0, 0xC0, 0xC3, 0xFF, 0x3C,   // C
    0xFC, 0xFE, 0xC3, 0xC3, 0xC3, 0xC3, 0xC3, 0xC3, 0xFE, 0xFC,   // D
    0xFF, 0xFF, 0xC0, 0xC0, 0xFF, 0xFF, 0xC0, 0xC0, 0xFF, 0xFF,   // E
    0xFF, 0xFF, 0xC0, 0xC0, 0xFF, 0xFF, 0xC0, 0xC0, 0xC0, 0xC0,   // F
};

static void zero(void *p, uint32_t n) { uint8_t *b = (uint8_t *)p; while (n--) *b++ = 0; }

void c8_reset(chip8_t *c, const uint8_t *rom, uint32_t len, uint16_t quirks) {
    uint32_t seed = c->rng;
    zero(c, sizeof *c);
    c->rng = seed ? seed : 0x2545F491u;
    for (int k = 0; k < 16 * 5; k++) c->mem[k] = k_font[k];
    for (int k = 0; k < 16 * 10; k++) c->mem[0x50 + k] = k_bigfont[k];
    if (len > 65536 - 0x200) len = 65536 - 0x200;
    for (uint32_t k = 0; k < len; k++) c->mem[0x200 + k] = rom[k];
    c->pc = 0x200;
    c->planes = 1;
    c->pitch = 64;
    c->quirks = quirks;
    c->dirty = 1;
}

void c8_keys(chip8_t *c, uint16_t mask) {
    uint16_t released = c->keys & (uint16_t)~mask;
    c->keys_prev = c->keys;
    c->keys = mask;
    if (c->state == C8_WAITKEY && released) {
        int k = 0;
        while (!(released & (1u << k))) k++;
        c->v[c->waitreg] = (uint8_t)k;
        c->state = C8_RUN;
    }
}

void c8_tick(chip8_t *c) {
    if (c->dt) c->dt--;
    if (c->st) c->st--;
    c->vblank_hit = 0;
}

static inline uint8_t rnd(chip8_t *c) {
    uint32_t x = c->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    c->rng = x;
    return (uint8_t)(x >> 8);
}

static inline void skip(chip8_t *c) {
    // A skipped XO-CHIP "i := long NNNN" is 4 bytes long.
    if (c->mem[c->pc] == 0xF0 && c->mem[(uint16_t)(c->pc + 1)] == 0x00) c->pc += 4;
    else c->pc += 2;
}

static inline void carry(chip8_t *c, int x, unsigned val, int flag) {
    c->v[x] = (uint8_t)val;
    c->v[15] = (uint8_t)(flag ? 1 : 0);
    if (c->quirks & C8Q_VFORDER) c->v[x] = (uint8_t)val;
}

static void clear(chip8_t *c) {
    for (int k = 0; k < 128 * 64; k++) c->disp[k] &= (uint8_t)~c->planes;
    c->dirty = 1;
}

// Scroll the selected planes by (dx, dy) pixels of the current resolution; vacated area is blank.
static void scroll(chip8_t *c, int dx, int dy) {
    const int w = c8_w(c), h = c8_h(c);
    const uint8_t pm = c->planes, keep = (uint8_t)~pm;
    const int y0 = dy > 0 ? h - 1 : 0, y1 = dy > 0 ? -1 : h, ys = dy > 0 ? -1 : 1;
    const int x0 = dx > 0 ? w - 1 : 0, x1 = dx > 0 ? -1 : w, xs = dx > 0 ? -1 : 1;
    for (int y = y0; y != y1; y += ys) {
        for (int x = x0; x != x1; x += xs) {
            int sx = x - dx, sy = y - dy;
            uint8_t src = (sx >= 0 && sx < w && sy >= 0 && sy < h) ? (c->disp[sy * 128 + sx] & pm) : 0;
            c->disp[y * 128 + x] = (uint8_t)((c->disp[y * 128 + x] & keep) | src);
        }
    }
    c->dirty = 1;
}

static void draw(chip8_t *c, int x, int y, int n) {
    const int w = c8_w(c), h = c8_h(c);
    const int clip = (c->quirks & C8Q_CLIP) != 0;
    const int bx = c->v[x] % w, by = c->v[y] % h;
    const int rows = n ? n : 16, cols = n ? 8 : 16, step = n ? n : 32;
    uint16_t addr = c->i;
    int hit = 0;
    for (int layer = 0; layer < 2; layer++) {
        const uint8_t bit = (uint8_t)(1 << layer);
        if (!(c->planes & bit)) continue;
        for (int r = 0; r < rows; r++) {
            int py = by + r;
            if (py >= h) { if (clip) break; py -= h; }
            unsigned line = n ? c->mem[(uint16_t)(addr + r)]
                              : (unsigned)(c->mem[(uint16_t)(addr + 2 * r)] << 8 | c->mem[(uint16_t)(addr + 2 * r + 1)]);
            if (!line) continue;
            for (int b = 0; b < cols; b++) {
                if (!((line >> (cols - 1 - b)) & 1)) continue;
                int px = bx + b;
                if (px >= w) { if (clip) break; px -= w; }
                uint8_t *d = &c->disp[py * 128 + px];
                if (*d & bit) hit = 1;
                *d ^= bit;
            }
        }
        addr = (uint16_t)(addr + step);
    }
    c->v[15] = (uint8_t)hit;
    c->dirty = 1;
    if (c->quirks & C8Q_VBLANK) c->vblank_hit = 1;
}

static void fail(chip8_t *c, uint16_t op) {
    c->state = C8_ERROR;
    c->err_op = op;
    c->err_pc = (uint16_t)(c->pc - 2);
}

void c8_run(chip8_t *c, int n) {
    uint8_t *m = c->mem, *v = c->v;
    while (n-- > 0) {
        if (c->state != C8_RUN || c->vblank_hit) return;
        const uint16_t op = (uint16_t)(m[c->pc] << 8 | m[(uint16_t)(c->pc + 1)]);
        c->pc += 2;
        const int x = (op >> 8) & 0xF, y = (op >> 4) & 0xF, nn = op & 0xFF, nnn = op & 0xFFF;
        switch (op >> 12) {
        case 0x0:
            if (op == 0x00E0) clear(c);
            else if (op == 0x00EE) {
                if (!c->sp) { fail(c, op); return; }
                c->pc = c->stack[--c->sp];
            } else if ((op & 0xFFF0) == 0x00C0) scroll(c, 0, op & 0xF);
            else if ((op & 0xFFF0) == 0x00D0) scroll(c, 0, -(op & 0xF));
            else if (op == 0x00FB) scroll(c, 4, 0);
            else if (op == 0x00FC) scroll(c, -4, 0);
            else if (op == 0x00FD) { c->state = C8_HALT; return; }
            else if (op == 0x00FE || op == 0x00FF) {
                c->hires = (uint8_t)(op == 0x00FF);
                for (int k = 0; k < 128 * 64; k++) c->disp[k] = 0;
                c->dirty = 1;
            } else { fail(c, op); return; }
            break;
        case 0x1: c->pc = (uint16_t)nnn; break;
        case 0x2:
            if (c->sp >= 16) { fail(c, op); return; }
            c->stack[c->sp++] = c->pc;
            c->pc = (uint16_t)nnn;
            break;
        case 0x3: if (v[x] == nn) skip(c); break;
        case 0x4: if (v[x] != nn) skip(c); break;
        case 0x5: {
            const int sub = op & 0xF;
            if (sub == 0) { if (v[x] == v[y]) skip(c); }
            else if (sub == 2 || sub == 3) {       // XO-CHIP save/load vX..vY (either direction)
                const int d = x > y ? x - y : y - x;
                for (int z = 0; z <= d; z++) {
                    const int r = x > y ? x - z : x + z;
                    uint8_t *p = &m[(uint16_t)(c->i + z)];
                    if (sub == 2) *p = v[r]; else v[r] = *p;
                }
            } else { fail(c, op); return; }
            break;
        }
        case 0x6: v[x] = (uint8_t)nn; break;
        case 0x7: v[x] = (uint8_t)(v[x] + nn); break;
        case 0x8:
            switch (op & 0xF) {
            case 0x0: v[x] = v[y]; break;
            case 0x1: v[x] |= v[y]; if (c->quirks & C8Q_LOGIC) v[15] = 0; break;
            case 0x2: v[x] &= v[y]; if (c->quirks & C8Q_LOGIC) v[15] = 0; break;
            case 0x3: v[x] ^= v[y]; if (c->quirks & C8Q_LOGIC) v[15] = 0; break;
            case 0x4: { unsigned t = (unsigned)v[x] + v[y]; carry(c, x, t, t > 0xFF); break; }
            case 0x5: { int f = v[x] >= v[y]; carry(c, x, (unsigned)(v[x] - v[y]), f); break; }
            case 0x7: { int f = v[y] >= v[x]; carry(c, x, (unsigned)(v[y] - v[x]), f); break; }
            case 0x6: { uint8_t s = (c->quirks & C8Q_SHIFT) ? v[x] : v[y]; carry(c, x, s >> 1, s & 1); break; }
            case 0xE: { uint8_t s = (c->quirks & C8Q_SHIFT) ? v[x] : v[y]; carry(c, x, (unsigned)(s << 1), s >> 7); break; }
            default: fail(c, op); return;
            }
            break;
        case 0x9:
            if ((op & 0xF) != 0) { fail(c, op); return; }
            if (v[x] != v[y]) skip(c);
            break;
        case 0xA: c->i = (uint16_t)nnn; break;
        case 0xB: c->pc = (uint16_t)(nnn + ((c->quirks & C8Q_JUMP) ? v[x] : v[0])); break;
        case 0xC: v[x] = (uint8_t)(rnd(c) & nn); break;
        case 0xD: draw(c, x, y, op & 0xF); break;
        case 0xE:
            if (nn == 0x9E) { c->polled |= (uint16_t)(1u << (v[x] & 0xF)); if (c->keys & (1u << (v[x] & 0xF))) skip(c); }
            else if (nn == 0xA1) { c->polled |= (uint16_t)(1u << (v[x] & 0xF)); if (!(c->keys & (1u << (v[x] & 0xF)))) skip(c); }
            else { fail(c, op); return; }
            break;
        case 0xF:
            switch (nn) {
            case 0x00:
                if (x) { fail(c, op); return; }                            // i := long NNNN
                c->i = (uint16_t)(m[c->pc] << 8 | m[(uint16_t)(c->pc + 1)]);
                c->pc += 2;
                break;
            case 0x01: c->planes = (uint8_t)(x & 3); break;
            case 0x02:
                if (x) { fail(c, op); return; }
                for (int k = 0; k < 16; k++) c->pattern[k] = m[(uint16_t)(c->i + k)];
                c->pattern_set = 1;
                break;
            case 0x07: v[x] = c->dt; break;
            case 0x0A: c->state = C8_WAITKEY; c->waitreg = (uint8_t)x; return;
            case 0x15: c->dt = v[x]; break;
            case 0x18: c->st = v[x]; break;
            case 0x1E: c->i = (uint16_t)(c->i + v[x]); break;
            case 0x29: c->i = (uint16_t)((v[x] & 0xF) * 5); break;
            case 0x30: c->i = (uint16_t)(0x50 + (v[x] & 0xF) * 10); break;
            case 0x33:
                m[c->i] = (uint8_t)(v[x] / 100);
                m[(uint16_t)(c->i + 1)] = (uint8_t)(v[x] / 10 % 10);
                m[(uint16_t)(c->i + 2)] = (uint8_t)(v[x] % 10);
                break;
            case 0x3A: c->pitch = v[x]; break;
            case 0x55:
                for (int k = 0; k <= x; k++) m[(uint16_t)(c->i + k)] = v[k];
                if (!(c->quirks & C8Q_LOADSTORE)) c->i = (uint16_t)(c->i + x + 1);
                break;
            case 0x65:
                for (int k = 0; k <= x; k++) v[k] = m[(uint16_t)(c->i + k)];
                if (!(c->quirks & C8Q_LOADSTORE)) c->i = (uint16_t)(c->i + x + 1);
                break;
            case 0x75: for (int k = 0; k <= x; k++) c->flags[k] = v[k]; break;
            case 0x85: for (int k = 0; k <= x; k++) v[k] = c->flags[k]; break;
            default: fail(c, op); return;
            }
            break;
        }
    }
}
