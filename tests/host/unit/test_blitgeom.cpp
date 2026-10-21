// nv_blit_geom.h: where nv_hal_video_blit puts a (turned) picture on the panel, and nv_blit_unmap,
// which takes a touch back to the picture's pixel. ABI v19 (nv.gfx_set_rotation) shows a portrait
// canvas turned by the PPA; the touch must land on the pixel the user sees under the finger.
//   - unturned: the geometry is the one the firmware used before the turn existed (copied below);
//   - turned 1:1 and 2x: a model of the PPA (scale, then turn the block the way nv_disp's CPU fallback
//     does, which matches the hardware) puts each pixel exactly where nv_blit_unmap says it is;
//   - any other scale: within one output pixel's worth of source (16/k + 1: where a mirrored k/16 step
//     rounds is the hardware's business), and every block stays inside the picture and the panel.
#include "check.h"
#include <cstdlib>
#include <cstring>
#include <vector>
extern "C" {
#include "nv_blit_geom.h"
}

static const int PW = 1024, PH = 600;

// The firmware's geometry before ABI v19 (components/nv_hal/nv_hal.cpp, 1.2.149), for the regression.
static bool old_geom(int sw, int sh, int dx, int dy, int dw, int dh, int mode, nv_hal_blit_geom_t *g) {
    if (!g || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return false;
    if (dx < 0) dx = 0;
    if (dy < 0) dy = 0;
    if (dx + dw > PW) dw = PW - dx;
    if (dy + dh > PH) dh = PH - dy;
    if (dw < 2 || dh < 2) return false;
    int bx = 0, by = 0, bw = sw, bh = sh, kx, ky;
    if (mode == NV_HAL_BLIT_STRETCH) {
        kx = dw * 16 / sw;  ky = dh * 16 / sh;
    } else if (mode == NV_HAL_BLIT_ZOOM) {
        int k = (dw * 16 + sw - 1) / sw;
        { const int k2 = (dh * 16 + sh - 1) / sh; if (k2 > k) k = k2; }
        kx = ky = k;
    } else {
        int k = dw * 16 / sw;
        { const int k2 = dh * 16 / sh; if (k2 < k) k = k2; }
        const int kb = k + 1;
        const int cw = (dw * 16 / kb < sw) ? dw * 16 / kb : sw;
        const int ch = (dh * 16 / kb < sh) ? dh * 16 / kb : sh;
        if (mode != NV_HAL_BLIT_FIT_EXACT && k >= 1 && cw * 100 >= sw * 96 && ch * 100 >= sh * 96) k = kb;
        kx = ky = k;
    }
    if (kx < 1) kx = 1;
    if (ky < 1) ky = 1;
    if (kx > 255 * 16) kx = 255 * 16;
    if (ky > 255 * 16) ky = 255 * 16;
    {
        const int mw = dw * 16 / kx, mh = dh * 16 / ky;
        if (bw > mw) bw = mw;
        if (bh > mh) bh = mh;
    }
    if (bw < 1 || bh < 1) return false;
    bx = (sw - bw) / 2;  by = (sh - bh) / 2;
    const int tw = bw * kx / 16, th = bh * ky / 16;
    if (tw < 1 || th < 1) return false;
    g->kx = kx; g->ky = ky; g->bx = bx; g->by = by; g->bw = bw; g->bh = bh; g->tw = tw; g->th = th;
    g->ox = dx + (dw - tw) / 2; g->oy = dy + (dh - th) / 2;
    g->dx = dx; g->dy = dy; g->dw = dw; g->dh = dh;
    return true;
}

// The PPA as the firmware drives it: the source block (sbx,sby,sbw,sbh) scaled by the source-axis
// factors (scale_x = ky/16 when the turn is odd, see video_draw_into), then turned `rot` quarter turns
// with nv_disp's rotate_strip_cpu mapping (HW-verified against the PPA), written at (ox, oy).
// Fills `panel` (PW x PH) with source pixel indices (y * sw + x), -1 = not written.
static void ppa_model(const nv_hal_blit_geom_t &g, std::vector<int> &panel) {
    panel.assign((size_t)PW * PH, -1);
    const int ksx = (g.rot & 1) ? g.ky : g.kx, ksy = (g.rot & 1) ? g.kx : g.ky;
    const int w = g.sbw * ksx / 16, h = g.sbh * ksy / 16;   // the scaled block, before the turn
    const int cols = (g.rot & 1) ? h : w, rows = (g.rot & 1) ? w : h;
    CHECK(cols == g.tw && rows == g.th);
    for (int dy = 0; dy < rows; dy++)
        for (int dx = 0; dx < cols; dx++) {
            int sx, sy;   // in the scaled block
            switch (g.rot) {
            case 1:  sx = w - 1 - dy; sy = dx;         break;
            case 2:  sx = w - 1 - dx; sy = h - 1 - dy; break;
            case 3:  sx = dy;         sy = h - 1 - dx; break;
            default: sx = dx;         sy = dy;         break;
            }
            const int x = g.sbx + sx * 16 / ksx, y = g.sby + sy * 16 / ksy;   // nearest sample
            panel[(size_t)(g.oy + dy) * PW + g.ox + dx] = y * g.sw + x;
        }
}

static bool inside(const nv_hal_blit_geom_t &g) {
    return g.sbx >= 0 && g.sby >= 0 && g.sbx + g.sbw <= g.sw && g.sby + g.sbh <= g.sh &&
           g.ox >= g.dx && g.oy >= g.dy && g.ox + g.tw <= g.dx + g.dw && g.oy + g.th <= g.dy + g.dh &&
           g.dx + g.dw <= PW && g.dy + g.dh <= PH;
}

// Every written panel pixel unmaps to the pixel the model put there (tol 0) or within tol of it.
static void check_unmap(const nv_hal_blit_geom_t &g, int tol) {
    std::vector<int> panel;
    ppa_model(g, panel);
    int bad = 0;
    for (int py = g.oy; py < g.oy + g.th; py++)
        for (int px = g.ox; px < g.ox + g.tw; px++) {
            const int v = panel[(size_t)py * PW + px];
            int sx, sy;
            nv_blit_unmap(&g, px, py, &sx, &sy);
            if (v < 0 || std::abs(sx - v % g.sw) > tol || std::abs(sy - v / g.sw) > tol) bad++;
        }
    CHECK(bad == 0);
    if (bad) std::fprintf(stderr, "  rot %d %dx%d k %d/%d: %d pixels off\n", g.rot, g.sw, g.sh, g.kx, g.ky, bad);
}

int main() {
    // 1. Unturned: the same geometry as before, field by field.
    static const int sizes[][2] = { {1024, 600}, {512, 300}, {320, 240}, {160, 160}, {1200, 672},
                                    {975, 546}, {16, 16}, {600, 1024}, {1920, 1080}, {333, 77} };
    static const int rects[][4] = { {0, 0, PW, PH}, {0, 40, PW, PH - 40}, {100, 50, 500, 300},
                                    {-5, -5, 2000, 2000}, {0, 0, 1, 1} };
    int same = 0;
    for (const auto &s : sizes)
        for (const auto &r : rects)
            for (int mode = 0; mode < 4; mode++) {
                nv_hal_blit_geom_t a, b;
                std::memset(&a, 0, sizeof a); std::memset(&b, 0, sizeof b);
                const bool oa = old_geom(s[0], s[1], r[0], r[1], r[2], r[3], mode, &a);
                const bool ob = nv_blit_geom(PW, PH, s[0], s[1], r[0], r[1], r[2], r[3], mode, &b);
                CHECK(oa == ob);
                if (!oa || !ob) continue;
                CHECK(a.kx == b.kx && a.ky == b.ky && a.bx == b.bx && a.by == b.by && a.bw == b.bw &&
                      a.bh == b.bh && a.ox == b.ox && a.oy == b.oy && a.tw == b.tw && a.th == b.th &&
                      a.dx == b.dx && a.dy == b.dy && a.dw == b.dw && a.dh == b.dh);
                CHECK(b.rot == 0 && b.sbx == b.bx && b.sby == b.by && b.sbw == b.bw && b.sbh == b.bh);
                same++;
            }
    CHECK(same > 100);

    // 2. Libri's case: a 600x1024 upright canvas turned once fills the panel 1:1, and upright pixel
    // (x, y) is at panel (y, W-1-x), the system's portrait mode (nv_disp rotation 90).
    {
        nv_hal_blit_geom_t g;
        CHECK(nv_blit_geom(PW, PH, 600, 1024, 0, 0, PW, PH, NV_HAL_BLIT_FIT_EXACT | NV_HAL_BLIT_ROT(1), &g));
        CHECK(g.kx == 16 && g.ky == 16 && g.ox == 0 && g.oy == 0 && g.tw == PW && g.th == PH);
        CHECK(g.sbx == 0 && g.sby == 0 && g.sbw == 600 && g.sbh == 1024);
        std::vector<int> panel;
        ppa_model(g, panel);
        const int pts[][2] = { {0, 0}, {599, 0}, {0, 1023}, {599, 1023}, {123, 456} };
        for (const auto &p : pts) CHECK(panel[(size_t)(600 - 1 - p[0]) * PW + p[1]] == p[1] * 600 + p[0]);
        check_unmap(g, 0);
        // the system title bar popped down: the turned picture shrinks to fit below it
        CHECK(nv_blit_geom(PW, PH, 600, 1024, 0, 40, PW, PH - 40, NV_HAL_BLIT_FIT_EXACT | NV_HAL_BLIT_ROT(1), &g));
        CHECK(inside(g) && g.th <= PH - 40 && g.oy >= 40);
        check_unmap(g, 2);
    }

    // 3. Every turn, exact at 1:1 and 2x, within one output pixel at other scales; blocks always inside.
    static const int canv[][2] = { {1024, 600}, {512, 300}, {320, 240}, {256, 160}, {100, 60} };
    for (int rot = 0; rot < 4; rot++)
        for (const auto &c : canv)
            for (const auto &r : rects)
                for (int mode = 0; mode < 4; mode++) {
                    // the canvas as the app draws it: upright (sides swapped for an odd turn)
                    const int sw = (rot & 1) ? c[1] : c[0], sh = (rot & 1) ? c[0] : c[1];
                    nv_hal_blit_geom_t g;
                    if (!nv_blit_geom(PW, PH, sw, sh, r[0], r[1], r[2], r[3], mode | NV_HAL_BLIT_ROT(rot), &g))
                        continue;
                    CHECK(g.rot == rot);
                    CHECK(inside(g));
                    const bool exact = g.kx == g.ky && (g.kx == 16 || g.kx == 32);
                    const int k = g.kx < g.ky ? g.kx : g.ky;
                    check_unmap(g, exact ? 0 : (16 + k - 1) / k + 1);
                }

    // 4. Panel points outside the picture clamp to its edge, never outside the canvas.
    {
        nv_hal_blit_geom_t g;
        CHECK(nv_blit_geom(PW, PH, 300, 512, 0, 0, PW, PH, NV_HAL_BLIT_FIT_EXACT | NV_HAL_BLIT_ROT(1), &g));
        int sx, sy;
        nv_blit_unmap(&g, -50, -50, &sx, &sy);
        CHECK(sx >= 0 && sx < 300 && sy >= 0 && sy < 512);
        nv_blit_unmap(&g, 5000, 5000, &sx, &sy);
        CHECK(sx >= 0 && sx < 300 && sy >= 0 && sy < 512);
    }
    return TEST_DONE("blitgeom");
}
