// Nucleo Tanks — a Pocket-Tanks-style artillery duel, running as a WASM app on the SD card
// (host ABI v2 game surface). Two tanks on destructible terrain trade shots across a wind field;
// aim by angle+power (on-screen steppers or drag on the battlefield), pick a weapon, fire. All
// pixels are drawn with the OS's native gfx commands, so the interpreted guest only runs logic.
//
// Freestanding / no libm: trig comes from an embedded SIN table, distances from an integer sqrt.
#include "nucleo_sdk.h"

// ------------------------------------------------------------------ geometry
#define W       960
#define H       456
#define HUD_H   42
#define CTL_TOP (H - 56)     // 400
#define BF_TOP  HUD_H        // 42
#define BF_BOT  CTL_TOP      // 400
#define TANK_HW 14
#define TANK_H  12
#define BARREL  22
#define HIT_R   15
#define TRAIL_N 8
#define PART_N  24

static const short SIN[91] = {
    0, 175, 349, 523, 698, 872, 1045, 1219, 1392, 1564,
    1736, 1908, 2079, 2250, 2419, 2588, 2756, 2924, 3090, 3256,
    3420, 3584, 3746, 3907, 4067, 4226, 4384, 4540, 4695, 4848,
    5000, 5150, 5299, 5446, 5592, 5736, 5878, 6018, 6157, 6293,
    6428, 6561, 6691, 6820, 6947, 7071, 7193, 7314, 7431, 7547,
    7660, 7771, 7880, 7986, 8090, 8192, 8290, 8387, 8480, 8572,
    8660, 8746, 8829, 8910, 8988, 9063, 9135, 9205, 9272, 9336,
    9397, 9455, 9511, 9563, 9613, 9659, 9703, 9744, 9781, 9816,
    9848, 9877, 9903, 9925, 9945, 9962, 9976, 9986, 9994, 9998,
    10000,
};
static int isin(int a) {           // scaled by 10000
    a %= 360; if (a < 0) a += 360;
    if (a <= 90)  return SIN[a];
    if (a <= 180) return SIN[180 - a];
    if (a <= 270) return -SIN[a - 180];
    return -SIN[360 - a];
}
static int icos(int a) { return isin(a + 90); }
static int isqrt(long long v) {     // long is 32-bit on wasm32; use long long for big products
    if (v <= 0) return 0;
    long long x = v, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + v / x) / 2; }
    return (int)x;
}
static int rnd(int lo, int hi) { int r = nv_rand(); if (r < 0) r = -r; return lo + r % (hi - lo + 1); }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ------------------------------------------------------------------ weapons
struct Weapon { const char *name; int radius; int dmg; };
static const struct Weapon WEAP[] = {
    { "SHELL",   30, 34 },
    { "BIG SHOT",46, 52 },
    { "NUKE",    72, 82 },
    { "DIGGER",  40, 16 },
    { "SNIPER",  18, 60 },
};
#define NWEAP 5

// ------------------------------------------------------------------ state
enum { AIM, FLY, BOOM, OVER };
static short terr[W];
static float txf[2], tyf[2];
static int   hp[2], ang[2], pw[2], weap[2];
static int   cur, wind, cpu = 1, phase, winner, cpu_wait;
static float px, py, pvx, pvy;
static int   owner;
static short trx[TRAIL_N], tryy[TRAIL_N];
static int   trn;
static struct { float x, y, vx, vy; int life; } part[PART_N];
static int   pcnt;
static int   bx, by, br, blife;
static int   prev_down, dragging, held_btn = -1, btn_rep;

// physics
#define G     340.0f
#define POW_K 5.0f

// colors (RGB565 — NV_RGB is a function, so fill these at start)
static int C_SKYA, C_SKYB, C_SKYC, C_GRASS, C_DIRT, C_HUD, C_CTL, C_BTN, C_BTN2,
           C_INK, C_DIM, C_T1, C_T2, C_SHELL, C_TRAIL, C_BOOMA, C_BOOMB, C_WIND, C_ACC, C_WIN;

