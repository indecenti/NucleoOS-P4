// Cannon — NucleoOS ABI v2 game-surface demo. A one-tank artillery target range that exercises
// the whole game host: nv_gfx_* draw commands, touch input, tone, and the present()-driven loop.
//
// Aim by dragging from the cannon (the drag vector IS the launch velocity — no trig needed, so it
// stays libm-free); release to fire. Land a shell on the red target for a point. Everything is
// drawn with host-native primitives, so the interpreted guest only runs the (tiny) game logic.
#include "nucleo_sdk.h"

static int W, H;          // canvas size
static int gy;            // ground line y
static int bx, by;        // barrel pivot
static float px, py, vx, vy;
static int flying;
static int tx, ty, tw, th;   // target rect
static int score;
static int aim_x, aim_y;     // current aim point
static int prev_down;
static int boom, boomx, boomy;

// colors
static int C_SKY, C_GROUND, C_GRASS, C_TANK, C_BARREL, C_SHELL, C_TGT, C_GUIDE, C_BOOM, C_PIP;

static int rnd(int lo, int hi) {
    int r = nv_rand();
    if (r < 0) r = -r;
    return lo + r % (hi - lo + 1);
}

static void new_target(void) {
    tw = 30; th = 46;
    tx = rnd(W / 2, W - 60);
    ty = gy - th;
}

static void fire(void) {
    px = (float)bx; py = (float)by;
    vx = (aim_x - bx) * 2.4f;
    vy = (aim_y - by) * 2.4f;
    flying = 1;
    nv_gfx_tone(180, 55);
}

static void draw_guide(void) {
    float x = (float)bx, y = (float)by;
    float dx = (aim_x - bx) * 2.4f, dy = (aim_y - by) * 2.4f;
    for (int i = 0; i < 26; i++) {
        dy += 520.0f * 0.03f;
        x += dx * 0.03f; y += dy * 0.03f;
        if (x < 0 || x >= W || y >= gy) break;
        if ((i % 2) == 0) nv_gfx_circle((int)x, (int)y, 1, C_GUIDE);
    }
}

NV_EXPORT("run")
void run(void) {
    W = nv_gfx_width();
    H = nv_gfx_height();
    gy = H - 38;
    bx = 46; by = gy - 20;
    score = 0; flying = 0; prev_down = 0; boom = 0;
    aim_x = bx + 70; aim_y = by - 50;

    C_SKY    = NV_RGB(30, 96, 165);
    C_GROUND = NV_RGB(126, 92, 60);
    C_GRASS  = NV_RGB(86, 182, 84);
    C_TANK   = NV_RGB(60, 120, 240);
    C_BARREL = NV_RGB(22, 26, 32);
    C_SHELL  = NV_RGB(255, 226, 74);
    C_TGT    = NV_RGB(240, 92, 74);
    C_GUIDE  = NV_RGB(255, 255, 255);
    C_BOOM   = NV_RGB(255, 150, 40);
    C_PIP    = NV_RGB(255, 214, 64);

    new_target();
    int last = nv_millis();

    // The loop: draw this frame, present it (paces + tells us when to quit), then step logic.
    while (nv_gfx_present()) {
        int now = nv_millis();
        float dt = (now - last) / 1000.0f;
        last = now;
        if (dt > 0.05f) dt = 0.05f;
        if (dt < 0.0f)  dt = 0.0f;

        // ---- input: drag to aim, release to fire ----
        int ix, iy;
        int down = nv_touch(&ix, &iy);
        if (!flying && !boom) {
            if (down) { aim_x = ix; aim_y = iy; }
            else if (prev_down) fire();     // released
        }
        prev_down = down;

        // ---- physics ----
        if (flying) {
            vy += 520.0f * dt;
            px += vx * dt;
            py += vy * dt;
            if (px < 0 || px >= W) flying = 0;
            else if (py >= gy) { flying = 0; }
            else if (px >= tx && px <= tx + tw && py >= ty && py <= ty + th) {
                flying = 0; boom = 9; boomx = (int)px; boomy = (int)py;
                score++;
                nv_gfx_tone(660, 90);
                new_target();
            }
        }
        if (boom > 0) boom--;

        // ---- draw ----
        nv_gfx_clear(C_SKY);
        nv_gfx_rect(0, gy, W, H - gy, C_GROUND);
        nv_gfx_rect(0, gy, W, 4, C_GRASS);
        // target
        nv_gfx_rect(tx, ty, tw, th, C_TGT);
        nv_gfx_rect(tx, ty, tw, 5, NV_RGB(255, 180, 170));
        // cannon
        nv_gfx_rect(bx - 16, gy - 12, 32, 12, C_TANK);
        nv_gfx_circle(bx, gy - 14, 8, C_TANK);
        nv_gfx_line(bx, by, bx + (aim_x - bx) / 5, by + (aim_y - by) / 5, C_BARREL);
        // aim guide (only while aiming)
        if (!flying && !boom) draw_guide();
        // shell
        if (flying) nv_gfx_circle((int)px, (int)py, 3, C_SHELL);
        // explosion
        if (boom > 0) nv_gfx_circle(boomx, boomy, 4 + (9 - boom) * 3, C_BOOM);
        // score pips
        for (int i = 0; i < score && i < 20; i++) nv_gfx_rect(8 + i * 12, 8, 8, 8, C_PIP);
    }
}
