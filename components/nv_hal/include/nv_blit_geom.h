// nv_blit_geom.h — the geometry of a PPA scaled (and turned) blit, as plain arithmetic.
//
// nv_hal_video_blit / nv_hal_video_draw place a picture inside a panel rectangle with it; a caller
// that shows touch on the picture (the game view of a scaled WASM canvas) maps panel points back to
// picture pixels with nv_blit_unmap. Header-only, no ESP-IDF: tests/host checks it on the PC.
//
// Turning (NV_HAL_BLIT_ROT(q), q quarter turns): the picture is shown turned the way the system's own
// rotated UI is (nv_disp, LV_DISPLAY_ROTATION_n == PPA_SRM_ROTATION_ANGLE_n). For a sw x sh picture,
// pixel (x, y) shows at (rx, ry) of the turned picture:
//   q=1: (y, sw-1-x)    q=2: (sw-1-x, sh-1-y)    q=3: (sh-1-y, x)
// so with q=1 a 600x1024 picture fills the 1024x600 panel and reads upright on a device held with its
// left edge up (the system's portrait mode). The PPA turns while it scales: no pass over the pixels.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// `mode` of nv_hal_video_blit / _draw / _geom:
// NV_HAL_BLIT_FIT (letterbox, aspect kept), _STRETCH (fill, aspect ignored), _ZOOM (fill, aspect kept,
// overflow cropped), _FIT_EXACT (like _FIT but never crops: game canvases, HUD and touch at the edges);
// OR NV_HAL_BLIT_ROT(q) into it to turn the picture q quarter turns.
enum { NV_HAL_BLIT_FIT = 0, NV_HAL_BLIT_STRETCH = 1, NV_HAL_BLIT_ZOOM = 2, NV_HAL_BLIT_FIT_EXACT = 3 };
#define NV_HAL_BLIT_ROT(q)      (((q) & 3) << 8)
#define NV_HAL_BLIT_ROT_OF(m)   (((m) >> 8) & 3)
#define NV_HAL_BLIT_SCALE_OF(m) ((m) & 0xFF)

// The turned picture's block (bx,by,bw,bh) is scaled by kx/16 x ky/16 into (ox,oy,tw,th) on the panel,
// inside the clamped rect (dx,dy,dw,dh). Without turning it is a block of the picture itself. The PPA
// reads the source block (sbx,sby,sbw,sbh) of the sw x sh picture and turns it `rot` quarter turns.
typedef struct {
    int kx, ky, bx, by, bw, bh, ox, oy, tw, th, dx, dy, dw, dh;
    int rot, sw, sh, sbx, sby, sbw, sbh;
} nv_hal_blit_geom_t;

