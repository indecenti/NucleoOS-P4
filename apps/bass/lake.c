// lake.c — Vertice Bass: the four lakes. One coordinate frame for two worlds: above the water
// (Mode-7 water surface, shoreline ring, forest panorama, reeds, logs, rocks, lily pads, the boat)
// and below it (Mode-7 lake bed, weed forests, sunken logs, boulders, drifting specks). Each fishing
// spot exists in both, at the same x/z, so what you aim at from the boat is what you fish under.
#include "bass.h"

Spot g_spot[NSPOTS];
int g_fx_splash, g_fx_bubble, g_fx_dust, g_boat;

// Species mix: bass, trout, pike, catfish, carp, perch, zander, gold (percent; gold = rare trophy).
const Stage g_stage[NSTAGES] = {
    { "LAGO ALPINO", "ALPINE LAKE", 170, 2.4f,
      C565(40, 110, 220), C565(190, 220, 248), C565(44, 110, 150), C565(20, 70, 84),
      0xFFF4E0, 0x4A5868, 55, { 30, 32, 8, 2, 4, 20, 2, 2 }, C565(30, 72, 40), C565(118, 128, 170) },
    { "PALUDE AL TRAMONTO", "SUNSET MARSH", 170, 4.0f,
      C565(70, 60, 140), C565(255, 170, 110), C565(70, 90, 100), C565(40, 58, 40),
      0xFFB070, 0x5A4858, 12, { 34, 2, 18, 12, 20, 8, 4, 2 }, C565(34, 50, 30), C565(120, 96, 120) },
    { "DIGA DI NOTTE", "NIGHT DAM", 170, 6.0f,
      C565(8, 12, 40), C565(40, 60, 110), C565(20, 36, 60), C565(8, 22, 36),
      0x9AB4FF, 0x283048, 35, { 20, 4, 14, 30, 10, 4, 16, 2 }, C565(12, 26, 22), C565(60, 70, 100) },
    { "CANYON ROSSO", "RED CANYON", 170, 6.8f,
      C565(60, 110, 200), C565(250, 196, 140), C565(40, 110, 120), C565(40, 60, 50),
      0xFFD8A0, 0x584840, 30, { 36, 10, 10, 18, 12, 4, 8, 2 }, C565(90, 96, 50), C565(190, 90, 60) },
    { "LAGO D'AUTUNNO", "AUTUMN LAKE", 185, 7.6f,
      C565(70, 120, 200), C565(230, 214, 190), C565(50, 90, 110), C565(30, 56, 50),
      0xFFE4B0, 0x505048, 40, { 26, 12, 20, 8, 12, 10, 10, 2 }, C565(170, 90, 30), C565(130, 120, 140) },
    { "LAGO DEL RE", "KING'S LAKE", 200, 8.8f,
      C565(30, 90, 200), C565(200, 226, 250), C565(30, 104, 140), C565(14, 62, 80),
      0xFFF0D0, 0x485868, 60, { 26, 10, 22, 12, 10, 4, 12, 4 }, C565(26, 66, 40), C565(118, 128, 170) },
};

#define MAXG 160
static int s_above[MAXG], s_nabove, s_under[MAXG], s_nunder;
static int s_stage, s_tex_water, s_tex_bed, s_tex_pano;
static uint16_t s_forest;
static void add_above(int id) { if (id >= 0 && s_nabove < MAXG) s_above[s_nabove++] = id; }
static void add_under(int id) { if (id >= 0 && s_nunder < MAXG) s_under[s_nunder++] = id; }
// Weeds are tracked with their positions, so the ones right in front of the lens can be hidden.
#define MAXW 128
static int s_weed[MAXW], s_nweed;
static float s_wx[MAXW], s_wz[MAXW];
static void weed_add(int id, float x, float z) {
    add_under(id);
    if (id >= 0 && s_nweed < MAXW) { s_weed[s_nweed] = id; s_wx[s_nweed] = x; s_wz[s_nweed] = z; s_nweed++; }
}
void lake_clear_near(float x, float z, float r) {
    for (int i = 0; i < s_nweed; i++) {
        const float dx = s_wx[i] - x, dz = s_wz[i] - z;
        vx_obj_show(s_weed[i], dx * dx + dz * dz > r * r);
    }
}
static void background(int id) { if (id >= 0) vx_obj_depth(id, 0, VX_DEPTH_NOTEST | VX_DEPTH_NOWRITE); }

