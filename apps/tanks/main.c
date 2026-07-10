// Nucleo Tanks — turn-based artillery (Worms / Pocket Tanks lineage), ported to the NucleoOS WASM
// runtime from the Cardputer "NUCLEO TANKS" (G:\Nucleo firmware/app_tanks.cpp). The Cardputer build
// was native C++/M5GFX driven by a physical keyboard; this is a fresh WASM implementation of the same
// game for our touch device: a destructible height-field battlefield, biomes, wind, angle/power aim
// with a live trajectory preview, a small arsenal, side HP gauges, and a CPU that solves its shot.
//
// Controls (touch): a bottom control bar with ANG -/+, PWR -/+, WEAPON, FIRE. Hold -/+ to sweep.
// Left-edge Back (nv_gfx_back) exits. Player is the left tank (P1), CPU is the right tank.
#include "nucleo_sdk.h"

// ------------------------------------------------------------------ palette / biomes
#define MAXW 1024
static int W, H, GROUND;              // canvas + terrain bottom (y where dirt ends)

static const unsigned short BSKY[]  = { 0x9DFB, 0xFE38, 0xCE7D, 0x0841, 0x3800 };
static const unsigned short BSKY2[] = { 0x5CDF, 0xFDC8, 0xFFFF, 0x2010, 0x8000 };
static const unsigned short BGRS[]  = { 0x3E68, 0xCC46, 0xE73C, 0x2C68, 0x5140 };
static const unsigned short BDRT[]  = { 0x5AC9, 0x8365, 0x9CD3, 0x2124, 0x3082 };
static int biome;

#define INK    0xFFFF
#define DIM    0x8410
#define P1COL  0x2D7F   // player blue
#define P2COL  0xF9A6   // cpu  red
#define BARBG  0x18E3

// ------------------------------------------------------------------ weapons
typedef struct { const char *name; int crater; int dmg; int kind; int ammo0; } Weap;
// kind: 0 blast, 1 dirt-mound (defensive), 2 tripler (3 shells), 3 nuke
static const Weap WP[] = {
    { "SHELL",  34,  34, 0, -1 },   // infinite
    { "HEAVY",  50,  62, 0,  3 },
    { "TRIPLE", 26,  22, 2,  3 },
    { "DIRT",   44,   0, 1,  2 },
    { "NUKE",   74, 100, 3,  1 },
};
#define NWP 5

// ------------------------------------------------------------------ state
static short top[MAXW];               // surface-y per column (destructible: larger = dug down)
typedef struct { int x, hp, ang, pow, weap, dead; } Tank;
static Tank tk[2];
static int  ammo[2][NWP];
static int  turn;                     // 0 = player, 1 = cpu
static int  wind;                     // -12..+12 (px/s^2 * scale)
static int  phase;                    // 0 aim, 1 flying, 2 settle, 3 gameover
static int  win;                      // gameover: 0 player won, 1 cpu won

// projectile(s) — up to 3 for TRIPLE
#define NPR 3
static struct { int on; float x, y, vx, vy; int wp, owner; } pr[NPR];
static int  cpu_delay;                // frames the cpu "thinks" before firing
static int  msg_t; static const char *msg;

// touch / buttons
static int  prev_down, hold_btn = -1, hold_t;
static int  g_fps, fps_n, fps_t0;      // measured frame rate (diagnostic HUD)
static int  need_scene = 1, need_ov = 1;   // ABI v6: rebuild static bg / redraw dynamic overlay
static void nom(int x, int y, int w, int h);   // union overlay dirty bbox (defined below)

// ------------------------------------------------------------------ tiny math (freestanding)
static float sinf_(float x) {                     // range-reduced Taylor, plenty for aim/physics
    while (x >  3.14159265f) x -= 6.28318531f;
    while (x < -3.14159265f) x += 6.28318531f;
    float x2 = x * x;
    return x * (1 - x2 * (1.0f/6 - x2 * (1.0f/120 - x2 * (1.0f/5040))));
}
static float cosf_(float x) { return sinf_(x + 1.57079633f); }
static float sqrtf_(float v) { if (v <= 0) return 0; float g = v; for (int i = 0; i < 12; i++) g = 0.5f * (g + v / g); return g; }
#define DEG (3.14159265f / 180.0f)