// ------------------------------------------------------------------ buttons
enum { B_ANGD, B_ANGU, B_POWD, B_POWU, B_WPRV, B_WNXT, B_FIRE, B_MODE, B_NEW, B_N };
static const short BX[B_N] = { 10, 62, 118, 170, 250, 448, 528, 650, 724 };
static const short BW[B_N] = { 48, 48, 48,  48,  40,  40,  110, 64,  56  };
static const char *BLBL[B_N] = { "A-", "A+", "P-", "P+", "<", ">", "FIRE", "CPU", "NEW" };
#define BTN_Y (CTL_TOP + 6)
#define BTN_H 44

static int hit_button(int x, int y) {
    if (y < BTN_Y - 2 || y > BTN_Y + BTN_H + 2) return -1;
    for (int i = 0; i < B_N; i++)
        if (x >= BX[i] && x < BX[i] + BW[i]) return i;
    return -1;
}

// ------------------------------------------------------------------ terrain / tanks
static void settle(void) {
    for (int i = 0; i < 2; i++) {
        int x = clampi((int)txf[i], 0, W - 1);
        tyf[i] = terr[x];
    }
}
static void gen_terrain(void) {
    int base = BF_TOP + (BF_BOT - BF_TOP) * 55 / 100;
    int p1 = rnd(0, 359), p2 = rnd(0, 359), p3 = rnd(0, 359);
    for (int x = 0; x < W; x++) {
        int h = base
              + 46 * isin((x * 2 + p1) % 360) / 10000
              + 22 * isin((x * 5 + p2) % 360) / 10000
              + 10 * isin((x * 11 + p3) % 360) / 10000;
        terr[x] = (short)clampi(h, BF_TOP + 26, BF_BOT - 6);
    }
    txf[0] = 78; txf[1] = W - 78;
    for (int i = 0; i < 2; i++) {                 // flatten a stance under each tank
        int cx = (int)txf[i], y = terr[cx];
        for (int x = cx - TANK_HW - 3; x <= cx + TANK_HW + 3; x++)
            if (x >= 0 && x < W) terr[x] = (short)y;
    }
    settle();
}
static void carve(int cx, int cy, int r) {
    for (int dx = -r; dx <= r; dx++) {
        int x = cx + dx;
        if (x < 0 || x >= W) continue;
        int dy = isqrt((long)r * r - (long)dx * dx);
        int bottom = cy + dy;
        if (bottom > terr[x]) terr[x] = (short)clampi(bottom, 0, BF_BOT - 1);
    }
}

// ------------------------------------------------------------------ turn flow
static void new_wind(void) { wind = rnd(-90, 90); }