static uint16_t mix16(uint16_t a, uint16_t b, int t) {   // t 0..256
    const int r = (a >> 11) + (((b >> 11) - (a >> 11)) * t >> 8);
    const int g = ((a >> 5) & 63) + ((((b >> 5) & 63) - ((a >> 5) & 63)) * t >> 8);
    const int bl = (a & 31) + (((b & 31) - (a & 31)) * t >> 8);
    return (uint16_t)((r << 11) | (g << 5) | bl);
}
static int r5(uint16_t c) { return (c >> 11) << 3; }
static int g6(uint16_t c) { return ((c >> 5) & 63) << 2; }
static int b5(uint16_t c) { return (c & 31) << 3; }

// ---- textures ------------------------------------------------------------------------------------------
// Open water: the stage's water colour, long soft swells and sun glints.
static int tex_water(const Stage *st) {
    const int R = r5(st->water), G = g6(st->water), B = b5(st->water);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            const float a = sinf_((x + y * 0.4f) * 0.20f) + 0.6f * sinf_((x * 0.7f - y) * 0.31f);
            const int d = (int)(a * 9) + rnd(5);
            uint16_t c = rgb(R + d, G + d, B + d + 4);
            if (a > 1.35f && rnd(3) == 0) c = rgb(R + 90, G + 90, B + 80);   // glints
            tex_buf[y * 64 + x] = c;
        }
    return vx_texture(tex_buf, 64, 64, 0);
}
// Lake bed: silt with pebbles and dark weed patches.
static int tex_bed(const Stage *st) {
    const int R = r5(st->deep) + 70, G = g6(st->deep) + 60, B = b5(st->deep) + 30;
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            int d = rnd(16) - 8;
            uint16_t c = rgb(R + d, G + d, B + d / 2);
            const int px = x & 15, py = y & 15, h = ((x >> 4) * 7 + (y >> 4) * 13) & 7;
            if ((px - 4 - h) * (px - 4 - h) + (py - 6) * (py - 6) < 6) c = rgb(R + 34, G + 30, B + 26);   // pebbles
            if (((x * 3 + y * 5) % 37) < 3) c = rgb(R - 30, G - 10, B - 20);                          // weed dark
            tex_buf[y * 64 + x] = c;
        }
    return vx_texture(tex_buf, 64, 64, 0);
}
// Reeds (billboard, v = 0 at the bottom): a tuft of blades with brown cattail heads.
static int tex_reeds(void) {
    for (int i = 0; i < 64 * 64; i++) tex_buf[i] = KEY;
    for (int k = 0; k < 11; k++) {
        const int x0 = 6 + rnd(52), top = 34 + rnd(28), lean = rnd(9) - 4;
        for (int y = 0; y < top; y++) {
            const int x = x0 + lean * y / 64;
            if (x < 1 || x > 62) continue;
            const uint16_t c = rgb(70 + y + rnd(10), 120 + y / 2 + rnd(12), 50);
            tex_buf[y * 64 + x] = c; tex_buf[y * 64 + x - 1] = rgb(50 + y, 96 + y / 2, 40);
            if (k % 3 == 0 && y > top - 9) { tex_buf[y * 64 + x] = rgb(110, 70, 40); tex_buf[y * 64 + x + 1] = rgb(90, 56, 30); }
        }
    }
    return vx_texture(tex_buf, 64, 64, VX_TEX_KEY);
}
// Underwater weed: tall wavy fronds, bottom rooted.
static int tex_weed(void) {
    for (int i = 0; i < 64 * 64; i++) tex_buf[i] = KEY;
    for (int k = 0; k < 7; k++) {
        const int x0 = 8 + rnd(48), top = 40 + rnd(24), ph = rnd(64);
        for (int y = 0; y < top; y++) {
            const int x = x0 + (int)(sinf_((y + ph) * 0.18f) * 4);
            for (int w = -1; w <= 1; w++) {
                if (x + w < 0 || x + w > 63) continue;
                tex_buf[y * 64 + x + w] = rgb(40 + w * 12 + y / 2, 110 + y + w * 14, 60 + y / 3);
            }
            if (y % 7 == 3 && x + 3 < 64) { tex_buf[y * 64 + x + 2] = rgb(70, 150, 70); tex_buf[y * 64 + x + 3] = rgb(60, 130, 60); }
        }
    }
    return vx_texture(tex_buf, 64, 64, VX_TEX_KEY);
}
// Lily pad (flat decal, 32x32): notched green disc, veins, a pink flower on some.
static int tex_pad(void) {
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
            const int dx = x - 16, dy = y - 16, d = dx * dx + dy * dy;
            uint16_t c = KEY;
            if (d < 14 * 14 && !(dx > 0 && dy > -2 && dy < 2 + dx / 3)) {
                c = rgb(60 + (d < 30 ? 30 : 0), 130 + rnd(10), 60);
                if ((dx == 0 || dy == 0 || dx == dy || dx == -dy) && d > 9) c = rgb(96, 160, 80);
                if (d > 12 * 12) c = rgb(40, 96, 40);
            }
            tex_buf[y * 32 + x] = c;
        }
    return vx_texture(tex_buf, 32, 32, VX_TEX_KEY);
}
// Pine for the shore (billboard).
static int tex_pine(int dark) {
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            uint16_t c = KEY;
            const int dx = x - 32, h = y;
            if (h < 10 && dx >= -2 && dx <= 2) c = rgb(90, 60, 36);
            for (int tier = 0; tier < 4; tier++) {
                const int base = 8 + tier * 13, top = base + 22;
                if (h >= base && h < top) {
                    const int half = (top - h) * (20 - tier * 4) / 22;
                    if (dx >= -half && dx <= half) {
                        const int lit = dx < 0 ? 14 : -10;
                        c = rgb(r5(s_forest) + lit + rnd(8) - dark, g6(s_forest) + 20 + lit + rnd(10) - dark * 2,
                                b5(s_forest) + 10 - dark);
                    }
                }
            }
            tex_buf[y * 64 + x] = c;
        }
    return vx_texture(tex_buf, 64, 64, VX_TEX_KEY);
}