static unsigned s_rng;
static int rnd(int lo, int hi) { s_rng = s_rng * 1664525u + 1013904223u; return lo + (int)((s_rng >> 8) % (unsigned)(hi - lo + 1)); }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void ctext(int cx, int y, const char *s, int col, int sc) { nv_gfx_text(cx - slen(s) * 3 * sc, y, s, col, sc); }

// ------------------------------------------------------------------ terrain
static void gen_terrain(void) {
    biome = rnd(0, 4);
    float base = H * 0.56f;
    float a1 = 34 + rnd(0, 22), a2 = 14 + rnd(0, 12), a3 = 6 + rnd(0, 6);
    float f1 = 0.006f + rnd(0, 30) * 0.0001f, f2 = 0.018f + rnd(0, 40) * 0.0002f, f3 = 0.05f + rnd(0, 40) * 0.001f;
    float p1 = rnd(0, 628) * 0.01f, p2 = rnd(0, 628) * 0.01f, p3 = rnd(0, 628) * 0.01f;
    for (int x = 0; x < W; x++) {
        float h = base + a1 * sinf_(x * f1 + p1) + a2 * sinf_(x * f2 + p2) + a3 * sinf_(x * f3 + p3);
        top[x] = (short)clampi((int)h, (int)(H * 0.28f), GROUND - 4);
    }
}
static void plateau(int cx, int w) {                 // flatten under a tank so it sits level
    int hv = top[clampi(cx, 0, W - 1)];
    for (int x = cx - w; x <= cx + w; x++) if (x >= 0 && x < W) top[x] = (short)hv;
}
// Carve a circular crater (dig, dir=+1) or raise a mound (dir=-1) at (cx,cy) radius r.
static void deform(int cx, int cy, int r, int dir) {
    for (int x = cx - r; x <= cx + r; x++) {
        if (x < 0 || x >= W) continue;
        int dx = x - cx, inside = r * r - dx * dx;
        if (inside <= 0) continue;
        int dy = (int)sqrtf_((float)inside);
        if (dir > 0) { int nt = cy + dy; if (nt > top[x]) top[x] = (short)clampi(nt, 0, GROUND - 1); }
        else         { int nt = cy - dy; if (nt < top[x]) top[x] = (short)clampi(nt, (int)(H * 0.20f), GROUND - 1); }
    }
}

// ------------------------------------------------------------------ audio
static void beep(int f, int ms) { nv_gfx_tone(f, ms); }
static void sfx_fire(void)  { beep(180, 40); }
static void sfx_boom(int big){ beep(big ? 70 : 110, big ? 220 : 120); }
static void sfx_win(void)   { beep(523, 120); beep(659, 120); beep(784, 200); }
static void sfx_lose(void)  { beep(300, 160); beep(220, 240); }

// ------------------------------------------------------------------ match setup
static void new_match(void) {
    gen_terrain();
    wind = rnd(-12, 12);
    for (int t = 0; t < 2; t++) {
        tk[t].x = t == 0 ? (int)(W * 0.12f) + rnd(0, 40) : (int)(W * 0.88f) - rnd(0, 40);
        plateau(tk[t].x, 22);
        tk[t].hp = 100; tk[t].ang = 45; tk[t].pow = 60; tk[t].weap = 0; tk[t].dead = 0;
        for (int w = 0; w < NWP; w++) ammo[t][w] = WP[w].ammo0;
    }
    for (int i = 0; i < NPR; i++) pr[i].on = 0;
    turn = 0; phase = 0; msg_t = 0; need_scene = 1;
}

// select the next in-stock weapon for a tank (wraps)
static void cycle_weap(int t) {
    for (int i = 1; i <= NWP; i++) {
        int w = (tk[t].weap + i) % NWP;
        if (ammo[t][w] != 0) { tk[t].weap = w; return; }
    }
}

// ------------------------------------------------------------------ firing + physics
static void launch(int t) {
    int w = tk[t].weap, dir = t == 0 ? 1 : -1;
    if (ammo[t][w] == 0) { cycle_weap(t); w = tk[t].weap; }
    if (ammo[t][w] > 0) ammo[t][w]--;
    float a = tk[t].ang * DEG, sp = tk[t].pow * 0.11f + 2.0f;
    float bx = tk[t].x + dir * 16, by = top[tk[t].x] - 16;
    int shots = WP[w].kind == 2 ? 3 : 1;
    for (int i = 0; i < NPR; i++) pr[i].on = 0;
    for (int i = 0; i < shots; i++) {
        float da = (i - (shots - 1) * 0.5f) * 6 * DEG;   // tripler spread
        pr[i].on = 1; pr[i].x = bx; pr[i].y = by; pr[i].wp = w; pr[i].owner = t;
        pr[i].vx = dir * cosf_(a + da) * sp;
        pr[i].vy = -sinf_(a + da) * sp;
    }
    phase = 1; sfx_fire(); need_ov = 1;
}

