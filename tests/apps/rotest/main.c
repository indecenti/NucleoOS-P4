// rotest — board check for ABI v19 nv_gfx_set_rotation (not a store app: tools/hil/rotation.py
// pushes it, drives it and reads its log).
//
// Draws an upright test card on the turned canvas: a coloured square in each corner (top-left red,
// top-right green, bottom-left blue, bottom-right yellow), "TOP" and an arrow at the top, a 100 px
// grid. Each touch logs its canvas pixel ("touch x y") and leaves a cross there. Buttons R0..R3 turn
// the canvas; BENCH shows 120 frames with one pixel changed, then 120 full repaints, and logs the
// time per frame. The firmware's
// cost of each turned blit is in GET /api/display (blit_us_avg).
#include "nucleo_sdk.h"

static int W, H, rot;
static int mark_x = -1, mark_y = -1;

#define C_BG     NV_RGB(24, 28, 36)
#define C_GRID   NV_RGB(60, 66, 80)
#define C_TEXT   NV_RGB(235, 235, 235)
#define C_BTN    NV_RGB(50, 90, 160)
#define C_BTN_ON NV_RGB(230, 140, 30)
#define C_MARK   NV_RGB(255, 0, 255)

enum { NBTN = 5, BW = 100, BH = 70, GAP = 10 };
static const char *kBtn[NBTN] = { "R0", "R1", "R2", "R3", "BENCH" };

// Every event goes to the log and to events.txt ("<n> <text>" lines, the newest ~3 KB): the log ring
// drops a line when it is busy (a reader holds it), the file is what tools/hil/rotation.py reads.
static char s_ev[3072];
static int s_ev_len, s_ev_n;
static void log_line(const char *fmt, int a, int b, int c, int d) {
    char s[96], line[112];
    nv_snprintf(s, sizeof s, fmt, a, b, c, d);
    nv_log(NV_LOG_INFO, s);
    const int k = nv_snprintf(line, sizeof line, "%d %s\n", ++s_ev_n, s);
    if (s_ev_len + k > (int)sizeof s_ev) {   // keep the newer half
        int cut = s_ev_len / 2;
        while (cut < s_ev_len && s_ev[cut - 1] != '\n') cut++;
        for (int i = cut; i < s_ev_len; i++) s_ev[i - cut] = s_ev[i];
        s_ev_len -= cut;
    }
    for (int i = 0; i < k; i++) s_ev[s_ev_len++] = line[i];
    nv_save("events.txt", s_ev, s_ev_len);
}

static int btn_x(int i) { return (W - (NBTN * BW + (NBTN - 1) * GAP)) / 2 + i * (BW + GAP); }
static int btn_y(void) { return H / 2 - BH / 2; }

static void text_center(int y, const char *s, int col, int scale) {
    nv_gfx_text((W - nv_gfx_text_width(s, scale)) / 2, y, s, col, scale);
}

static void draw(void) {
    nv_gfx_clear(C_BG);
    for (int x = 100; x < W; x += 100) nv_gfx_rect(x, 0, 1, H, C_GRID);
    for (int y = 100; y < H; y += 100) nv_gfx_rect(0, y, W, 1, C_GRID);
    nv_gfx_rect(0, 0, 60, 60, NV_RGB(230, 40, 40));             // top-left red
    nv_gfx_rect(W - 60, 0, 60, 60, NV_RGB(40, 200, 60));        // top-right green
    nv_gfx_rect(0, H - 60, 60, 60, NV_RGB(40, 90, 230));        // bottom-left blue
    nv_gfx_rect(W - 60, H - 60, 60, 60, NV_RGB(240, 220, 40));  // bottom-right yellow
    nv_gfx_tri(W / 2, 70, W / 2 - 40, 130, W / 2 + 40, 130, C_TEXT);   // arrow up
    text_center(140, "TOP", C_TEXT, 4);
    char s[48];
    nv_snprintf(s, sizeof s, "ROT %d  %dX%d", rot, W, H);
    text_center(200, s, C_TEXT, 3);
    for (int i = 0; i < NBTN; i++) {
        const int on = i < 4 && i == rot;
        nv_gfx_rect(btn_x(i), btn_y(), BW, BH, on ? C_BTN_ON : C_BTN);
        nv_gfx_text(btn_x(i) + (BW - nv_gfx_text_width(kBtn[i], 3)) / 2, btn_y() + (BH - 21) / 2, kBtn[i], C_TEXT, 3);
    }
    text_center(H - 100, "BOTTOM", C_TEXT, 3);
    if (mark_x >= 0) {
        nv_gfx_rect(mark_x - 20, mark_y - 1, 41, 3, C_MARK);
        nv_gfx_rect(mark_x - 1, mark_y - 20, 3, 41, C_MARK);
        nv_snprintf(s, sizeof s, "%d %d", mark_x, mark_y);
        text_center(H - 160, s, C_MARK, 3);
    }
}

static void set_rot(int q) {
    const int r = nv_gfx_set_rotation(q);
    if (r >= 0) rot = r;
    W = nv_gfx_width();
    H = nv_gfx_height();
    mark_x = mark_y = -1;
    log_line("rot %d -> %d size %d %d", q, r, W, H);
}

// Back-to-back frames, each blitted whole by the OS. "full": the app repaints the whole canvas (its
// writes share the PSRAM with the blit); "light": one pixel changes, so the frame time is the blit.
static int bench_run(int full) {
    const int n = 120, t0 = nv_millis();
    for (int i = 0; i < n; i++) {
        if (full) {
            nv_gfx_clear(i & 1 ? NV_RGB(80, 30, 30) : NV_RGB(30, 30, 80));
            nv_gfx_rect(0, (i * 8) % H, W, 8, C_TEXT);
        } else {
            nv_gfx_rect(W / 2, H / 2, 1, 1, i & 1 ? C_TEXT : C_BG);
        }
        if (!nv_gfx_present()) return 0;
    }
    const int ms = nv_millis() - t0;
    log_line(full ? "bench full rot %d frames %d total_ms %d us_per_frame %d"
                  : "bench light rot %d frames %d total_ms %d us_per_frame %d", rot, n, ms, ms * 1000 / n);
    return 1;
}
static void bench(void) {
    if (bench_run(0)) bench_run(1);
}

NV_EXPORT("run") void run(void) {
    set_rot(1);   // the case this ABI is for: an upright portrait canvas
    int down_prev = 0, redraw = 1;
    for (;;) {
        if (redraw) { draw(); redraw = 0; }
        if (!nv_gfx_present()) return;
        if (nv_gfx_back()) return;
        int x, y;
        const int down = nv_touch(&x, &y);
        if (down && !down_prev) {
            log_line("touch %d %d rot %d", x, y, rot, 0);
            mark_x = x; mark_y = y;
            redraw = 1;
            for (int i = 0; i < NBTN; i++)
                if (x >= btn_x(i) && x < btn_x(i) + BW && y >= btn_y() && y < btn_y() + BH) {
                    if (i < 4) set_rot(i);
                    else { bench(); mark_x = -1; }
                }
        }
        down_prev = down;
    }
}