static void begin_turn(void) {
    phase = AIM;
    trn = pcnt = 0;
    cpu_wait = (cpu && cur == 1) ? 20 : 0;
}
static void new_game(void) {
    hp[0] = hp[1] = 100;
    ang[0] = 45; ang[1] = 135;
    pw[0] = pw[1] = 55;
    weap[0] = weap[1] = 0;
    cur = 0; phase = AIM;
    gen_terrain();
    new_wind();
    begin_turn();
}
static void resolve_turn(void) {
    if (hp[0] <= 0 || hp[1] <= 0) { phase = OVER; winner = hp[0] > 0 ? 0 : 1; nv_gfx_tone(880, 120); nv_gfx_tone(1180, 160); return; }
    cur ^= 1;
    new_wind();
    begin_turn();
}
static void fire(void) {
    int i = cur;
    float fc = icos(ang[i]) * 0.0001f, fs = isin(ang[i]) * 0.0001f;
    float v0 = pw[i] * POW_K;
    px = txf[i] + fc * (BARREL + 4);
    py = (tyf[i] - TANK_H - 2) - fs * (BARREL + 4);
    pvx = fc * v0; pvy = -fs * v0;
    owner = i; trn = pcnt = 0; phase = FLY;
    nv_gfx_tone(150, 55);
}
static void spawn_particles(int x, int y, int n) {
    pcnt = n > PART_N ? PART_N : n;
    for (int p = 0; p < pcnt; p++) {
        int a = rnd(0, 359), sp = rnd(40, 170);
        part[p].x = x; part[p].y = y;
        part[p].vx = icos(a) * 0.0001f * sp;
        part[p].vy = -(isin(a) < 0 ? -isin(a) : isin(a)) * 0.0001f * sp - 20.0f;
        part[p].life = rnd(6, 11);
    }
}
static void explode(int x, int y, int wi) {
    int r = WEAP[wi].radius, dm = WEAP[wi].dmg;
    carve(x, y, r);
    for (int i = 0; i < 2; i++) {
        int dx = (int)txf[i] - x, dy = (int)(tyf[i] - TANK_H / 2) - y;
        int d = isqrt((long)dx * dx + (long)dy * dy);
        if (d < r) hp[i] = clampi(hp[i] - dm * (r - d) / r, 0, 100);
    }
    settle();
    bx = x; by = y; br = r; blife = 10;
    spawn_particles(x, y, 12 + r / 6);
    phase = BOOM;
    nv_gfx_tone(70, 150);
}
static int step_shell(void) {          // returns 1 when the shot resolved
    float sdt = 0.028f / 4;
    for (int s = 0; s < 4; s++) {
        pvx += wind * sdt; pvy += G * sdt;
        px += pvx * sdt;   py += pvy * sdt;
        if (px < 0 || px >= W || py >= BF_BOT) { resolve_turn(); return 1; }
        if (py >= BF_TOP && py >= terr[(int)px]) { explode((int)px, terr[(int)px], weap[owner]); return 1; }
        int e = owner ^ 1;
        int dx = (int)txf[e] - (int)px, dy = (int)(tyf[e] - TANK_H / 2) - (int)py;
        if (isqrt((long)dx * dx + (long)dy * dy) < HIT_R) { explode((int)px, (int)py, weap[owner]); return 1; }
    }
    return 0;
}

// ------------------------------------------------------------------ CPU
static void cpu_fire(void) {
    int me = 1, foe = 0;
    int dx = (int)txf[foe] - (int)txf[me];
    int dist = dx < 0 ? -dx : dx;
    int elev = 42 + rnd(0, 9);
    long long num = (long long)dist * 340LL * 10000LL;
    int s2 = isin(2 * elev); if (s2 < 1) s2 = 1;
    int v0 = isqrt(num / s2);
    int power = v0 * 10 / (int)(POW_K * 10);
    power += (dx < 0 ? 1 : -1) * wind / 8;
    power += rnd(-5, 5);
    ang[me] = (dx >= 0) ? elev : 180 - elev;
    pw[me] = clampi(power, 8, 100);
    weap[me] = 0;
    fire();
}