static void explode(int cx, int cy, int w) {
    int r = WP[w].crater;
    if (WP[w].kind == 1) deform(cx, cy, r, -1);          // DIRT raises a protective mound
    else                 deform(cx, cy, r, +1);          // everything else craters
    sfx_boom(WP[w].kind == 3);
    if (WP[w].dmg) {
        for (int t = 0; t < 2; t++) {
            int dx = tk[t].x - cx, dy = (top[tk[t].x] - 10) - cy;
            int d = (int)sqrtf_((float)(dx * dx + dy * dy)), reach = r + 12;
            if (d < reach && !tk[t].dead) {
                int dm = WP[w].dmg * (reach - d) / reach;
                tk[t].hp -= dm; if (tk[t].hp < 0) tk[t].hp = 0;
            }
        }
    }
    for (int t = 0; t < 2; t++) { tk[t].x = clampi(tk[t].x, 4, W - 4); }   // stay on board
}

// advance active projectiles one tick; returns 1 while any is airborne
static int step_proj(void) {
    int any = 0;
    for (int i = 0; i < NPR; i++) {
        if (!pr[i].on) continue;
        pr[i].x += pr[i].vx; pr[i].y += pr[i].vy;
        pr[i].vy += 0.16f;                          // gravity
        pr[i].vx += wind * 0.0011f;                 // wind drift
        int ix = (int)pr[i].x, iy = (int)pr[i].y;
        int hit = 0;
        if (ix < 0 || ix >= W || iy > H) { pr[i].on = 0; continue; }       // flew off — dud
        if (iy >= top[ix]) hit = 1;                                        // ground
        for (int t = 0; t < 2 && !hit; t++)
            if (!tk[t].dead) { int dx = ix - tk[t].x, dy = iy - (top[tk[t].x] - 10); if (dx*dx + dy*dy < 196) hit = 1; }
        if (hit) { explode(ix, iy, pr[i].wp); pr[i].on = 0; }
        else any = 1;
    }
    return any;
}

// ------------------------------------------------------------------ CPU aim: coarse trajectory search
static void cpu_solve(void) {
    int me = 1, foe = 0, bestA = 45, bestP = 60, bestD = 1 << 30;
    // prefer a damaging weapon that's in stock
    int wsel = 0; for (int w = NWP - 1; w >= 0; w--) if (WP[w].dmg && ammo[me][w] != 0) { wsel = w; break; }
    tk[me].weap = wsel;
    float tx = tk[foe].x, ty = top[tk[foe].x] - 10;
    for (int A = 25; A <= 80; A += 3) {
        for (int P = 35; P <= 100; P += 5) {
            float a = A * DEG, sp = P * 0.11f + 2.0f, x = tk[me].x - 16, y = top[tk[me].x] - 16;
            float vx = -cosf_(a) * sp, vy = -sinf_(a) * sp;
            int md = 1 << 30;
            for (int s = 0; s < 400; s++) {
                x += vx; y += vy; vy += 0.16f; vx += wind * 0.0011f;
                if (x < 0 || x > W || y > H) break;
                int dx = (int)(x - tx), dy = (int)(y - ty), d = dx * dx + dy * dy;
                if (d < md) md = d;
                if (y >= top[clampi((int)x, 0, W - 1)]) break;
            }
            if (md < bestD) { bestD = md; bestA = A; bestP = P; }
        }
    }
    int err = rnd(-8, 8);                            // imperfect gunner (keeps it beatable)
    tk[me].ang = clampi(bestA + err, 15, 85);
    tk[me].pow = clampi(bestP + rnd(-6, 6), 20, 100);
}

