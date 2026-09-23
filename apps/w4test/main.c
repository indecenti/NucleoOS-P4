// w4test — WASM-4 compatibility self-test cart for NucleoOS. Uses only the official wasm4.h API:
// D-pad moves the smiley, X beeps (pulse), Z toggles the palette (noise burst), touching the
// screen draws a crosshair at the mouse. Shapes, text, 1bpp/2bpp sprites, the launch counter on
// the save disk, and a line with +-2e9 endpoints (host clipping) are drawn every frame.
//
// Build: .\sdk\build_app.ps1 -AppDir apps\w4test -Wasm4
#include "wasm4.h"

static const uint8_t smiley[] = {
    0xc3, 0x81, 0x24, 0x24, 0x00, 0x24, 0x99, 0xc3,
};

// 8x8 2bpp checker (colors 1..4 via DRAW_COLORS 0x4321)
static const uint8_t checker[] = {
    0x1b, 0x1b, 0xe4, 0xe4, 0x1b, 0x1b, 0xe4, 0xe4,
    0x1b, 0x1b, 0xe4, 0xe4, 0x1b, 0x1b, 0xe4, 0xe4,
};

static int px = 76, py = 96;
static unsigned frame;
static uint8_t prev_pad;
static int alt_palette;
static unsigned launches;

static void set_palette(int alt) {
    if (alt) {
        PALETTE[0] = 0xfff6d3; PALETTE[1] = 0xf9a875; PALETTE[2] = 0xeb6b6f; PALETTE[3] = 0x7c3f58;
    } else {
        PALETTE[0] = 0xe0f8cf; PALETTE[1] = 0x86c06c; PALETTE[2] = 0x306850; PALETTE[3] = 0x071821;
    }
}

static void put_num(unsigned v, int x, int y) {
    char buf[12];
    int i = 11;
    buf[i] = 0;
    do { buf[--i] = (char)('0' + v % 10); v /= 10; } while (v && i > 0);
    text(buf + i, x, y);
}

void start(void) {
    if (diskr(&launches, sizeof launches) != sizeof launches) launches = 0;
    launches++;
    diskw(&launches, sizeof launches);
    tracef("w4test start, launch %d", launches);
}

void update(void) {
    frame++;
    const uint8_t pad = *GAMEPAD1;
    const uint8_t pressed = pad & (uint8_t)~prev_pad;
    prev_pad = pad;

    if (pad & BUTTON_LEFT)  px -= 1;
    if (pad & BUTTON_RIGHT) px += 1;
    if (pad & BUTTON_UP)    py -= 1;
    if (pad & BUTTON_DOWN)  py += 1;
    if (px < 0) px = 0; if (px > 152) px = 152;
    if (py < 20) py = 20; if (py > 152) py = 152;

    if (pressed & BUTTON_1) tone(262 | (523u << 16), 12, 60, TONE_PULSE1 | TONE_MODE3);
    if (pressed & BUTTON_2) {
        alt_palette = !alt_palette;
        set_palette(alt_palette);
        tone(900, 8, 50, TONE_NOISE);
    }
    if ((frame % 120) == 0) tone(60 | (72u << 16), 6 << 8 | 6, 40, TONE_TRIANGLE | TONE_NOTE_MODE);

    *DRAW_COLORS = 2;
    text("NucleoOS W4", 36, 2);
    *DRAW_COLORS = 3;
    text("launch", 2, 12);
    put_num(launches, 54, 12);
    put_num(frame / 60, 120, 12);

    *DRAW_COLORS = 0x42;          // fill 2, stroke 4
    rect(4, 26, 40, 24);
    *DRAW_COLORS = 0x34;
    oval(52, 26, 36, 24);
    *DRAW_COLORS = 4;
    line(96, 26, 156, 50);
    hline(4, 56, 152);
    vline(156, 60, 40);
    line(-2000000000, 58, 2000000000, 58);   // must clip on the host, not walk 4e9 steps

    *DRAW_COLORS = 0x4321;
    blit(checker, 4, 64, 8, 8, BLIT_2BPP);
    blit(checker, 16, 64, 8, 8, BLIT_2BPP | BLIT_FLIP_X | BLIT_ROTATE);

    *DRAW_COLORS = 0x40;          // transparent background, color 4 ink
    blit(smiley, px, py, 8, 8, BLIT_1BPP);

    if (*MOUSE_BUTTONS & MOUSE_LEFT) {
        *DRAW_COLORS = 4;
        hline(*MOUSE_X - 4, *MOUSE_Y, 9);
        vline(*MOUSE_X, *MOUSE_Y - 4, 9);
    }

    *DRAW_COLORS = 3;
    char pad_txt[9];
    for (int i = 0; i < 8; i++) pad_txt[i] = (pad & (1u << (7 - i))) ? '1' : '0';
    pad_txt[8] = 0;
    text("pad", 2, 150);
    text(pad_txt, 30, 150);
}
