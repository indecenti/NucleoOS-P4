// Timer — a stopwatch + countdown for NucleoOS. ABI v4 (uses nv_gfx_text_width for centered layout).
//
// Two modes (toggle when stopped): STOPWATCH counts up, COUNTDOWN counts a set minutes down and beeps
// at zero. Immediate-mode: redraw only when the shown second changes or on a tap (double-buffered, so
// each change paints two frames then idles — zero host calls while paused).
#include "nucleo_sdk.h"

#define BG      NV_RGB(18, 20, 28)
#define PANEL   NV_RGB(34, 38, 52)
#define ACCENT  NV_RGB(84, 162, 255)
#define GREEN   NV_RGB(56, 200, 120)
#define RED     NV_RGB(232, 84, 92)
#define INK     NV_RGB(240, 242, 248)
#define DIMINK  NV_RGB(150, 156, 172)
#define WHITE   NV_RGB(255, 255, 255)

typedef struct { int x, y, w, h; } Rect;

static int  W, H;
static int  mode = 0;                 // 0 = stopwatch, 1 = countdown
static int  running = 0;
static int  base_ms = 0;              // elapsed accumulated while paused
static int  start_ms = 0;             // nv_millis() at the last start
static int  set_ms = 5 * 60 * 1000;   // countdown target (default 5:00)
static int  beeped = 0;
static int  last_shown = -1;          // last displayed whole second (dirty tracking)
static int  redraw = 2;
static int  prev_down = 0;

static Rect b_mode, b_minus, b_plus, b_start, b_reset;

static int cur_ms(void) {
    int e = base_ms;
    if (running) e += nv_millis() - start_ms;
    return e;
}
static int disp_ms(void) {
    if (mode == 0) return cur_ms();
    int r = set_ms - cur_ms();
    return r < 0 ? 0 : r;
}
static int hit(Rect r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static void layout(void) {
    int cx = W / 2;
    b_start = (Rect){ cx - 150, 470, 300, 92 };
    b_reset = (Rect){ cx + 172, 470, 168, 92 };
    b_mode  = (Rect){ cx - 320, 470, 148, 92 };
    b_minus = (Rect){ cx - 172, 384, 150, 64 };
    b_plus  = (Rect){ cx + 22,  384, 150, 64 };
}

static void btn(Rect r, const char *label, int fill, int ink, int scale) {
    nv_gfx_rect(r.x, r.y, r.w, r.h, fill);
    int tw = nv_gfx_text_width(label, scale);
    nv_gfx_text(r.x + (r.w - tw) / 2, r.y + (r.h - 7 * scale) / 2, label, ink, scale);
}

// "MM:SS" into out[6] (minutes clamp at 99).
static void fmt_time(char *out, int ms) {
    int tot = ms / 1000, m = tot / 60, s = tot % 60;
    if (m > 99) m = 99;
    out[0] = '0' + (m / 10) % 10; out[1] = '0' + m % 10;
    out[2] = ':';
    out[3] = '0' + s / 10;        out[4] = '0' + s % 10;
    out[5] = 0;
}

static void draw(void) {
    nv_gfx_clear(BG);
    nv_gfx_text_center(66, mode ? "COUNTDOWN" : "STOPWATCH", ACCENT, 3);

    char t[8];
    fmt_time(t, disp_ms());
    nv_gfx_text_center(180, t, INK, 15);

    if (mode == 1 && !running) {   // countdown minute adjust
        btn(b_minus, "-1M", PANEL, INK, 3);
        btn(b_plus,  "+1M", PANEL, INK, 3);
    }
    btn(b_mode,  "MODE", PANEL, DIMINK, 3);
    btn(b_start, running ? "STOP" : "START", running ? RED : GREEN, WHITE, 4);
    btn(b_reset, "RESET", PANEL, INK, 3);
}

static void tap(int x, int y) {
    if (hit(b_start, x, y)) {
        if (running) { base_ms = cur_ms(); running = 0; }
        else if (!(mode == 1 && disp_ms() == 0)) { start_ms = nv_millis(); running = 1; beeped = 0; }
    } else if (hit(b_reset, x, y)) {
        base_ms = 0; running = 0; beeped = 0;
    } else if (!running && hit(b_mode, x, y)) {
        mode ^= 1; base_ms = 0; beeped = 0;
    } else if (!running && mode == 1 && hit(b_minus, x, y)) {
        set_ms -= 60000; if (set_ms < 60000) set_ms = 60000;
    } else if (!running && mode == 1 && hit(b_plus, x, y)) {
        set_ms += 60000; if (set_ms > 99 * 60000) set_ms = 99 * 60000;
    } else {
        return;
    }
    last_shown = -1;   // force the time label to refresh
    redraw = 2;
}

NV_EXPORT("run")
void run(void) {
    W = nv_gfx_width();
    H = nv_gfx_height();
    layout();
    while (nv_gfx_present()) {
        if (nv_gfx_back()) break;

        int x, y, down = nv_touch(&x, &y);
        int t = (!down && prev_down);
        prev_down = down;
        if (t) tap(x, y);

        if (mode == 1 && running && disp_ms() == 0) {   // countdown reached zero
            running = 0; base_ms = set_ms;               // park exactly at 00:00
            if (!beeped) { nv_gfx_tone(1046, 180); beeped = 1; }
            redraw = 2;
        }

        int ds = disp_ms() / 1000;
        if (ds != last_shown) { last_shown = ds; redraw = 2; }   // paint both buffers on a tick

        if (redraw > 0) { draw(); redraw--; }
    }
}