// 360° shoreline panorama, 1024x128, horizon at row PANO_HR: far ranges fading into the sky's haze,
// a dark forest band on the shore, clouds (by day) or stars and a moon (at night).
#define PANO_W 1024
#define PANO_H 128
#define PANO_HR 124
static float ridge(int x, float a0, float f0, float a1, float f1, float a2, float f2, float ph) {
    const float a = 2 * PI_F * x / PANO_W;
    return a0 * sinf_(f0 * a + ph) + a1 * fabsf_(sinf_(f1 * a + ph * 2)) + a2 * sinf_(f2 * a + ph * 3);
}
static int tex_panorama(const Stage *st, int night) {
    static uint8_t hf[PANO_W], hm[PANO_W], hs[PANO_W];
    const float ph = rnd(100) * 0.06f;
    for (int x = 0; x < PANO_W; x++) {
        const float f = 34 + ridge(x, 10, 2, 12, 5, 3, 13, ph), m = 18 + ridge(x, 5, 3, 7, 9, 2, 21, ph + 1);
        hf[x] = (uint8_t)clampf(f, 12, 70);
        hm[x] = (uint8_t)clampf(m, 8, 40);
        hs[x] = (uint8_t)(7 + rnd(4) + ((x * 7) % 11 < 4 ? 3 : 0));    // forest crowns
    }
    const int tex = vx_texture_new(PANO_W, PANO_H, KEY, VX_TEX_KEY);
    if (tex < 0) return -1;
    const uint16_t haze = st->sky_bot;
    for (int y0 = 0; y0 < PANO_H; y0 += 4) {
        for (int y = y0; y < y0 + 4; y++) {
            const int up = PANO_HR - y;
            for (int x = 0; x < PANO_W; x++) {
                uint16_t c = KEY;
                if (night) {
                    if (((x * 131 + y * 17) % 997) == 0 && up > 20) c = rgb(230, 230, 255);   // stars
                    const int mx = x - 700, my = y - 30;
                    if (mx * mx + my * my < 64) c = rgb(245, 245, 225);                       // moon
                } else {
                    const int cx = (x % 256) - 128, cy = (y - (40 + (x / 256) * 9)) * 3;
                    if (cx * cx / 4 + cy * cy < 200 + (x / 256) * 60 && up > 40) c = rgb(250, 250, 255);
                }
                const int xl = (x + PANO_W - 1) % PANO_W, xr = (x + 1) % PANO_W;
                if (up < hf[x]) {
                    const int lit = hf[xr] <= hf[xl];
                    c = mix16(lit ? st->rock : mix16(st->rock, 0, 50), haze, night ? 120 : 140);
                    if (!night && up > 50 && hf[x] - up < 4) c = rgb(245, 248, 255);
                }
                if (up < hm[x]) {
                    const int lit = hm[xr] <= hm[xl];
                    c = mix16(lit ? rgb(70, 110, 110) : rgb(52, 88, 96), haze, night ? 90 : 90);
                }
                if (up < hs[x]) c = mix16(mix16(st->forest, (x * 5) % 3 ? 0xFFFF : 0, 20), haze, night ? 60 : 40);
                tex_buf[(y - y0) * PANO_W + x] = c;
            }
        }
        vx_texture_write(tex, 0, y0, PANO_W, 4, tex_buf);
    }
    return tex;
}