// The geometry for a pw x ph panel. False when nothing would be drawn.
static inline bool nv_blit_geom(int pw, int ph, int sw, int sh, int dx, int dy, int dw, int dh, int mode,
                                nv_hal_blit_geom_t *g) {
    if (!g || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return false;
    const int rot = NV_HAL_BLIT_ROT_OF(mode), scale = NV_HAL_BLIT_SCALE_OF(mode);
    // clamp the destination rect to the panel
    if (dx < 0) dx = 0;
    if (dy < 0) dy = 0;
    if (dx + dw > pw) dw = pw - dx;
    if (dy + dh > ph) dh = ph - dy;
    if (dw < 2 || dh < 2) return false;

    // The picture as the panel shows it: a quarter turn swaps its sides.
    const int tsw = (rot & 1) ? sh : sw, tsh = (rot & 1) ? sw : sh;
    // The SRM hardware scales by k/16 (rounded down) and writes floor(block * k / 16) pixels, so pick
    // k per mode and crop the source block to match — the output then covers exactly what we clear
    // around. Block = (bx, by, bw, bh) inside the turned tsw x tsh picture.
    int bx = 0, by = 0, bw = tsw, bh = tsh, kx, ky;
    if (scale == NV_HAL_BLIT_STRETCH) {
        kx = dw * 16 / tsw;  ky = dh * 16 / tsh;
    } else if (scale == NV_HAL_BLIT_ZOOM) {
        // cover the rect: the smallest k that fills both axes, overflow cropped (centred)
        int k = (dw * 16 + tsw - 1) / tsw;
        { const int k2 = (dh * 16 + tsh - 1) / tsh; if (k2 > k) k = k2; }
        kx = ky = k;
    } else {
        // fit inside the rect; one 1/16 step larger costs a thin edge crop — worth it while the crop
        // stays under ~4% per axis (1200x672 camera clips: 975x546 -> 1022x588 on the 1024x600 panel).
        // FIT_EXACT (game canvases: HUD at the edges, touch mapped back to canvas pixels) never crops.
        int k = dw * 16 / tsw;
        { const int k2 = dh * 16 / tsh; if (k2 < k) k = k2; }
        const int kb = k + 1;
        const int cw = (dw * 16 / kb < tsw) ? dw * 16 / kb : tsw;
        const int ch = (dh * 16 / kb < tsh) ? dh * 16 / kb : tsh;
        if (scale != NV_HAL_BLIT_FIT_EXACT && k >= 1 && cw * 100 >= tsw * 96 && ch * 100 >= tsh * 96) k = kb;
        kx = ky = k;
    }
    if (kx < 1) kx = 1;
    if (ky < 1) ky = 1;
    if (kx > 255 * 16) kx = 255 * 16;   // SRM integer part < 256
    if (ky > 255 * 16) ky = 255 * 16;
    {   // largest block that fits
        const int mw = dw * 16 / kx, mh = dh * 16 / ky;
        if (bw > mw) bw = mw;
        if (bh > mh) bh = mh;
    }
    if (bw < 1 || bh < 1) return false;
    bx = (tsw - bw) / 2;  by = (tsh - bh) / 2;
    const int tw = bw * kx / 16, th = bh * ky / 16;
    if (tw < 1 || th < 1) return false;
    g->kx = kx; g->ky = ky; g->bx = bx; g->by = by; g->bw = bw; g->bh = bh; g->tw = tw; g->th = th;
    g->ox = dx + (dw - tw) / 2; g->oy = dy + (dh - th) / 2;
    g->dx = dx; g->dy = dy; g->dw = dw; g->dh = dh;
    g->rot = rot; g->sw = sw; g->sh = sh;
    switch (rot) {   // the turned block back in the picture (inverse of the mapping above)
    case 1:  g->sbx = sw - by - bh; g->sby = bx;           g->sbw = bh; g->sbh = bw; break;
    case 2:  g->sbx = sw - bx - bw; g->sby = sh - by - bh; g->sbw = bw; g->sbh = bh; break;
    case 3:  g->sbx = by;           g->sby = sh - bx - bw; g->sbw = bh; g->sbh = bw; break;
    default: g->sbx = bx;           g->sby = by;           g->sbw = bw; g->sbh = bh; break;
    }
    return true;
}

// Panel point -> picture pixel (inverse of the k/16 scale and of the turn), clamped to the picture.
static inline void nv_blit_unmap(const nv_hal_blit_geom_t *g, int px, int py, int *sx, int *sy) {
    const int rx = g->kx > 0 ? g->bx + (px - g->ox) * 16 / g->kx : 0;
    const int ry = g->ky > 0 ? g->by + (py - g->oy) * 16 / g->ky : 0;
    int x, y;
    switch (g->rot) {
    case 1:  x = g->sw - 1 - ry; y = rx;              break;
    case 2:  x = g->sw - 1 - rx; y = g->sh - 1 - ry;  break;
    case 3:  x = ry;             y = g->sh - 1 - rx;  break;
    default: x = rx;             y = ry;              break;
    }
    *sx = x < 0 ? 0 : (x >= g->sw ? g->sw - 1 : x);
    *sy = y < 0 ? 0 : (y >= g->sh ? g->sh - 1 : y);
}

#ifdef __cplusplus
}
#endif
