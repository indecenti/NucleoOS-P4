// chip8.h — CHIP-8 / SUPER-CHIP / XO-CHIP interpreter core for the NucleoOS chip8 app.
//
// Follows Octo's semantics (the reference the CHIP-8 Community Archive tests its programs with):
// one 64 KB address space, 128x64 display with two XO-CHIP bit planes (lores uses the top-left
// 64x32), the Octo quirk flags, SUPER-CHIP scrolling / big font / flag registers, XO-CHIP long
// I, plane select, 5XY2/5XY3 ranges and the audio pattern + pitch registers.
// Freestanding (no libc): builds for wasm32 and natively for the PC test harness.
#pragma once
#include <stdint.h>

enum {                       // Octo quirk flags (programs.json "options")
    C8Q_SHIFT     = 1 << 0,  // 8XY6/8XYE shift vX in place (ignore vY)
    C8Q_LOADSTORE = 1 << 1,  // FX55/FX65 leave I unchanged
    C8Q_VFORDER   = 1 << 2,  // 8XY4/5/6/7/E: the result wins over the flag when X == F
    C8Q_CLIP      = 1 << 3,  // sprites clip at the screen edge instead of wrapping
    C8Q_JUMP      = 1 << 4,  // BXNN jumps to XNN + vX (SUPER-CHIP) instead of NNN + v0
    C8Q_VBLANK    = 1 << 5,  // DXYN waits for the next 60 Hz frame
    C8Q_LOGIC     = 1 << 6,  // 8XY1/2/3 reset vF
};

enum { C8_RUN = 0, C8_WAITKEY = 1, C8_HALT = 2, C8_ERROR = 3 };

typedef struct {
    uint8_t  mem[65536];
    uint8_t  disp[128 * 64];     // bit0 = plane 1, bit1 = plane 2; lores uses x<64, y<32
    uint8_t  v[16];
    uint8_t  flags[16];          // SUPER-CHIP / XO-CHIP "saveflags" registers
    uint8_t  pattern[16];        // XO-CHIP audio pattern (128 one-bit samples)
    uint16_t stack[16];
    uint16_t i, pc;
    uint8_t  sp, dt, st;
    uint8_t  hires, planes, pitch;
    uint8_t  state, waitreg;
    uint8_t  pattern_set;        // program loaded its own audio pattern (F002)
    uint8_t  vblank_hit;         // a DXYN under the vBlank quirk ended this frame
    uint16_t quirks;
    uint16_t keys;               // bit k = hex key k held
    uint16_t keys_prev;          // previous c8_keys() mask
    uint16_t polled;             // keys the program tested with EX9E/EXA1 (learned key hints)
    uint32_t dirty;              // display changed since the host last cleared it
    uint32_t rng;                // CXNN xorshift state (seed it before running)
    uint16_t err_op, err_pc;     // C8_ERROR: offending opcode / address
} chip8_t;

void c8_reset(chip8_t *c, const uint8_t *rom, uint32_t len, uint16_t quirks);
// Run up to n instructions (stops early on a key wait, halt, error or a vBlank-quirk draw).
void c8_run(chip8_t *c, int n);
// Latest hex keypad state (bit k = key k held). A release while FX0A waits delivers that key.
void c8_keys(chip8_t *c, uint16_t mask);
// One 60 Hz tick: timers count down and the vBlank quirk re-arms.
void c8_tick(chip8_t *c);
// Pixel colour index 0..3 at display coordinates of the current resolution.
static inline int c8_px(const chip8_t *c, int x, int y) { return c->disp[y * 128 + x]; }
static inline int c8_w(const chip8_t *c) { return c->hires ? 128 : 64; }
static inline int c8_h(const chip8_t *c) { return c->hires ? 64 : 32; }