// ---- building ------------------------------------------------------------------------------------------
static void place_spots(void) {
    const int kinds[NSPOTS] = { SPOT_WEEDS, SPOT_LOG, SPOT_PADS, SPOT_ROCKS, SPOT_WEEDS, SPOT_LOG };
    int n = 0, tries = 0;
    while (n < NSPOTS && tries++ < 400) {
        const float a = (rnd(1000) / 1000.0f - 0.5f) * 1.9f, d = 520 + rnd(1250);
        const float x = sinf_(a) * d, z = cosf_(a) * d;
        int ok = 1;
        for (int k = 0; k < n; k++) {
            const float ex = x - g_spot[k].x, ez = z - g_spot[k].z;
            if (ex * ex + ez * ez < 430 * 430) ok = 0;
        }
        if (!ok) continue;
        g_spot[n].x = x; g_spot[n].z = z; g_spot[n].r = 150 + rnd(80); g_spot[n].kind = kinds[n];
        n++;
    }
    for (; n < NSPOTS; n++) { g_spot[n].x = (n - 3) * 400.0f; g_spot[n].z = 1400; g_spot[n].r = 150; g_spot[n].kind = SPOT_WEEDS; }
}

int lake_spot_near(float x, float z) {
    for (int k = 0; k < NSPOTS; k++) {
        const float ex = x - g_spot[k].x, ez = z - g_spot[k].z, r = g_spot[k].r + 180;
        if (ex * ex + ez * ez < r * r) return k;
    }
    return -1;
}