// ------------------------------------------------------------------ rendering
static void draw_sky(void) {
    int bands = 6, bh = GROUND / bands + 1;
    for (int b = 0; b < bands; b++) {
        // lerp sky -> sky2 top to horizon (cheap: pick nearer endpoint per band)
        nv_gfx_rect(0, b * bh, W, bh, b < bands / 2 ? BSKY[biome] : BSKY2[biome]);
    }
}
static void draw_tank(int t) {
    int x = tk[t].x, y = top[x] - 10, col = t == 0 ? P1COL : P2COL, dir = t == 0 ? 1 : -1;
    if (tk[t].dead) col = 0x630C;
    nv_gfx_rect(x - 12, y - 4, 24, 10, col);                 // hull
    nv_gfx_rect(x - 14, y + 5, 28, 4, 0x4208);               // tracks
    nv_gfx_circle(x, y - 4, 6, col);                         // turret
    if (tk[t].ang >= 0) {                                    // barrel (ang<0 => body only, scene layer)
        float a = tk[t].ang * DEG;
        int bx = x + (int)(dir * cosf_(a) * 20), by = (y - 4) - (int)(sinf_(a) * 20);
        nv_gfx_line(x, y - 4, bx, by, INK);
        nv_gfx_line(x, y - 5, bx, by - 1, INK);
    }
}
static void hpbar(int t) {                                   // side HP gauge
    int x = t == 0 ? 16 : W - 16 - 120, y = 16, col = t == 0 ? P1COL : P2COL;
    nv_gfx_rect(x - 2, y - 2, 124, 20, 0x2104);
    nv_gfx_rect(x, y, 120 * tk[t].hp / 100, 16, col);
    nv_gfx_text(x, y - 20, t == 0 ? "P1" : "CPU", col, 2);
}

// bottom control bar buttons (player turn only). Returns count; fills rects.
typedef struct { int x, y, w, h; const char *lab; } Btn;
static Btn s_btn[6];
static int build_bar(void) {
    int bh = 92, by = H - bh, bw = W / 6;
    const char *L[6] = { "ANG-", "ANG+", "PWR-", "PWR+", "WPN", "FIRE" };
    for (int i = 0; i < 6; i++) { s_btn[i].x = i * bw; s_btn[i].y = by; s_btn[i].w = bw - 4; s_btn[i].h = bh - 8; s_btn[i].lab = L[i]; }
    return 6;
}
static void draw_bar(void) {                         // static: bar bg + 6 buttons (scene layer)
    int bh = 92, by = H - bh;
    nv_gfx_rect(0, by, W, bh, BARBG);
    for (int i = 0; i < 6; i++) {
        nv_gfx_rect(s_btn[i].x + 2, s_btn[i].y + 4, s_btn[i].w, s_btn[i].h, i == 5 ? P2COL : 0x39E7);
        ctext(s_btn[i].x + s_btn[i].w / 2 + 2, s_btn[i].y + 4 + s_btn[i].h / 2 - 8, s_btn[i].lab, INK, 2);
    }
}
static void draw_readouts(void) {                    // dynamic HUD strip above the bar (overlay layer)
    int by = H - 92, w = tk[0].weap; char buf[40];
    nv_gfx_rect(0, by - 32, W, 28, BARBG);           // wipe the strip (overlay repaints it each change)
    buf[0]='A';buf[1]='N';buf[2]='G';buf[3]=' ';buf[4]='0'+tk[0].ang/10;buf[5]='0'+tk[0].ang%10;buf[6]=0;
    nv_gfx_text(16, by - 26, buf, INK, 2);
    buf[0]='P';buf[1]='W';buf[2]='R';buf[3]=' ';buf[4]='0'+tk[0].pow/100;buf[5]='0'+(tk[0].pow/10)%10;buf[6]='0'+tk[0].pow%10;buf[7]=0;
    nv_gfx_text(140, by - 26, buf, INK, 2);
    nv_gfx_text(300, by - 26, WP[w].name, WP[w].kind ? 0xFE60 : INK, 2);
    if (ammo[0][w] >= 0) { char a[6]; a[0]='X';a[1]=' ';a[2]='0'+ammo[0][w]%10;a[3]=0; nv_gfx_text(470, by - 26, a, DIM, 2); }
    else nv_gfx_text(470, by - 26, "X -", DIM, 2);
    nv_gfx_text(W - 200, by - 26, "WIND", DIM, 2);
    int wx = W - 110, wl = wind * 4; nv_gfx_line(wx, by - 18, wx + wl, by - 18, wind ? 0xFE60 : DIM);
    nom(0, by - 32, W, 28);
}