// ------------------------------------------------------------------ input
static void aim_point(int ix, int iy) {
    int i = cur;
    int ox = (int)txf[i], oy = (int)(tyf[i] - TANK_H - 2);
    int dx = ix - ox, dy = oy - iy;         // dy up = positive
    if (dx == 0 && dy == 0) return;
    long long best = -9000000000LL; int ba = ang[i];
    for (int a = 1; a < 180; a++) {
        long long d = (long long)dx * icos(a) + (long long)dy * isin(a);
        if (d > best) { best = d; ba = a; }
    }
    ang[i] = ba;
    int len = isqrt((long long)dx * dx + (long long)dy * dy);
    pw[i] = clampi(len * 100 / 220, 5, 100);
}
static void btn_action(int b) {
    int i = cur;
    switch (b) {
        case B_ANGD: ang[i] = clampi(ang[i] - 2, 1, 179); nv_gfx_tone(400, 8); break;
        case B_ANGU: ang[i] = clampi(ang[i] + 2, 1, 179); nv_gfx_tone(400, 8); break;
        case B_POWD: pw[i]  = clampi(pw[i] - 2, 1, 100);  nv_gfx_tone(400, 8); break;
        case B_POWU: pw[i]  = clampi(pw[i] + 2, 1, 100);  nv_gfx_tone(400, 8); break;
        case B_WPRV: weap[i] = (weap[i] + NWEAP - 1) % NWEAP; nv_gfx_tone(520, 10); break;
        case B_WNXT: weap[i] = (weap[i] + 1) % NWEAP;         nv_gfx_tone(520, 10); break;
        case B_FIRE: fire(); break;
        case B_MODE: cpu = !cpu; if (cpu && cur == 1) begin_turn(); break;
        case B_NEW:  new_game(); break;
    }
}
static void input(void) {
    int ix, iy, st = nv_touch(&ix, &iy);
    int press = st && !prev_down;
    if (phase == OVER) {
        if (press) { int b = hit_button(ix, iy); if (b == B_FIRE || b == B_NEW) new_game(); }
        prev_down = st; return;
    }
    int human = (phase == AIM) && !(cpu && cur == 1);
    if (human) {
        if (press) {
            int b = hit_button(ix, iy);
            if (b >= 0) { btn_action(b); held_btn = b; btn_rep = 0; }
            else if (iy >= BF_TOP && iy < BF_BOT) { dragging = 1; aim_point(ix, iy); }
        } else if (st) {
            if (dragging) aim_point(ix, iy);
            else if (held_btn >= 0 && held_btn <= B_POWU) { if (++btn_rep % 4 == 0) btn_action(held_btn); }
        }
        if (!st) { dragging = 0; held_btn = -1; }
    }
    prev_down = st;
}
static void update(void) {
    if (phase == AIM) { if (cpu && cur == 1) { if (cpu_wait > 0) cpu_wait--; else cpu_fire(); } }
    else if (phase == FLY) { step_shell(); }
    else if (phase == BOOM) {
        for (int p = 0; p < pcnt; p++) if (part[p].life > 0) {
            part[p].vy += G * 0.028f; part[p].x += part[p].vx * 0.028f; part[p].y += part[p].vy * 0.028f; part[p].life--;
        }
        if (--blife <= 0) resolve_turn();
    }
}