static void build_above(const Stage *st, int night) {
    // Shore: a ring of flat grass quads far out (painter's background, drawn over the water).
    const int grass = vx_material(night ? C565(24, 44, 30) : mix16(st->forest, C565(96, 150, 70), 150), VX_UNLIT, 255, -1, 0);
    const int sand = vx_material(night ? C565(60, 58, 50) : C565(170, 156, 120), VX_UNLIT, 255, -1, 0);
    for (int k = 0; k < 32; k++) {
        const float a0 = k * 2 * PI_F / 32, a1 = (k + 1) * 2 * PI_F / 32;
        const float r0 = 2700 + 160 * sinf_(k * 1.7f), r1 = 2700 + 160 * sinf_((k + 1) * 1.7f);
        const int s0 = mb_v(sinf_(a0) * r0, 1, cosf_(a0) * r0, 0, 0), s1 = mb_v(sinf_(a1) * r1, 1, cosf_(a1) * r1, 0, 0);
        const int m0 = mb_v(sinf_(a0) * (r0 + 70), 1, cosf_(a0) * (r0 + 70), 0, 0);
        const int m1 = mb_v(sinf_(a1) * (r1 + 70), 1, cosf_(a1) * (r1 + 70), 0, 0);
        const int o0 = mb_v(sinf_(a0) * 9000, 1, cosf_(a0) * 9000, 0, 0), o1 = mb_v(sinf_(a1) * 9000, 1, cosf_(a1) * 9000, 0, 0);
        mb_quad(s0, s1, m1, m0, sand, 0, -1000, 0);
        mb_quad(m0, m1, o1, o0, grass, 0, -1000, 0);
    }
    { const int id = mb_commit(grass, 0); background(id); add_above(id); }
    // Shore pines.
    const int pine[2] = { vx_material(0xFFFF, night ? VX_GOURAUD : VX_UNLIT, 255, tex_pine(0), 0),
                          vx_material(0xFFFF, night ? VX_GOURAUD : VX_UNLIT, 255, tex_pine(20), 0) };
    const int proto[2] = { vx_prim(VX_BILLBOARD, 300, 420, 0, pine[0], -1), vx_prim(VX_BILLBOARD, 260, 360, 0, pine[1], -1) };
    add_above(proto[0]); add_above(proto[1]);
    vx_obj_pos(proto[0], 0, 210, 2950); vx_obj_pos(proto[1], 300, 180, 3000);
    for (int i = 0; i < 70; i++) {
        const float a = rnd(6283) / 1000.0f, r = 2820 + rnd(700);
        const int k = rnd(2), t = vx_clone(proto[k]);
        if (t < 0) break;
        vx_obj_pos(t, iroundf(sinf_(a) * r), k ? 180 : 210, iroundf(cosf_(a) * r));
        add_above(t);
    }
    // Spots on the surface.
    const int reeds = vx_material(0xFFFF, VX_UNLIT, 255, tex_reeds(), 0);
    const int reed0 = vx_prim(VX_BILLBOARD, 110, 150, 0, reeds, -1);
    const int pads = vx_material(0xFFFF, VX_UNLIT, 255, tex_pad(), 0);
    const int bark = vx_material(C565(96, 70, 44), VX_GOURAUD, 255, -1, 0);
    const int stone = vx_material(C565(120, 124, 130), VX_GOURAUD, 255, -1, 0);
    vx_obj_pos(reed0, 0, -500, 0);
    add_above(reed0);
    for (int k = 0; k < NSPOTS; k++) {
        const Spot *s = &g_spot[k];
        if (s->kind == SPOT_WEEDS || s->kind == SPOT_PADS) {
            const int n = s->kind == SPOT_WEEDS ? 7 : 3;
            for (int i = 0; i < n; i++) {
                const int t = vx_clone(reed0);
                if (t < 0) break;
                vx_obj_pos(t, iroundf(s->x + rnd((int)s->r * 2) - s->r), 70, iroundf(s->z + rnd((int)s->r * 2) - s->r));
                add_above(t);
            }
        }
        if (s->kind == SPOT_PADS) {                 // a raft of lily pads, flat on the water
            for (int i = 0; i < 12; i++) {
                const float px = s->x + rnd((int)s->r * 2) - s->r, pz = s->z + rnd((int)s->r * 2) - s->r, q = 30 + rnd(14);
                const int a = mb_v(px - q, 2, pz - q, 0, 0), b = mb_v(px + q, 2, pz - q, 1024, 0);
                const int c = mb_v(px + q, 2, pz + q, 1024, 1024), d = mb_v(px - q, 2, pz + q, 0, 1024);
                mb_quad(a, b, c, d, pads, px, -100, pz);
            }
            const int id = mb_commit(pads, 1); background(id); add_above(id);
        }
        if (s->kind == SPOT_LOG) {                  // a dead tree lying in the water, branch up
            mb_box(s->x - 140, 0, s->z - 14, s->x + 140, 22, s->z + 14, bark);
            mb_box(s->x + 60, 0, s->z - 8, s->x + 76, 110, s->z + 8, bark);
            add_above(mb_commit(bark, 0));
        }
        if (s->kind == SPOT_ROCKS) {
            for (int i = 0; i < 4; i++) {
                const float px = s->x + rnd(200) - 100, pz = s->z + rnd(200) - 100, q = 24 + rnd(30);
                mb_box(px - q, 0, pz - q * 0.8f, px + q, q * 0.9f, pz + q * 0.8f, stone);
            }
            add_above(mb_commit(stone, 0));
        }
    }
    // Tournament rivals: small bass boats with an angler, anchored far out on the lake.
    {
        const int rh = vx_material(C565(30, 70, 150), VX_GOURAUD, 255, -1, 100);
        const int rt = vx_material(C565(236, 236, 230), VX_GOURAUD, 255, -1, 60);
        const int shirt = vx_material(C565(250, 200, 40), VX_GOURAUD, 255, -1, 0);
        const int skin = vx_material(C565(230, 180, 140), VX_GOURAUD, 255, -1, 0);
        for (int b = 0; b < 3; b++) {
            const float a = (b - 1) * 1.05f + (rnd(40) - 20) / 100.0f, d = 2000 + rnd(500);
            const float x = sinf_(a) * d, z = cosf_(a) * d;
            mb_box(x - 30, 0, z - 90, x + 30, 26, z + 80, rh);
            mb_box(x - 32, 26, z - 92, x + 32, 30, z + 82, rt);
            mb_box(x - 10, 30, z - 20, x + 10, 62, z - 6, shirt);      // angler
            mb_box(x - 7, 62, z - 18, x + 7, 76, z - 8, skin);
            mb_box(x + 8, 50, z - 14, x + 10, 150, z - 12, rt);         // rod up
        }
        add_above(mb_commit(rh, 0));
    }
    // A wooden pier and a fishing cabin on the shore.
    {
        const int wood = vx_material(C565(140, 104, 66), VX_GOURAUD, 255, -1, 0);
        const int wall = vx_material(C565(170, 126, 80), VX_GOURAUD, 255, -1, 0);
        const int roof = vx_material(C565(120, 40, 34), VX_GOURAUD, 255, -1, 0);
        const float a = 0.55f, r0 = 2380, r1 = 2780;
        const float ux = sinf_(a), uz = cosf_(a), px = uz, pz = -ux;
        for (int k = 0; k < 8; k++) {                                   // planks + posts toward the shore
            const float r = r0 + k * (r1 - r0) / 8, cx = ux * r, cz = uz * r;
            mb_box(cx - 34, 16, cz - 34, cx + 34, 22, cz + 34, wood);
            if (k % 2 == 0) { mb_box(cx + px * 30 - 5, 0, cz + pz * 30 - 5, cx + px * 30 + 5, 30, cz + pz * 30 + 5, wood);
                              mb_box(cx - px * 30 - 5, 0, cz - pz * 30 - 5, cx - px * 30 + 5, 30, cz - pz * 30 + 5, wood); }
        }
        const float hx = ux * 2900, hz = uz * 2900;
        mb_box(hx - 90, 0, hz - 70, hx + 90, 110, hz + 70, wall);
        const int e0 = mb_v(hx - 100, 110, hz - 80, 0, 0), e1 = mb_v(hx + 100, 110, hz - 80, 0, 0);
        const int e2 = mb_v(hx + 100, 110, hz + 80, 0, 0), e3 = mb_v(hx - 100, 110, hz + 80, 0, 0);
        const int ridge0 = mb_v(hx - 100, 170, hz, 0, 0), ridge1 = mb_v(hx + 100, 170, hz, 0, 0);
        mb_quad(e0, e1, ridge1, ridge0, roof, hx, 120, hz + 40);
        mb_quad(e3, e2, ridge1, ridge0, roof, hx, 120, hz - 40);
        mb_tri(e0, e3, ridge0, roof, hx + 50, 130, hz); mb_tri(e1, e2, ridge1, roof, hx - 50, 130, hz);
        add_above(mb_commit(wood, 0));
    }
    // A proper bass boat, nose +Z (the camera sits just behind the stern): a raked hull in metal-flake
    // red with a white stripe, carpeted deck, raised bow casting deck with a trolling motor, two
    // pedestal seats, the side console with its windscreen, rod lockers, and the big outboard.
    const int hull = vx_material(C565(186, 34, 30), VX_GOURAUD, 255, -1, 140);
    const int stripe = vx_material(C565(240, 238, 230), VX_GOURAUD, 255, -1, 120);
    const int carpet = vx_material(C565(70, 76, 90), VX_FLAT, 255, -1, 0);
    const int carpet2 = vx_material(C565(56, 62, 76), VX_FLAT, 255, -1, 0);
    const int seat = vx_material(C565(230, 226, 214), VX_GOURAUD, 255, -1, 60);
    const int black = vx_material(C565(24, 26, 30), VX_GOURAUD, 255, -1, 120);
    const int chrome = vx_material(C565(200, 204, 214), VX_GOURAUD, 255, -1, 160);
    const int glass = vx_material(C565(150, 190, 210), VX_GOURAUD, 255, -1, 200);
    // Hull: stern 110 wide, sides curving in to the bow point.
    static const float hz[6] = { -170, -90, 0, 80, 140, 175 }, hw[6] = { 58, 60, 58, 48, 30, 4 };
    int top[6][2], bot[6][2];
    for (int i = 0; i < 6; i++)
        for (int s = 0; s < 2; s++) {
            const float x = (s ? 1 : -1) * hw[i];
            top[i][s] = mb_v(x, 40 + (i >= 4 ? 4 : 0), hz[i], 0, 0);
            bot[i][s] = mb_v(x * 0.8f, 4, hz[i] - (i == 5 ? 8 : 0), 0, 0);
        }
    for (int i = 0; i < 5; i++)
        for (int s = 0; s < 2; s++) {
            const float cx = (s ? -20.0f : 20.0f);
            const int mid0 = mb_v((s ? 1 : -1) * hw[i] * 0.95f, 30, hz[i], 0, 0);
            const int mid1 = mb_v((s ? 1 : -1) * hw[i + 1] * 0.95f, 30 + (i + 1 >= 4 ? 3 : 0), hz[i + 1], 0, 0);
            mb_quad(top[i][s], top[i + 1][s], mid1, mid0, stripe, cx, 22, (hz[i] + hz[i + 1]) / 2);   // white band
            mb_quad(mid0, mid1, bot[i + 1][s], bot[i][s], hull, cx, 22, (hz[i] + hz[i + 1]) / 2);
        }
    mb_quad(top[0][0], top[0][1], bot[0][1], bot[0][0], hull, 0, 22, -120);          // transom
    // Deck: aft and mid carpet, the raised bow casting deck.
    for (int i = 0; i < 5; i++) {
        const int m = i >= 3 ? carpet2 : carpet;
        const float y0 = i >= 3 ? 44.5f : 40.5f, y1 = i + 1 >= 3 ? 44.5f : 40.5f;
        const int a = mb_v(-hw[i] + 3, y0, hz[i], 0, 0), b = mb_v(hw[i] - 3, y0, hz[i], 0, 0);
        const int c = mb_v(hw[i + 1] - 3 > 1 ? hw[i + 1] - 3 : 1, y1, hz[i + 1], 0, 0), d = mb_v(-(hw[i + 1] - 3 > 1 ? hw[i + 1] - 3 : 1), y1, hz[i + 1], 0, 0);
        mb_quad(a, b, c, d, m, 0, 0, (hz[i] + hz[i + 1]) / 2);
    }
    mb_box(-46, 40, 60, 46, 45, 66, carpet2);                                         // step to the bow deck
    mb_box(-4, 40, -150, 4, 46, 150, black);                                          // rod locker seam
    mb_box(-18, 40, -128, -8, 64, -118, chrome); mb_box(-24, 64, -132, -2, 70, -114, seat);   // rear seat
    mb_box(-6, 44, 96, 4, 70, 106, chrome); mb_box(-12, 70, 92, 10, 76, 110, seat);           // bow seat
    mb_box(22, 40, -40, 54, 72, -10, stripe);                                         // console
    mb_box(24, 72, -30, 52, 84, -28, glass);                                          // windscreen
    mb_box(36, 72, -24, 40, 78, -20, black);                                          // wheel
    mb_box(-12, 12, -206, 12, 70, -172, black);                                       // outboard leg + cowl
    mb_box(-16, 60, -212, 16, 92, -176, black);
    mb_box(-15, 88, -210, 15, 92, -178, chrome);
    mb_box(-2, 44, 150, 2, 90, 154, chrome);                                          // trolling motor shaft
    mb_box(-8, 88, 144, 8, 96, 160, black);
    mb_box(-60, 42, -60, -56, 48, 90, chrome);                                        // grab rail
    g_boat = mb_commit(hull, 0);
    add_above(g_boat);
}