// ---- dirty-rect rendering (ABI v6) --------------------------------------------------------------
// STATIC scene (sky/terrain/tank bodies/hp/title/bar chrome) is drawn rarely and snapshotted as the
// background. The DYNAMIC overlay (barrels, trajectory, projectiles, HUD values) is erased from the
// background and redrawn only when it changes — so the whole 1024x600 is repainted only on real
// scene changes, not every frame.
static int lox0, loy0, lox1, loy1;   // overlay bbox drawn last frame (to erase next)
static int nox0, noy0, nox1, noy1;   // overlay bbox being built this frame
static void nom(int x, int y, int w, int h) {
    int x1 = x + w, y1 = y + h;
    if (nox1 <= nox0) { nox0 = x; noy0 = y; nox1 = x1; noy1 = y1; }
    else { if (x < nox0) nox0 = x; if (y < noy0) noy0 = y; if (x1 > nox1) nox1 = x1; if (y1 > noy1) noy1 = y1; }
}

static void draw_scene(void) {                       // full static background
    draw_sky();
    nv_gfx_terrain(top, W, 0, GROUND, BGRS[biome], BDRT[biome]);
    for (int t = 0; t < 2; t++) { int sv = tk[t].ang; tk[t].ang = -1; draw_tank(t); tk[t].ang = sv; }  // body only
    hpbar(0); hpbar(1);
    ctext(W / 2, 18, "NUCLEO TANKS", INK, 2);
    if (phase != 3) draw_bar();
    if (phase == 3) {
        nv_gfx_rect(W / 2 - 220, H / 2 - 70, 440, 140, 0x18E3);
        ctext(W / 2, H / 2 - 40, win == 0 ? "P1 WINS!" : "CPU WINS", win == 0 ? P1COL : P2COL, 4);
        ctext(W / 2, H / 2 + 20, "TAP TO PLAY AGAIN", INK, 2);
    }
}
static void barrel(int t) {                          // dynamic: rotating cannon
    int x = tk[t].x, y = top[x] - 14, dir = t == 0 ? 1 : -1;
    float a = tk[t].ang * DEG;
    int bx = x + (int)(dir * cosf_(a) * 20), by = y - (int)(sinf_(a) * 20);
    nv_gfx_line(x, y, bx, by, INK); nv_gfx_line(x, y - 1, bx, by - 1, INK);
    int lx = x < bx ? x : bx, ly = (y - 1 < by - 1 ? y - 1 : by - 1);
    nom(lx - 1, ly - 1, (x > bx ? x - bx : bx - x) + 4, (y > by ? y - by : by - y) + 4);
}
static void draw_overlay(void) {
    if (phase == 3) return;                          // game-over card is in the scene layer
    { char f[10]; f[0]='F';f[1]='P';f[2]='S';f[3]=' ';f[4]='0'+(g_fps/10)%10;f[5]='0'+g_fps%10;f[6]=0;
      nv_gfx_rect(16, 14, 80, 20, BARBG); nv_gfx_text(20, 16, f, 0xFFE0, 2); nom(16, 14, 80, 20); }
    draw_readouts();
    ctext(W / 2, 44, turn == 0 ? "YOUR TURN" : "CPU TURN", turn == 0 ? P1COL : P2COL, 2);
    nom(W / 2 - 120, 44, 240, 18);
    barrel(0); barrel(1);
    if (phase == 0 && turn == 0) {                   // trajectory preview
        int dir = 1; float a = tk[0].ang * DEG, sp = tk[0].pow * 0.11f + 2.0f;
        float px2 = tk[0].x + dir * 16, py2 = top[tk[0].x] - 16, vx = dir * cosf_(a) * sp, vy = -sinf_(a) * sp;
        for (int s = 0; s < 90; s++) {
            px2 += vx; py2 += vy; vy += 0.16f; vx += wind * 0.0011f;
            if (px2 < 0 || px2 > W || py2 > H) break;
            if (py2 >= top[clampi((int)px2, 0, W - 1)]) break;
            if ((s & 3) == 0) { nv_gfx_rect((int)px2 - 1, (int)py2 - 1, 2, 2, 0xFE60); nom((int)px2 - 1, (int)py2 - 1, 3, 3); }
        }
    }
    for (int i = 0; i < NPR; i++) if (pr[i].on) {
        int px2 = (int)pr[i].x, py2 = (int)pr[i].y;
        nv_gfx_circle(px2, py2, 4, 0xFFE0); nv_gfx_circle(px2, py2, 2, 0xF800);
        nom(px2 - 5, py2 - 5, 11, 11);
    }
    if (msg_t) { ctext(W / 2, 80, msg, 0xFFE0, 3); nom(W / 2 - 200, 80, 400, 24); }
}