// ------------------------------------------------------------------ drawing
static void draw_tank(int i) {
    int cx = (int)txf[i], base = (int)tyf[i], col = i ? C_T2 : C_T1;
    nv_gfx_rect(cx - TANK_HW, base - 3, TANK_HW * 2, 4, C_DIRT);
    nv_gfx_rect(cx - TANK_HW, base - TANK_H, TANK_HW * 2, TANK_H - 2, col);
    nv_gfx_rect(cx - TANK_HW, base - TANK_H, TANK_HW * 2, 2, C_INK);
    nv_gfx_circle(cx, base - TANK_H, 6, col);
    int fc = icos(ang[i]), fs = isin(ang[i]);
    nv_gfx_line(cx, base - TANK_H - 2, cx + fc * BARREL / 10000, (base - TANK_H - 2) - fs * BARREL / 10000, C_INK);
}
static void draw_guide(void) {
    int i = cur;
    float fc = icos(ang[i]) * 0.0001f, fs = isin(ang[i]) * 0.0001f, v0 = pw[i] * POW_K;
    float x = txf[i] + fc * (BARREL + 4), y = (tyf[i] - TANK_H - 2) - fs * (BARREL + 4);
    float vx = fc * v0, vy = -fs * v0;
    for (int s = 0; s < 60; s++) {
        vx += wind * 0.028f; vy += G * 0.028f; x += vx * 0.028f; y += vy * 0.028f;
        if (x < 0 || x >= W || y >= BF_BOT || (y >= BF_TOP && y >= terr[(int)x])) break;
        if (s % 3 == 0) nv_gfx_rect((int)x, (int)y, 2, 2, C_DIM);
    }
}
static void hbar(int x, int i) {
    // player tag + health bar, colored green->red by hp
    char t[4]; t[0] = 'P'; t[1] = (char)('1' + i); t[2] = 0;
    nv_gfx_text(x, 6, t, cur == i && phase != OVER ? C_ACC : C_DIM, 2);
    int bx0 = x + 30, bw = 168;
    nv_gfx_rect(bx0, 8, bw, 14, C_BTN2);
    int col = NV_RGB(clampi((100 - hp[i]) * 255 / 55, 0, 255), clampi(hp[i] * 255 / 55, 0, 255), 46);
    nv_gfx_rect(bx0, 8, bw * hp[i] / 100, 14, col);
    char n[5]; int v = hp[i], p = 0;
    if (v >= 100) { n[p++]='1'; n[p++]='0'; n[p++]='0'; } else { if (v>=10) n[p++]=(char)('0'+v/10); n[p++]=(char)('0'+v%10); }
    n[p]=0;
    nv_gfx_text(bx0 + bw + 6, 8, n, C_INK, 2);
}
static void draw_hud(void) {
    nv_gfx_rect(0, 0, W, HUD_H, C_HUD);
    hbar(8, 0);
    hbar(W - 240, 1);
    // wind (center)
    char wl[12]; int wv = (wind < 0 ? -wind : wind) / 6;
    int p = 0; wl[p++]='W'; wl[p++]='I'; wl[p++]='N'; wl[p++]='D'; wl[p++]=' ';
    wl[p++] = wind >= 0 ? '>' : '<'; wl[p++]=' ';
    if (wv >= 10) wl[p++] = (char)('0' + wv/10);
    wl[p++] = (char)('0' + wv%10); wl[p]=0;
    nv_gfx_text(W/2 - 44, 8, wl, C_WIND, 2);
    // small wind arrow
    int ax = W/2, ay = 30, al = wind / 4;
    if (al > 2 || al < -2) { nv_gfx_line(ax - al/2, ay, ax + al/2, ay, C_WIND);
        int s = al > 0 ? -4 : 4; nv_gfx_line(ax + al/2, ay, ax + al/2 + s, ay - 3, C_WIND);
        nv_gfx_line(ax + al/2, ay, ax + al/2 + s, ay + 3, C_WIND); }
}
static void draw_controls(void) {
    nv_gfx_rect(0, CTL_TOP, W, H - CTL_TOP, C_CTL);
    for (int i = 0; i < B_N; i++) {
        int col = (i == B_FIRE) ? C_ACC : C_BTN;
        nv_gfx_rect(BX[i], BTN_Y, BW[i], BTN_H, col);
        const char *lbl = (i == B_MODE) ? (cpu ? "CPU" : "2P") : BLBL[i];
        int tw = (int)0; for (const char *q = lbl; *q; q++) tw += 12;
        nv_gfx_text(BX[i] + (BW[i]-tw)/2, BTN_Y + 15, lbl, i == B_FIRE ? C_HUD : C_INK, 2);
    }
    // angle / power readout + weapon name between the < > buttons
    char ap[16]; int p = 0;
    int a = ang[cur]; if (a>=100) ap[p++]=(char)('0'+a/100); if (a>=10) ap[p++]=(char)('0'+(a/10)%10); ap[p++]=(char)('0'+a%10);
    ap[p++]=' '; ap[p++]='P'; ap[p++]=':'; int pv=pw[cur];
    if (pv>=100){ap[p++]='1';ap[p++]='0';ap[p++]='0';} else { if(pv>=10)ap[p++]=(char)('0'+pv/10); ap[p++]=(char)('0'+pv%10);} ap[p]=0;
    nv_gfx_text(BX[B_POWU] + BW[B_POWU] + 8, BTN_Y + 4, "ANG", C_DIM, 1);
    nv_gfx_text(BX[B_POWU] + BW[B_POWU] + 8, BTN_Y + 16, ap, C_INK, 2);
    nv_gfx_rect(296, BTN_Y, 144, BTN_H, C_BTN2);
    nv_gfx_text(302, BTN_Y + 4, WEAP[weap[cur]].name, C_ACC, 2);
    char rd[8]; int q=0; rd[q++]='R'; rd[q++]=':'; int rr=WEAP[weap[cur]].radius;
    if (rr>=10) rd[q++]=(char)('0'+rr/10); rd[q++]=(char)('0'+rr%10); rd[q]=0;
    nv_gfx_text(302, BTN_Y + 24, rd, C_DIM, 2);
}
static void draw(void) {
    // sky bands
    nv_gfx_rect(0, BF_TOP, W, (BF_BOT-BF_TOP)/3, C_SKYA);
    nv_gfx_rect(0, BF_TOP + (BF_BOT-BF_TOP)/3, W, (BF_BOT-BF_TOP)/3, C_SKYB);
    nv_gfx_rect(0, BF_TOP + 2*(BF_BOT-BF_TOP)/3, W, (BF_BOT-BF_TOP)/3 + 2, C_SKYC);
    // terrain (one call)
    nv_gfx_terrain(terr, W, 0, BF_BOT, C_GRASS, C_DIRT);
    draw_tank(0); draw_tank(1);
    if (phase == AIM && !(cpu && cur == 1)) draw_guide();
    // trail + shell
    for (int k = trn - 1; k >= 0; k--) nv_gfx_circle(trx[k], tryy[k], 1 + (TRAIL_N-1-k)/3, C_TRAIL);
    if (phase == FLY) { nv_gfx_circle((int)px, (int)py, 3, C_SHELL); nv_gfx_circle((int)px, (int)py, 1, C_WIN); }
    // particles + blast
    if (phase == BOOM) {
        int r = br - (blife * br) / 12; if (r < 4) r = 4;
        nv_gfx_circle(bx, by, r, (blife & 1) ? C_BOOMA : C_BOOMB);
        for (int pi = 0; pi < pcnt; pi++) if (part[pi].life > 0)
            nv_gfx_circle((int)part[pi].x, (int)part[pi].y, part[pi].life > 5 ? 2 : 1, part[pi].life > 4 ? C_BOOMA : C_DIM);
    }
    draw_hud();
    draw_controls();
    if (phase == OVER) {
        const char *w = winner == 0 ? "P1 WINS" : (cpu ? "CPU WINS" : "P2 WINS");
        int tw = 0; for (const char *q = w; *q; q++) tw += 24;
        nv_gfx_rect(W/2 - tw/2 - 16, BF_TOP + 120, tw + 32, 70, C_HUD);
        nv_gfx_text(W/2 - tw/2, BF_TOP + 130, w, C_WIN, 4);
        nv_gfx_text(W/2 - 66, BF_TOP + 166, "TAP FIRE OR NEW", C_INK, 1);
    }
}