// The surface seen from below: bright, rippled, with caustic lines — the ceiling of the underwater view.
static int tex_ceiling(const Stage *st) {
    const int R = r5(st->water), G = g6(st->water), B = b5(st->water);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            const float a = sinf_(x * 0.29f + sinf_(y * 0.21f) * 2.2f) + sinf_(y * 0.33f + sinf_(x * 0.17f) * 2.0f);
            const int k = a > 1.3f ? 90 : a > 0.8f ? 50 : 20;
            tex_buf[y * 64 + x] = rgb(R + k + 30, G + k + 40, B + k + 30);
        }
    return vx_texture(tex_buf, 64, 64, 0);
}
static void build_under(void) {
    {   // the surface overhead, facing down
        const int ceil = vx_material(0xFFFF, VX_UNLIT, 255, tex_ceiling(&g_stage[s_stage]), 0);
        for (int gz = 0; gz < 6; gz++)
            for (int gx = 0; gx < 6; gx++) {
                const float x0 = -3600 + gx * 1200, z0 = -1200 + gz * 1100, x1 = x0 + 1200, z1 = z0 + 1100;
                const int a = mb_v(x0, SURF, z0, gx * 1024, gz * 1024), b = mb_v(x1, SURF, z0, gx * 1024 + 1024, gz * 1024);
                const int c = mb_v(x1, SURF, z1, gx * 1024 + 1024, gz * 1024 + 1024), d = mb_v(x0, SURF, z1, gx * 1024, gz * 1024 + 1024);
                mb_quad(a, b, c, d, ceil, (x0 + x1) / 2, SURF + 1000, (z0 + z1) / 2);
            }
        add_under(mb_commit(ceil, 1));
    }
    const int weedm = vx_material(0xFFFF, VX_GOURAUD, 255, tex_weed(), 0);
    const int weed0 = vx_prim(VX_BILLBOARD, 150, 300, 0, weedm, -1);
    vx_obj_pos(weed0, 0, -800, 0);
    add_under(weed0);
    const int stone = vx_material(C565(130, 136, 128), VX_GOURAUD, 255, -1, 0);
    const int bark = vx_material(C565(120, 96, 66), VX_GOURAUD, 255, -1, 0);
    for (int k = 0; k < NSPOTS; k++) {
        const Spot *s = &g_spot[k];
        if (s->kind == SPOT_WEEDS || s->kind == SPOT_PADS) {
            for (int i = 0; i < (s->kind == SPOT_WEEDS ? 10 : 6); i++) {
                const int t = vx_clone(weed0);
                if (t < 0) break;
                const float wx = s->x + rnd((int)s->r * 2) - s->r, wz = s->z + rnd((int)s->r * 2) - s->r;
                vx_obj_pos(t, iroundf(wx), s->kind == SPOT_PADS ? 200 : 150, iroundf(wz));
                weed_add(t, wx, wz);
            }
        }
        if (s->kind == SPOT_LOG) {                  // the same tree, seen from below: trunk + roots
            // A sunken trunk lying on the bed with a stump of a branch: cover, not a wall.
            mb_box(s->x - 170, 0, s->z - 22, s->x + 150, 44, s->z + 22, bark);
            mb_box(s->x + 40, 44, s->z - 10, s->x + 60, 120, s->z + 10, bark);
            mb_box(s->x - 190, 0, s->z - 40, s->x - 150, 70, s->z + 40, bark);
            add_under(mb_commit(bark, 0));
        }
        if (s->kind == SPOT_ROCKS) {
            for (int i = 0; i < 6; i++) {
                const float px = s->x + rnd(260) - 130, pz = s->z + rnd(260) - 130, q = 40 + rnd(60);
                mb_box(px - q, 0, pz - q, px + q, 50 + rnd(110), pz + q, stone);
            }
            add_under(mb_commit(stone, 0));
        }
    }
    // Scattered boulders and weed tufts on the open bed.
    for (int i = 0; i < 18; i++) {
        const float a = (rnd(1000) / 1000.0f - 0.5f) * 2.4f, d = 300 + rnd(1900), q = 20 + rnd(40);
        const float px = sinf_(a) * d, pz = cosf_(a) * d;
        mb_box(px - q, 0, pz - q, px + q, q * 1.2f, pz + q, stone);
    }
    add_under(mb_commit(stone, 0));
    for (int i = 0; i < 44; i++) {
        const float a = (rnd(1000) / 1000.0f - 0.5f) * 2.6f, d = 480 + rnd(1600);   // clear of the boat
        const int t = vx_clone(weed0);
        if (t < 0) break;
        vx_obj_pos(t, iroundf(sinf_(a) * d), 110, iroundf(cosf_(a) * d));
        vx_obj_scale(t, 60 + rnd(50));
        weed_add(t, sinf_(a) * d, cosf_(a) * d);
    }
}

