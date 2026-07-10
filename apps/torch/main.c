// Torch — a flashlight for NucleoOS. ABI v4 (uses nv_backlight to crank the panel; the OS restores
// the user's brightness on exit). Tap the screen to cycle colour/brightness presets; an SOS toggle
// blinks the international morse distress signal (with a beep on each flash).
#include "nucleo_sdk.h"

// Presets as literal RGB565 (NV_RGB is a function, not usable in a static initializer).
static const unsigned short P_COL[]  = { 0xFFFF, 0xFEB2, 0xF800, 0x07E0, 0x001F, 0xC81F, 0xFFFF };
static const int            P_BRT[]  = { 100,    100,    92,     92,     92,     92,     28     };
static const char *const    P_NAME[] = { "WHITE","WARM", "RED",  "GREEN","BLUE", "PURPLE","DIM"  };
#define NPRE 7

// SOS in morse: +units of light on, -units of gap. dot=1, dash=3, intra-letter gap 1, letter gap 3,
// word gap 7. One UNIT = 200 ms.
#define UNIT 200
static const signed char SOS[] = { 1,-1,1,-1,1, -3, 3,-1,3,-1,3, -3, 1,-1,1,-1,1, -7 };
#define NSEG ((int)(sizeof(SOS)/sizeof(SOS[0])))

typedef struct { int x, y, w, h; } Rect;

static int  W, H;
static int  idx = 0;
static int  sos = 0;
static int  sos_t0 = 0;
static int  prev_on = 0;
static int  redraw = 2;
static int  prev_down = 0;
static Rect b_color, b_sos;

static int hit(Rect r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

// morse light state for elapsed ms; also reports the current segment's remaining ms via *seg_ms.
static int sos_state(int elapsed, int *seg_ms) {
    int total = 0;
    for (int i = 0; i < NSEG; i++) total += (SOS[i] < 0 ? -SOS[i] : SOS[i]);
    total *= UNIT;
    int t = elapsed % total, acc = 0;
    for (int i = 0; i < NSEG; i++) {
        int u = SOS[i] < 0 ? -SOS[i] : SOS[i];
        int dur = u * UNIT;
        if (t < acc + dur) { if (seg_ms) *seg_ms = dur; return SOS[i] > 0; }
        acc += dur;
    }
    return 0;
}

static void btn(Rect r, const char *label, int fill, int ink, int scale) {
    nv_gfx_rect(r.x, r.y, r.w, r.h, fill);
    int tw = nv_gfx_text_width(label, scale);
    nv_gfx_text(r.x + (r.w - tw) / 2, r.y + (r.h - 7 * scale) / 2, label, ink, scale);
}

static void draw(void) {
    int on = sos ? sos_state(nv_millis() - sos_t0, 0) : 1;
    nv_gfx_clear(on ? P_COL[idx] : 0x0000);

    // dark control bar so the buttons read on any colour
    int barh = 92, by = H - barh;
    nv_gfx_rect(0, by, W, barh, NV_RGB(12, 12, 16));
    btn(b_color, P_NAME[idx], NV_RGB(40, 44, 58), NV_RGB(255, 255, 255), 3);
    btn(b_sos, sos ? "SOS ON" : "SOS", sos ? NV_RGB(232, 84, 92) : NV_RGB(40, 44, 58),
        NV_RGB(255, 255, 255), 3);

    char pct[8];   // "NN%" brightness readout
    int b = P_BRT[idx];
    if (b >= 100) { pct[0] = '1'; pct[1] = '0'; pct[2] = '0'; pct[3] = '%'; pct[4] = 0; }
    else          { pct[0] = '0' + (b / 10) % 10; pct[1] = '0' + b % 10; pct[2] = '%'; pct[3] = 0; }
    int tw = nv_gfx_text_width(pct, 3);
    nv_gfx_text((W - tw) / 2, by + (barh - 21) / 2, pct, NV_RGB(200, 204, 214), 3);
}

static void next_color(void) {
    idx = (idx + 1) % NPRE;
    nv_backlight(P_BRT[idx]);
    redraw = 2;
}

NV_EXPORT("run")
void run(void) {
    W = nv_gfx_width();
    H = nv_gfx_height();
    b_color = (Rect){ 20, H - 78, 260, 62 };
    b_sos   = (Rect){ W - 220, H - 78, 200, 62 };
    nv_backlight(P_BRT[idx]);   // crank to full white on entry (restored by the OS on exit)

    while (nv_gfx_present()) {
        if (nv_gfx_back()) break;

        int x, y, down = nv_touch(&x, &y);
        int tap = (!down && prev_down);
        prev_down = down;
        if (tap) {
            if (hit(b_sos, x, y)) { sos ^= 1; sos_t0 = nv_millis(); prev_on = 0; redraw = 2; }
            else                  { next_color(); }   // tap anywhere else cycles colour
        }

        if (sos) {
            int seg_ms = 0, on = sos_state(nv_millis() - sos_t0, &seg_ms);
            if (on && !prev_on) nv_gfx_tone(700, seg_ms > 400 ? 400 : seg_ms);   // beep at each flash
            prev_on = on;
            redraw = 2;   // animate: keep both buffers fresh while blinking
        }

        if (redraw > 0) { draw(); redraw--; }
    }
}