// ------------------------------------------------------------------ input
static int hit(Btn b, int x, int y) { return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h; }
static void press(int i) {
    Tank *p = &tk[0];
    if (i == 0) p->ang = clampi(p->ang - 1, 5, 89);
    else if (i == 1) p->ang = clampi(p->ang + 1, 5, 89);
    else if (i == 2) p->pow = clampi(p->pow - 1, 5, 100);
    else if (i == 3) p->pow = clampi(p->pow + 1, 5, 100);
    else if (i == 4) cycle_weap(0);
    else if (i == 5) launch(0);
    need_ov = 1;
}

NV_EXPORT("run")
void run(void) {
    W = nv_gfx_width(); H = nv_gfx_height();
    if (W > MAXW) W = MAXW;
    GROUND = H - 92;                 // leave the control-bar strip
    s_rng = (unsigned)nv_millis() ^ 0x9E3779B9u;
    nv_gfx_persist(1);               // ABI v6: dirty-rect engine — repaint only what changes
    build_bar();
    new_match();

    while (nv_gfx_present()) {
        if (nv_gfx_back()) break;
        int x, y, down = nv_touch(&x, &y);
        int tap = (!down && prev_down);

        if (phase == 3) {                                   // game over — tap replays
            if (tap) { new_match(); }
        } else if (turn == 0 && phase == 0) {               // player aiming
            if (down) {
                if (hold_btn < 0) {                         // press begins
                    for (int i = 0; i < 6; i++) if (hit(s_btn[i], x, y)) {
                        hold_btn = i; hold_t = nv_millis();
                        if (i < 5) press(i);                // ANG/PWR/WPN act on press; FIRE waits for release
                        break;
                    }
                } else if (hold_btn < 4) {                  // ANG/PWR auto-repeat while held (~8/s)
                    if (nv_millis() - hold_t > 120) { press(hold_btn); hold_t = nv_millis(); }
                }
            } else {
                if (hold_btn == 5 && tap) press(5);         // FIRE confirmed on release
                hold_btn = -1;
            }
        }

        if (phase == 1) {                                   // shot in flight
            if (!step_proj()) { phase = 2; }                // all projectiles resolved
            need_ov = 1;                                     // projectile moved -> repaint overlay only
        } else if (phase == 2) {                            // settle: terrain/hp changed -> rebuild scene
            for (int t = 0; t < 2; t++) if (tk[t].hp <= 0) tk[t].dead = 1;
            if (tk[0].dead || tk[1].dead) { phase = 3; win = tk[0].dead ? 1 : 0; if (win == 0) sfx_win(); else sfx_lose(); }
            else {
                turn ^= 1; phase = 0;
                if (turn == 1) { cpu_delay = 40; msg = "CPU AIMS"; msg_t = 1; }
                else { msg_t = 0; }
            }
            need_scene = 1;
        } else if (turn == 1 && phase == 0) {               // cpu turn
            if (cpu_delay > 0) { cpu_delay--; if (cpu_delay == 20) { cpu_solve(); need_ov = 1; } }
            else { msg_t = 0; launch(1); }
        }

        prev_down = down;

        // fps meter (updates every ~500 ms; a change repaints the overlay so it stays visible)
        if (++fps_n) { int now = nv_millis();
            if (now - fps_t0 >= 500) { int f = fps_n * 1000 / (now - fps_t0); if (f != g_fps) { g_fps = f; need_ov = 1; } fps_n = 0; fps_t0 = now; } }

        // ---- ABI v6 dirty-rect dispatch: rebuild the static scene rarely, the overlay only on change
        if (need_scene) {
            draw_scene(); nv_gfx_bg_save();
            lox0 = 1; lox1 = 0;                              // fresh background: nothing to erase
            nox0 = 1; nox1 = 0; draw_overlay();
            lox0 = nox0; loy0 = noy0; lox1 = nox1; loy1 = noy1;
            need_scene = 0; need_ov = 0;
        } else if (need_ov) {
            if (lox1 > lox0) nv_gfx_bg_restore(lox0, loy0, lox1 - lox0, loy1 - loy0);
            nox0 = 1; nox1 = 0; draw_overlay();
            lox0 = nox0; loy0 = noy0; lox1 = nox1; loy1 = noy1;
            need_ov = 0;
        }
    }
}