void lake_build(int stage, int loop) {
    s_stage = stage;
    const Stage *st = &g_stage[stage];
    const int night = stage == 2;
    s_forest = st->forest;
    vx_reset();
    s_nabove = s_nunder = s_nweed = 0;
    rnd_seed(0x9E3779B9u * (uint32_t)(stage + 1) + 7919u * (uint32_t)loop);
    place_spots();
    s_tex_water = tex_water(st);
    s_tex_bed = tex_bed(st);
    s_tex_pano = tex_panorama(st, night);
    build_above(st, night);
    build_under();
    g_fx_splash = vx_emitter(96, C565(255, 255, 255), C565(170, 210, 240), 12, 4, 700, 700, 0);
    g_fx_bubble = vx_emitter(96, C565(220, 240, 255), C565(160, 210, 240), 5, 8, 1400, -160, VX_PART_ADDITIVE);
    g_fx_dust = vx_emitter(64, C565(170, 200, 190), C565(120, 150, 150), 4, 6, 2600, -12, 0);
}

void lake_view(int under) {
    const Stage *st = &g_stage[s_stage];
    for (int i = 0; i < s_nabove; i++) vx_obj_show(s_above[i], !under);
    for (int i = 0; i < s_nunder; i++) vx_obj_show(s_under[i], under);
    if (!under) {
        vx_sky(st->sky_top, st->sky_bot);
        vx_fog(3200, 9500);
        vx_sun(210, st->sun_el, st->sun_rgb, 230);
        vx_ambient(st->amb_rgb);
        vx_floor(0, s_tex_water, 520, st->water);
        vx_panorama(s_tex_pano, PANO_HR);
    } else {
        // Sunlight from the surface above, the deep fading into the stage's murk.
        vx_sky(mix16(st->water, C565(200, 240, 255), 110), st->deep);
        vx_fog(200, 1350);
        vx_sun(200, 70, 0xD8F4FF, 230);
        vx_ambient(0x5C8C9C);
        vx_floor(0, s_tex_bed, 420, st->deep);
        vx_panorama(-1, 0);
    }
}