// ------------------------------------------------------------------ entry
NV_EXPORT("run")
void run(void) {
    C_SKYA=NV_RGB(28,86,150); C_SKYB=NV_RGB(90,150,205); C_SKYC=NV_RGB(150,195,230);
    C_GRASS=NV_RGB(96,190,86); C_DIRT=NV_RGB(126,92,60);
    C_HUD=NV_RGB(22,26,34); C_CTL=NV_RGB(30,34,44); C_BTN=NV_RGB(58,66,84); C_BTN2=NV_RGB(44,50,64);
    C_INK=NV_RGB(235,240,248); C_DIM=NV_RGB(150,160,178);
    C_T1=NV_RGB(70,130,255); C_T2=NV_RGB(255,92,74);
    C_SHELL=NV_RGB(255,226,74); C_TRAIL=NV_RGB(255,150,60);
    C_BOOMA=NV_RGB(255,210,60); C_BOOMB=NV_RGB(255,110,30); C_WIND=NV_RGB(240,246,252);
    C_ACC=NV_RGB(255,196,64); C_WIN=NV_RGB(255,255,255);

    new_game();
    while (nv_gfx_present()) {               // fixed timestep, paced by the host at present()
        input();
        update();
        if (phase == FLY) {                  // sample the shell trail after it moved
            for (int k = TRAIL_N - 1; k > 0; k--) { trx[k] = trx[k-1]; tryy[k] = tryy[k-1]; }
            trx[0] = (short)px; tryy[0] = (short)py; if (trn < TRAIL_N) trn++;
        }
        draw();
    }
}
