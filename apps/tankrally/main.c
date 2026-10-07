// Tank Rally - a small 3D arcade game on the Vertice engine. Drive a little tank round five arenas,
// shoot the crates (some hide coins and every one buys time), pick up all the coins before the clock
// runs out, chain pick-ups for a combo. From the second arena gun towers shoot back: three hits and
// the tank is gone. Each run lays the arenas out anew; the five best scores are kept with initials.
// Grown from sdk/templates/vertice-game (the engine kit: nv_math.h, vx_build.h).
#include "nucleo_sdk.h"
#include "nv_math.h"
#define VXB_MAXV 2048
#define VXB_MAXT 2048
#include "vx_build.h"

#define W 512
#define H 300
#define MAXC 20          // coins
#define MAXK 14          // crates
#define MAXS 112         // solids
#define NSHELL 4
#define MAXE 4           // gun towers
#define NESH 4           // their shells
#define HP_MAX 100
#define TOWER_HP 3
#define TOWER_Y 172        // the gun head sits on a pillar this high
#define NHI 5            // high-score table
#ifndef START_LV
#define START_LV 0       // the simulator starts later arenas with -DSTART_LV=n
#endif
#ifndef START_SCORE
#define START_SCORE 0
#endif
#define ARENA_R 3800.0f
#define SPAWN_Z (-ARENA_R * 0.6f)
#define TANK_R 50.0f

// ---- text ---------------------------------------------------------------------------------------
static int s_it;
static const char *T(const char *it, const char *en) { return s_it ? it : en; }

// ---- arenas ---------------------------------------------------------------------------------------
typedef struct {
    const char *it, *en;
    int sky_top, sky_bot, fog0, fog1, sun_el;
    int sun, amb;                       // rgb888
    int g0, g1;                         // ground colours (RGB565 via NV_RGB at build)
    int crown, trunk;                   // tree colours
    int coins, crates, rocks, trees, time_s;
} Arena;
static Arena k_arena[5];
static void arenas_init(void) {
    const Arena a[5] = {
        { "PRATO", "MEADOW", NV_RGB(80, 150, 240), NV_RGB(210, 230, 250), 2800, 6400, 55, 0xFFF4E0, 0x506070,
          NV_RGB(70, 140, 70), NV_RGB(58, 122, 60), NV_RGB(50, 140, 60), NV_RGB(120, 84, 50), 10, 7, 22, 30, 80 },
        { "CANYON", "CANYON", NV_RGB(90, 150, 220), NV_RGB(250, 210, 160), 2800, 6400, 45, 0xFFE0B0, 0x605040,
          NV_RGB(200, 130, 80), NV_RGB(180, 110, 70), NV_RGB(70, 140, 70), NV_RGB(90, 120, 60), 12, 8, 30, 16, 80 },
        { "TRAMONTO", "SUNSET", NV_RGB(80, 60, 140), NV_RGB(255, 160, 110), 2000, 5000, 12, 0xFFB070, 0x504060,
          NV_RGB(90, 110, 60), NV_RGB(76, 96, 52), NV_RGB(200, 90, 40), NV_RGB(90, 60, 40), 13, 9, 24, 32, 78 },
        { "NOTTE", "NIGHT", NV_RGB(8, 12, 40), NV_RGB(30, 50, 100), 900, 3200, 35, 0x8090FF, 0x202838,
          NV_RGB(30, 60, 50), NV_RGB(24, 50, 42), NV_RGB(30, 80, 50), NV_RGB(60, 50, 40), 14, 10, 24, 28, 78 },
        { "NEVE", "SNOW", NV_RGB(150, 180, 220), NV_RGB(235, 240, 250), 1400, 4200, 30, 0xFFFFFF, 0x707888,
          NV_RGB(235, 240, 248), NV_RGB(215, 222, 235), NV_RGB(240, 245, 250), NV_RGB(100, 80, 60), 16, 11, 28, 34, 76 },
    };
    for (int i = 0; i < 5; i++) k_arena[i] = a[i];
}

// ---- state ----------------------------------------------------------------------------------------
enum { ST_TITLE, ST_INTRO, ST_PLAY, ST_CLEAR, ST_OVER, ST_NAME };
static int s_state = ST_TITLE, s_state_at, s_level, s_score, s_time_ms, s_clock_at;
static float s_hp, s_hp_trail;                          // life, and the white "just lost" part of the bar
static int s_hurt_at, s_bump_at, s_dead;                  // s_dead: the tank blew up (else the clock ran out)
// high scores: five rows, initials, the arena reached
typedef struct { int score, arena; char name[4]; } Hi;
static Hi s_hi[NHI];
static int s_hi_new = -1;                               // the row just entered (highlighted)
static char s_name[4] = "AAA";
static int s_name_pos;
static const char k_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ";
static int s_combo, s_combo_at, s_bonus_show, s_bonus_at, s_msg_at;
static const char *s_msg;
static NvRand s_rnd;

// tank
static int s_hull, s_turret, s_barrel;
static float s_x, s_z, s_yaw, s_speed, s_tyaw, s_recoil, s_fire_cd, s_shake;
static int s_manual_until;                              // turret turned by hand: no auto-aim till then
// world
static float s_sx[MAXS], s_sz[MAXS], s_sr[MAXS];
static int s_nsolid;
static int s_coin[MAXC], s_ncoin, s_coin_on[MAXC], s_coin_hidden[MAXC];
static float s_cx[MAXC], s_cz[MAXC];
static int s_crate[MAXK], s_ncrate, s_crate_on[MAXK], s_crate_solid[MAXK];
static int s_kit[MAXK], s_kit_on[MAXK];                 // repair kits in crates: 1 hidden, 2 out (same spot)
static float s_kx[MAXK], s_kz[MAXK];
static int s_shell[NSHELL];
// gun towers: a base, a head that turns (with its barrel), two hits to knock out
static int s_ebase[MAXE], s_ehead[MAXE], s_ne, s_ehp[MAXE];
static float s_ex[MAXE], s_ez[MAXE], s_eyaw[MAXE], s_epitch[MAXE], s_ecd[MAXE];
static int s_esh[NESH];
static float s_eshx[NESH], s_eshy[NESH], s_eshz[NESH], s_eshvx[NESH], s_eshvy[NESH], s_eshvz[NESH], s_eshlife[NESH];
static float s_shx[NSHELL], s_shz[NSHELL], s_shvx[NSHELL], s_shvz[NSHELL], s_shlife[NSHELL];
static int s_fx_dust, s_fx_spark, s_fx_boom, s_fx_smoke, s_fx_glint;
static NvVec3 s_eye, s_at;

static void solid(float x, float z, float r) {
    if (s_nsolid < MAXS) { s_sx[s_nsolid] = x; s_sz[s_nsolid] = z; s_sr[s_nsolid] = r; s_nsolid++; }
}
static int collide(float *x, float *z, float r) {
    int hit = 0;
    for (int i = 0; i < s_nsolid; i++) {
        if (s_sr[i] <= 0) continue;
        const float dx = *x - s_sx[i], dz = *z - s_sz[i], m = r + s_sr[i], d2 = dx * dx + dz * dz;
        if (d2 >= m * m || d2 < 1e-6f) continue;
        const float d = nv_sqrtf(d2), k = (m - d) / d;
        *x += dx * k; *z += dz * k; hit = 1;
    }
    const float d = nv_sqrtf(*x * *x + *z * *z);              // the arena's fence
    if (d > ARENA_R - r) { const float k = (ARENA_R - r) / d; *x *= k; *z *= k; hit = 1; }
    return hit;
}
// a free spot: away from the start, the solids and the fence
static int free_spot(float *x, float *z, float r) {
    for (int t = 0; t < 40; t++) {
        const float a = nv_rand_float(&s_rnd) * 2 * NV_PI, d = nv_rand_range(&s_rnd, 400, ARENA_R - 250);
        float px = nv_sinf(a) * d, pz = nv_cosf(a) * d;
        int ok = 1;
        for (int i = 0; i < s_nsolid && ok; i++) {
            const float dx = px - s_sx[i], dz = pz - s_sz[i], m = r + s_sr[i] + 60;
            if (dx * dx + dz * dz < m * m) ok = 0;
        }
        if (ok) { *x = px; *z = pz; return 1; }
    }
    return 0;
}

// ---- building an arena -------------------------------------------------------------------------
static int ground_texture(const Arena *A) {
    static uint16_t px[64 * 64];
    NvRand r; nv_rand_seed(&r, 11);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            const int n = nv_rand_int(&r, 40);
            const int big = ((x >> 4) + (y >> 4)) & 1;
            px[y * 64 + x] = nv_mix565((uint16_t)(big ? A->g0 : A->g1), n < 6 ? 0 : 0xFFFF, n < 6 ? 30 : n > 36 ? 18 : 0);
        }
    const int t = vx_texture_new(64, 64, 0, 0);
    vx_texture_write(t, 0, 0, 64, 64, px);
    return t;
}
static void build_tank(void) {
    const int olive = vx_material(NV_RGB(96, 112, 56), VX_GOURAUD, 255, -1, 40);
    const int olive2 = vx_material(NV_RGB(70, 84, 40), VX_GOURAUD, 255, -1, 20);
    const int track = vx_material(NV_RGB(34, 34, 38), VX_GOURAUD, 255, -1, 0);
    const int wheel = vx_material(NV_RGB(60, 62, 66), VX_GOURAUD, 255, -1, 60);
    const int star = vx_material(NV_RGB(230, 230, 220), VX_FLAT, 255, -1, 0);
    // hull: tracks with road wheels, the body between them, a sloped front plate, a white star
    for (int side = -1; side <= 1; side += 2) {
        vxb_box(side * 50 - 10, 2, -62, side * 50 + 10, 24, 62, track, 0);
        for (int w = 0; w < 5; w++) {
            const float z = -48 + w * 24;
            vxb_limb(side * 61, 12, z, 9, side * 63, 12, z, 9, 8, VXB_CAP_B, wheel, 60);
        }
    }
    vxb_box(-40, 14, -56, 40, 36, 50, olive, 0);
    {   // the glacis: a slope from the deck down to the nose
        const int a = vxb_v(-40, 36, 50, 0, 0), b = vxb_v(40, 36, 50, 0, 0), c = vxb_v(40, 18, 66, 0, 0), d = vxb_v(-40, 18, 66, 0, 0);
        vxb_quad(a, b, c, d, olive2, 0, 0, 0);
        vxb_tri(b, c, vxb_v(40, 18, 50, 0, 0), olive2, 0, 26, 50);
        vxb_tri(a, d, vxb_v(-40, 18, 50, 0, 0), olive2, 0, 26, 50);
    }
    vxb_box(-8, 36.2f, -20, 8, 37, -4, star, 0);
    s_hull = vxb_commit(olive, 0, 0);
    vx_obj_shadow(s_hull, 82, 1, 120);
    // turret on the hull, the barrel on the turret (each its own object: it turns, it recoils)
    vxb_box(-24, 0, -26, 24, 20, 22, olive, 0);
    vxb_limb(10, 20, -8, 8, 10, 27, -8, 6, 8, VXB_CAP_B, olive2, 60);   // the commander's hatch
    s_turret = vxb_commit(olive, 0, 0);
    vx_obj_parent(s_turret, s_hull);
    vx_obj_pos(s_turret, 0, 36, -6);
    vxb_limb(0, 0, 0, 6, 0, 0, 70, 4.5f, 8, VXB_CAP_B, olive2, 60);
    vxb_limb(0, 0, 66, 6, 0, 0, 78, 6, 8, VXB_CAP_B, track, 60);       // muzzle brake
    s_barrel = vxb_commit(olive2, 0, 0);
    vx_obj_parent(s_barrel, s_turret);
    vx_obj_pos(s_barrel, 0, 10, 20);
}
static void build_arena(int lv) {
    const Arena *A = &k_arena[lv % 5];
    vx_reset();
    s_nsolid = 0; s_ncoin = 0; s_ncrate = 0;
    solid(0, SPAWN_Z, 380);                            // solid 0: keeps the start clear while laying out
    vx_sky(A->sky_top, A->sky_bot);
    vx_fog(A->fog0, A->fog1);
    vx_sun(210, A->sun_el, A->sun, 230);
    vx_ambient(A->amb);
    vx_floor(0, ground_texture(A), 360, A->g0);
    // particles
    s_fx_dust = vx_emitter(80, A->g1, A->sky_bot, 10, 30, 700, -40, 0);
    s_fx_spark = vx_emitter(64, NV_RGB(255, 240, 140), NV_RGB(255, 110, 20), 9, 2, 500, -300, VX_PART_ADDITIVE);
    s_fx_boom = vx_emitter(96, NV_RGB(255, 220, 120), NV_RGB(200, 60, 10), 26, 6, 650, -120, VX_PART_ADDITIVE);
    s_fx_smoke = vx_emitter(64, NV_RGB(90, 90, 96), NV_RGB(170, 170, 176), 18, 46, 1300, 40, 0);
    s_fx_glint = vx_emitter(48, NV_RGB(255, 250, 200), NV_RGB(255, 200, 60), 6, 1, 600, -60, VX_PART_ADDITIVE);
    // the fence: posts round the arena (one mesh per quarter: culled when behind you)
    const int wood = vx_material(NV_RGB(140, 100, 60), VX_GOURAUD, 255, -1, 0);
    for (int q = 0; q < 8; q++) {
        for (int i = 0; i < 12; i++) {
            const float a = (q * 12 + i) * (2 * NV_PI / 96), x = nv_sinf(a) * ARENA_R, z = nv_cosf(a) * ARENA_R;
            vxb_box(x - 8, 0, z - 8, x + 8, 70, z + 8, wood, 0);
            const float a2 = a + 2 * NV_PI / 96, x2 = nv_sinf(a2) * ARENA_R, z2 = nv_cosf(a2) * ARENA_R;
            vxb_limb(x, 50, z, 4, x2, 50, z2, 4, 4, 0, wood, 120);
            vxb_limb(x, 24, z, 4, x2, 24, z2, 4, 4, 0, wood, 120);
        }
        vxb_commit(wood, 0, 0);
    }
    // rocks
    const int stone = vx_material(lv % 5 == 4 ? NV_RGB(170, 175, 185) : NV_RGB(140, 132, 128), VX_GOURAUD, 255, -1, 0);
    for (int i = 0; i < A->rocks; i++) {
        float x, z;
        const float r = nv_rand_range(&s_rnd, 40, 95);
        if (!free_spot(&x, &z, r)) continue;
        vxb_rock(&s_rnd, x, z, r, nv_rand_range(&s_rnd, 40, 110), stone, 120);
        solid(x, z, r * 0.9f);
        if (vxb_room_t() < 40) vxb_commit(stone, 0, 0);
    }
    vxb_commit(stone, 0, 0);
    // trees (canyon: cacti)
    const int crown = vx_material(A->crown, VX_GOURAUD, 255, -1, 0), trunk = vx_material(A->trunk, VX_GOURAUD, 255, -1, 0);
    for (int i = 0; i < A->trees; i++) {
        float x, z;
        if (!free_spot(&x, &z, 40)) continue;
        const float h = nv_rand_range(&s_rnd, 140, 240);
        if (lv % 5 == 1) {
            vxb_limb(x, 0, z, 16, x, h, z, 13, 8, VXB_CAP_B, crown, 120);
            vxb_limb(x, h * 0.45f, z, 9, x + 42, h * 0.55f, z, 8, 6, 0, crown, 120);
            vxb_limb(x + 42, h * 0.55f, z, 8, x + 42, h * 0.8f, z, 7, 6, VXB_CAP_B, crown, 120);
        } else {
            static const float cr[5] = { 8, 64, 72, 44, 3 }, cy[5] = { 0, 26, 80, 136, 176 };
            float ry[5];
            for (int k = 0; k < 5; k++) ry[k] = h - 40 + cy[k];
            vxb_limb(x, 0, z, 14, x, h, z, 9, 6, 0, trunk, 120);
            vxb_lathe(x, z, cr, ry, 5, 8, crown, 200);
        }
        solid(x, z, 20);
        if (vxb_room_t() < 120) vxb_commit(crown, 0, VX_MESH_SMOOTH);
    }
    vxb_commit(crown, 0, VX_MESH_SMOOTH);
    // crates (shoot them: some hide a coin, each buys time)
    const int plank = vx_material(NV_RGB(176, 128, 70), VX_GOURAUD, 255, -1, 0), band = vx_material(NV_RGB(110, 76, 40), VX_GOURAUD, 255, -1, 0);
    vxb_box(-34, 0, -34, 34, 64, 34, plank, 0);
    vxb_box(-36, 28, -36, 36, 36, 36, band, 0);
    vxb_box(-36, 60, -36, 36, 66, 36, band, 0);
    const int crate0 = vxb_commit(plank, 0, 0);
    // coins
    const int gold = vx_material(NV_RGB(255, 200, 40), VX_GOURAUD, 255, -1, 220);
    vxb_limb(-6, 0, 0, 26, 6, 0, 0, 26, 12, VXB_CAP_A | VXB_CAP_B, gold, 100);
    const int coin0 = vxb_commit(gold, 0, VX_MESH_SMOOTH);
    const int ncr = A->crates + lv / 5 > MAXK ? MAXK : A->crates + lv / 5;
    for (int i = 0; i < ncr; i++) {
        float x, z;
        if (!free_spot(&x, &z, 50)) continue;
        const int id = i ? vx_clone(crate0) : crate0;
        vx_obj_pos(id, nv_roundi(x), 0, nv_roundi(z));
        vx_obj_rot(id, 0, nv_rand_int(&s_rnd, 90), 0);
        s_crate[s_ncrate] = id; s_kx[s_ncrate] = x; s_kz[s_ncrate] = z; s_crate_on[s_ncrate] = 1;
        s_crate_solid[s_ncrate] = s_nsolid; solid(x, z, 46);
        s_ncrate++;
    }
    const int nco = A->coins + lv / 5 > MAXC ? MAXC : A->coins + lv / 5;
    for (int i = 0; i < nco; i++) {
        float x, z;
        int hidden = i < s_ncrate / 2;                     // the first few sit inside crates
        if (hidden) { x = s_kx[i]; z = s_kz[i]; }
        else if (!free_spot(&x, &z, 40)) continue;
        const int id = s_ncoin ? vx_clone(coin0) : coin0;
        vx_obj_pos(id, nv_roundi(x), 46, nv_roundi(z));
        vx_obj_show(id, !hidden);
        s_coin[s_ncoin] = id; s_cx[s_ncoin] = x; s_cz[s_ncoin] = z; s_coin_on[s_ncoin] = 1; s_coin_hidden[s_ncoin] = hidden;
        s_ncoin++;
    }
    if (!s_ncoin) vx_obj_show(coin0, 0);
    // repair kits: some of the crates without a coin hold one
    const int kitg = vx_material(NV_RGB(60, 170, 80), VX_GOURAUD, 255, -1, 80), white = vx_material(NV_RGB(250, 250, 250), VX_FLAT, 255, -1, 0);
    vxb_box(-22, 0, -16, 22, 26, 16, kitg, 0);
    vxb_box(-16, 26, -4, 16, 27.5f, 4, white, 0);                              // the white cross, top
    vxb_box(-4, 26, -12, 4, 27.5f, 12, white, 0);
    vxb_box(-12, 9, -17, 12, 17, -16, white, 0);                               // and on both long sides
    vxb_box(-4, 3, -17, 4, 23, -16, white, 0);
    vxb_box(-12, 9, 16, 12, 17, 17, white, 0);
    vxb_box(-4, 3, 16, 4, 23, 17, white, 0);
    vxb_box(-8, 27, -2, 8, 32, 2, white, 0);                                   // the handle
    const int kit0 = vxb_commit(kitg, 0, 0);
    int nkit = 0;
    for (int c = 0; c < s_ncrate; c++) {
        s_kit_on[c] = 0;
        if (c < s_ncrate / 2 || nv_rand_float(&s_rnd) > 0.5f) continue;
        s_kit[c] = nkit++ ? vx_clone(kit0) : kit0;
        vx_obj_pos(s_kit[c], nv_roundi(s_kx[c]), 0, nv_roundi(s_kz[c]));
        vx_obj_show(s_kit[c], 0);
        s_kit_on[c] = 1;
    }
    if (!nkit) vx_obj_show(kit0, 0);
    // gun towers: none in the first arena, then one more each arena (up to four)
    s_ne = 0;
    const int nen = lv < MAXE ? lv : MAXE;
    if (nen > 0) {
        const int conc = vx_material(NV_RGB(120, 120, 128), VX_GOURAUD, 255, -1, 20);
        const int dark = vx_material(NV_RGB(64, 66, 72), VX_GOURAUD, 255, -1, 60);
        const int red = vx_material(NV_RGB(200, 40, 30), VX_FLAT, 255, -1, 0);
        static const float br[8] = { 58, 56, 34, 30, 30, 46, 46, 2 }, by[8] = { 0, 20, 44, 130, 150, 158, 170, 172 };
        vxb_lathe(0, 0, br, by, 8, 10, conc, 120);
        vxb_box(-48, 150, -4, 48, 156, 4, red, 0);                                 // a red ring band
        vxb_box(-4, 150, -48, 4, 156, 48, red, 0);
        const int base0 = vxb_commit(conc, 0, VX_MESH_SMOOTH);
        vxb_box(-22, 0, -22, 22, 24, 20, dark, 0);
        vxb_box(-23, 18, -23, 23, 22, 21, red, 0);                                 // the red band: a target
        vxb_limb(-7, 12, 16, 5, -7, 12, 70, 4, 6, VXB_CAP_B, dark, 60);            // twin barrels
        vxb_limb(7, 12, 16, 5, 7, 12, 70, 4, 6, VXB_CAP_B, dark, 60);
        const int head0 = vxb_commit(dark, 0, 0);
        for (int i = 0; i < nen; i++) {
            float x = 0, z = 0;
            int ok = 0;
            for (int t = 0; t < 12 && !ok; t++)                                     // not too near the start
                ok = free_spot(&x, &z, 70) && (x * x + (z - SPAWN_Z) * (z - SPAWN_Z)) > 1700.0f * 1700.0f;
            if (!ok) continue;
            const int b = s_ne ? vx_clone(base0) : base0, h = s_ne ? vx_clone(head0) : head0;
            vx_obj_pos(b, nv_roundi(x), 0, nv_roundi(z));
            vx_obj_pos(h, nv_roundi(x), TOWER_Y, nv_roundi(z));
            vx_obj_shadow(b, 70, 1, 110);
            s_ebase[s_ne] = b; s_ehead[s_ne] = h; s_ex[s_ne] = x; s_ez[s_ne] = z; s_ehp[s_ne] = TOWER_HP;
            s_eyaw[s_ne] = nv_rand_float(&s_rnd) * 6.28f; s_ecd[s_ne] = 2.0f + nv_rand_float(&s_rnd);
            solid(x, z, 56);
            s_ne++;
        }
        if (!s_ne) { vx_obj_show(base0, 0); vx_obj_show(head0, 0); }
    }
    // shells
    const int shellm = vx_material(NV_RGB(255, 230, 150), VX_UNLIT, 255, -1, 0);
    vxb_limb(0, 0, -10, 4, 0, 0, 10, 2, 6, VXB_CAP_A | VXB_CAP_B, shellm, 60);
    const int sh0 = vxb_commit(shellm, 0, 0);
    for (int i = 0; i < NSHELL; i++) { s_shell[i] = i ? vx_clone(sh0) : sh0; s_shlife[i] = 0; vx_obj_show(s_shell[i], 0); }
    const int eshm = vx_material(NV_RGB(255, 80, 50), VX_UNLIT, 255, -1, 0);
    vxb_limb(0, 0, -8, 6, 0, 0, 8, 6, 6, VXB_CAP_A | VXB_CAP_B, eshm, 60);
    const int esh0 = vxb_commit(eshm, 0, 0);
    for (int i = 0; i < NESH; i++) { s_esh[i] = i ? vx_clone(esh0) : esh0; s_eshlife[i] = 0; vx_obj_show(s_esh[i], 0); }
    s_sr[0] = 0;                                       // the start's placeholder is not a wall
    build_tank();
}

// ---- input --------------------------------------------------------------------------------------
// On-screen buttons (touch): steer left/right bottom-left; FIRE, GAS and BACK bottom-right. Lifting
// the thumb from GAS to FIRE keeps the throttle, so you can shoot on the move with one thumb.
enum { B_LEFT = 1, B_RIGHT = 2, B_GAS = 4, B_BACK = 8, B_FIRE = 16 };
typedef struct { int x, y, r; } Btn;
static const Btn k_btn[5] = {
    { 52, H - 50, 38 }, { 140, H - 50, 38 },          // left, right
    { W - 52, H - 52, 42 }, { W - 52, H - 140, 30 },  // gas, back
    { W - 146, H - 48, 36 },                          // fire
};
static int btn_at(int x, int y) {
    for (int i = 0; i < 5; i++) {
        const int dx = x - k_btn[i].x, dy = y - k_btn[i].y, r = k_btn[i].r + 14;   // generous: thumbs miss
        if (dx * dx + dy * dy < r * r) return 1 << i;
    }
    if (y > H - 110 && x < 96) return B_LEFT;          // the whole corner steers
    if (y > H - 110 && x < 200) return B_RIGHT;
    return 0;
}
// Controllers: a gamepad drives with the left stick (or d-pad), RT gas, LT/B reverse, A/X/RB fire,
// the right stick turns the turret. A keyboard: WASD or arrows, Space/Ctrl fire, Q/E turret. A mouse
// aims the turret (left button fires, right button is gas). Turning the turret by hand switches the
// auto-aim off for a few seconds.
typedef struct {
    float steer, gas, aim, aim_rad;                    // aim: turret turn rate -1..1; aim_rad: mouse turn
    int fire, go, btn, tap, tx, ty, up, down, left, right;
    char ch;                                           // a letter typed on the keyboard (initials)
    int bksp;
} In;
enum { DEV_TOUCH, DEV_PAD, DEV_KEYS };
static int s_dev = DEV_TOUCH;                          // what the hints and on-screen buttons show
static int s_touch_was, s_pad_was;
static uint8_t s_keys_was[8];
static float s_latch;
static float axis(int v, int dead) {                   // -32768..32767 -> -1..1 with a dead zone
    if (v > -dead && v < dead) return 0;
    const float f = (v > 0 ? v - dead : v + dead) / (32767.0f - dead);
    return nv_clampf(f, -1, 1);
}
static int held(const uint8_t *k, int n, int u) { for (int i = 1; i <= n; i++) if (k[i] == u) return 1; return 0; }
static void rumble(int lo, int hi, int ms) { if (nv_pad_count() > 0) nv_pad_rumble(0, lo, hi, ms); }
static In read_input(void) {
    In in;
    __builtin_memset(&in, 0, sizeof in);
    const int pad = nv_gfx_pad();
    // the merged SNES-style pad: menus, and driving when nothing richer is there
    in.steer = (pad & NV_PAD_RIGHT ? 1.0f : 0.0f) - (pad & NV_PAD_LEFT ? 1.0f : 0.0f);
    in.gas = (pad & (NV_PAD_UP | NV_PAD_A) ? 1.0f : 0.0f) - (pad & NV_PAD_DOWN ? 0.6f : 0.0f);
    in.fire = (pad & (NV_PAD_B | NV_PAD_X | NV_PAD_Y)) != 0;
    in.go = (pad & (NV_PAD_A | NV_PAD_START)) != 0;
    // a gamepad: analog steering and throttle
    nv_pad_state_t ps;
    if (nv_pad_count() > 0 && nv_pad_state(0, &ps, sizeof ps) == (int)sizeof ps) {
        const uint32_t b = ps.buttons;
        float st = axis(ps.lx, 7000);
        if (b & NV_PADB_LEFT) st = -1;
        if (b & NV_PADB_RIGHT) st = 1;
        float gas = ps.rt / 32767.0f - 0.6f * (ps.lt / 32767.0f);
        if (b & NV_PADB_B) gas = -0.6f;
        if (b & NV_PADB_UP) gas = 1;
        if (b & NV_PADB_DOWN) gas = -0.6f;
        if (gas == 0) { const float y = -axis(ps.ly, 9000); gas = y > 0 ? y : y * 0.6f; }   // pads without triggers
        in.steer = st; in.gas = nv_clampf(gas, -0.6f, 1);
        in.fire = (b & (NV_PADB_A | NV_PADB_X | NV_PADB_RB)) != 0;
        in.aim = axis(ps.rx, 9000);
        in.go = (b & (NV_PADB_A | NV_PADB_START)) != 0;
        if (b || st != 0 || gas != 0 || in.aim != 0) s_dev = DEV_PAD;
    }
    // a keyboard: real keys, so Space fires (on the merged pad it is A = gas)
    uint8_t k[8];
    const int nk = nv_kbd_state(k, 7);
    if (nk >= 0) {
        if (nk > 0 || k[0]) s_dev = DEV_KEYS;
        if (nk > 0 || k[0] || s_dev == DEV_KEYS) {
            const float st = (held(k, nk, 0x07) || held(k, nk, 0x4F) ? 1.0f : 0.0f) - (held(k, nk, 0x04) || held(k, nk, 0x50) ? 1.0f : 0.0f);
            const float gas = (held(k, nk, 0x1A) || held(k, nk, 0x52) ? 1.0f : 0.0f) - (held(k, nk, 0x16) || held(k, nk, 0x51) ? 0.6f : 0.0f);
            if (s_dev == DEV_KEYS) { in.steer = st; in.gas = gas; in.fire = 0; }
            if (held(k, nk, 0x2C) || (k[0] & 0x11)) in.fire = 1;                // Space, Ctrl
            in.aim += (held(k, nk, 0x08) ? 1.0f : 0.0f) - (held(k, nk, 0x14) ? 1.0f : 0.0f);   // E, Q
        }
        for (int i = 1; i <= nk; i++) {                                         // new key presses: typing
            if (held(s_keys_was, s_keys_was[7], k[i])) continue;
            const int u = k[i];
            if (u >= 0x04 && u <= 0x1D) in.ch = (char)('A' + u - 0x04);
            else if (u >= 0x1E && u <= 0x26) in.ch = (char)('1' + u - 0x1E);
            else if (u == 0x27) in.ch = '0';
            else if (u == 0x2A) in.bksp = 1;
            else if (u == 0x28 || u == 0x2C) in.go = 1;
        }
        __builtin_memcpy(s_keys_was, k, 7);
        s_keys_was[7] = (uint8_t)(nk > 0 ? nk : 0);
    }
    // a mouse: sideways motion turns the turret, left button fires, right button is gas
    nv_mouse_t m;
    if (nv_mouse_read(&m, sizeof m) == 1) {
        if (m.dx) { in.aim_rad = m.dx * 0.0045f; s_dev = DEV_KEYS; }
        if (m.buttons & NV_MOUSE_LEFT) { in.fire = 1; in.go = 1; }
        if (m.buttons & NV_MOUSE_RIGHT) in.gas = 1;
    }
    const int edge = pad & ~s_pad_was;
    s_pad_was = pad;
    in.up = (edge & NV_PAD_UP) != 0; in.down = (edge & NV_PAD_DOWN) != 0;
    in.left = (edge & NV_PAD_LEFT) != 0; in.right = (edge & NV_PAD_RIGHT) != 0;
    const int n = nv_touch_count();
    if (n) s_dev = DEV_TOUCH;
    for (int i = 0; i < n; i++) {
        int x, y;
        if (!nv_touch_at(i, &x, &y)) continue;
        in.go = 1;
        if (i == 0 && !s_touch_was) { in.tap = 1; in.tx = x; in.ty = y; }
        in.btn |= btn_at(x, y);
    }
    s_touch_was = n > 0;
    if (in.btn & B_LEFT) in.steer -= 1;
    if (in.btn & B_RIGHT) in.steer += 1;
    if (in.btn & (B_GAS | B_BACK)) { in.gas = in.btn & B_GAS ? 1.0f : -0.6f; s_latch = in.gas; }
    else if (in.btn & B_FIRE) in.gas = s_latch;
    else if (n) s_latch = 0;
    if (in.btn & B_FIRE) in.fire = 1;
    if (!n && !(pad & (NV_PAD_UP | NV_PAD_DOWN | NV_PAD_A))) s_latch = 0;
    return in;
}
static int touch_ui(void) { return s_dev == DEV_TOUCH; }

// ---- high scores --------------------------------------------------------------------------------
static void hi_load(void) {
    if (nv_load("hiscore.bin", s_hi, sizeof s_hi) == (int)sizeof s_hi && s_hi[0].score >= 0) return;
    static const char *const def[NHI] = { "TNK", "VTX", "NUC", "RLY", "BOB" };
    for (int i = 0; i < NHI; i++) {                    // something to beat on a fresh install
        s_hi[i].score = 5000 - i * 1000; s_hi[i].arena = 5 - i;
        __builtin_memcpy(s_hi[i].name, def[i], 4);
    }
    char nm[4];
    if (nv_load("name.bin", nm, 4) == 4 && nm[3] == 0) __builtin_memcpy(s_name, nm, 4);
}
static int hi_rank(int score) {                        // where this score would go, NHI if nowhere
    for (int i = 0; i < NHI; i++) if (score > s_hi[i].score) return i;
    return NHI;
}
static void hi_insert(void) {
    const int r = hi_rank(s_score);
    if (r >= NHI) return;
    for (int i = NHI - 1; i > r; i--) s_hi[i] = s_hi[i - 1];
    s_hi[r].score = s_score; s_hi[r].arena = s_level + 1;
    __builtin_memcpy(s_hi[r].name, s_name, 4);
    s_hi_new = r;
    nv_save("hiscore.bin", s_hi, sizeof s_hi);
    nv_save("name.bin", s_name, 4);
}

// ---- play ---------------------------------------------------------------------------------------
static void say(const char *m) { s_msg = m; s_msg_at = nv_millis(); }
static void fire(void) {
    int k = 0;
    while (k < NSHELL && s_shlife[k] > 0) k++;
    if (k >= NSHELL) return;
    const float a = s_yaw + s_tyaw, dx = nv_sinf(a), dz = nv_cosf(a);
    const float tx = s_x + nv_sinf(s_yaw) * -6, tz = s_z + nv_cosf(s_yaw) * -6;      // the turret's centre
    s_shx[k] = tx + dx * 100; s_shz[k] = tz + dz * 100;
    s_shvx[k] = dx * 1500; s_shvz[k] = dz * 1500; s_shlife[k] = 1.0f;
    vx_obj_show(s_shell[k], 1);
    vx_emit(s_fx_spark, nv_roundi(s_shx[k]), 46, nv_roundi(s_shz[k]), nv_roundi(dx * 200), 40, nv_roundi(dz * 200), 90, 10);
    vx_emit(s_fx_smoke, nv_roundi(s_shx[k]), 46, nv_roundi(s_shz[k]), 0, 30, 0, 30, 4);
    s_recoil = 1.0f; s_shake = 6; s_fire_cd = 0.55f;
    rumble(12000, 20000, 90);
    s_speed -= 60;
    nv_gfx_tone(140, 70);
}
static void add_time(int s) { s_time_ms += s * 1000; s_bonus_show = s; s_bonus_at = nv_millis(); }
static void crate_hit(int c) {
    s_crate_on[c] = 0;
    vx_obj_show(s_crate[c], 0);
    s_sr[s_crate_solid[c]] = 0;
    vx_emit(s_fx_boom, nv_roundi(s_kx[c]), 40, nv_roundi(s_kz[c]), 0, 160, 0, 220, 40);
    vx_emit(s_fx_smoke, nv_roundi(s_kx[c]), 40, nv_roundi(s_kz[c]), 0, 60, 0, 90, 14);
    s_shake = 10; s_score += 50; add_time(3);
    rumble(20000, 30000, 150);
    nv_gfx_tone(90, 160);
    if (s_kit_on[c] == 1) {                            // a repair kit falls out
        s_kit_on[c] = 2; vx_obj_show(s_kit[c], 1);
        vx_emit(s_fx_glint, nv_roundi(s_kx[c]), 40, nv_roundi(s_kz[c]), 0, 100, 0, 80, 12);
    }
    for (int i = 0; i < s_ncoin; i++)
        if (s_coin_hidden[i] && s_coin_on[i] && nv_absf(s_cx[i] - s_kx[c]) < 1 && nv_absf(s_cz[i] - s_kz[c]) < 1) {
            s_coin_hidden[i] = 0; vx_obj_show(s_coin[i], 1);
            vx_emit(s_fx_glint, nv_roundi(s_cx[i]), 60, nv_roundi(s_cz[i]), 0, 120, 0, 80, 16);
        }
}
static int coins_left(void) { int n = 0; for (int i = 0; i < s_ncoin; i++) n += s_coin_on[i]; return n; }
static int nearest(int coins, float *bx, float *bz) {
    int best = -1; float bd = 1e12f;
    for (int i = 0; i < (coins ? s_ncoin : s_ncrate); i++) {
        if (coins ? (!s_coin_on[i] || s_coin_hidden[i]) : !s_crate_on[i]) continue;
        const float x = coins ? s_cx[i] : s_kx[i], z = coins ? s_cz[i] : s_kz[i];
        const float dx = x - s_x, dz = z - s_z, d = dx * dx + dz * dz;
        if (d < bd) { bd = d; best = i; *bx = x; *bz = z; }
    }
    return best;
}
static void game_over(int now) {
    s_state = ST_OVER; s_state_at = now; s_hi_new = -1;
    nv_gfx_tone(196, 260); nv_gfx_tone(131, 500);
}
static void tank_show(int on) { vx_obj_show(s_hull, on); vx_obj_show(s_turret, on); vx_obj_show(s_barrel, on); }
static void tower_hit(int e) {
    vx_emit(s_fx_spark, nv_roundi(s_ex[e]), 60, nv_roundi(s_ez[e]), 0, 140, 0, 140, 16);
    if (--s_ehp[e] > 0) { nv_gfx_tone(330, 60); return; }
    vx_obj_show(s_ehead[e], 0);                        // the head blows off, the base stays (still solid)
    vx_emit(s_fx_boom, nv_roundi(s_ex[e]), 50, nv_roundi(s_ez[e]), 0, 180, 0, 240, 44);
    vx_emit(s_fx_smoke, nv_roundi(s_ex[e]), 50, nv_roundi(s_ez[e]), 0, 70, 0, 90, 16);
    s_shake = 12; s_score += 150; add_time(4);
    say(T("TORRETTA DISTRUTTA!", "TOWER DOWN!"));
    nv_gfx_tone(80, 220);
}
static void tank_hit(int now, float dmg) {
    if (now - s_hurt_at < 1100) return;                // a moment's grace after a hit
    s_hurt_at = now; s_hp -= dmg; s_shake = 14; s_speed *= 0.3f;
    vx_emit(s_fx_spark, nv_roundi(s_x), 50, nv_roundi(s_z), 0, 160, 0, 160, 18);
    vx_emit(s_fx_smoke, nv_roundi(s_x), 50, nv_roundi(s_z), 0, 50, 0, 60, 8);
    nv_gfx_tone(100, 180);
    rumble(40000, 50000, 300);
    if (s_hp <= 0) {
        s_hp = 0;
        tank_show(0);
        vx_emit(s_fx_boom, nv_roundi(s_x), 40, nv_roundi(s_z), 0, 220, 0, 300, 60);
        vx_emit(s_fx_smoke, nv_roundi(s_x), 40, nv_roundi(s_z), 0, 80, 0, 120, 24);
        s_dead = 1;
        game_over(now);
    } else say(s_hp < 30 ? T("DANNI GRAVI!", "HEAVY DAMAGE!") : T("COLPITO!", "HIT!"));
}
static void towers_update(float dt, int now) {
    const float reload = nv_clampf(2.4f - s_level * 0.12f, 1.2f, 2.4f);
    for (int e = 0; e < s_ne; e++) {
        if (s_ehp[e] <= 0) continue;
        const float dx = s_x - s_ex[e], dz = s_z - s_ez[e], d2 = dx * dx + dz * dz;
        s_ecd[e] -= dt;
        if (d2 < 1400.0f * 1400.0f) {                  // in range: turn to the tank, fire when lined up
            const float a = nv_angle_diff(s_eyaw[e], nv_atan2f(dx, dz)), d = nv_sqrtf(d2);
            s_eyaw[e] += nv_clampf(a, -1.3f * dt, 1.3f * dt);
            s_epitch[e] = nv_deg(nv_atan2f(TOWER_Y - 30, d));                    // the barrels dip at you
            if (nv_absf(a) < 0.1f && s_ecd[e] <= 0) {
                int k = 0;
                while (k < NESH && s_eshlife[k] > 0) k++;
                if (k < NESH) {
                    const float fx = nv_sinf(s_eyaw[e]), fz = nv_cosf(s_eyaw[e]);
                    s_eshx[k] = s_ex[e] + fx * 80; s_eshz[k] = s_ez[e] + fz * 80; s_eshy[k] = TOWER_Y + 8;
                    s_eshvx[k] = fx * 640; s_eshvz[k] = fz * 640; s_eshlife[k] = 2.6f;   // slow: you can dodge
                    s_eshvy[k] = -(TOWER_Y - 30) / (d / 640.0f + 0.05f);               // it lands where you were
                    vx_obj_show(s_esh[k], 1);
                    vx_emit(s_fx_spark, nv_roundi(s_eshx[k]), TOWER_Y + 10, nv_roundi(s_eshz[k]), 0, 40, 0, 60, 8);
                    nv_gfx_tone(180, 60);
                }
                s_ecd[e] = reload;
            }
        } else s_eyaw[e] += 0.4f * dt;                 // idle: it sweeps
        vx_obj_rot(s_ehead[e], nv_roundi(s_epitch[e]), nv_roundi(nv_deg(s_eyaw[e])), 0);
    }
    for (int k = 0; k < NESH; k++) {
        if (s_eshlife[k] <= 0) continue;
        s_eshx[k] += s_eshvx[k] * dt; s_eshz[k] += s_eshvz[k] * dt; s_eshlife[k] -= dt;
        s_eshy[k] = s_eshy[k] + s_eshvy[k] * dt;
        if (s_eshy[k] < 30) s_eshy[k] = 30;
        vx_obj_pos(s_esh[k], nv_roundi(s_eshx[k]), nv_roundi(s_eshy[k]), nv_roundi(s_eshz[k]));
        int hit = 0;
        const float dx = s_eshx[k] - s_x, dz = s_eshz[k] - s_z;
        if (s_hp > 0 && dx * dx + dz * dz < 58 * 58) { tank_hit(now, nv_clampf(22.0f + s_level * 2, 22, 34)); hit = 1; }
        float px = s_eshx[k], pz = s_eshz[k];
        if (!hit && collide(&px, &pz, 6)) {
            int own = 0;                               // its own tower's base does not stop it
            for (int e = 0; e < s_ne; e++) { const float ex = s_eshx[k] - s_ex[e], ez = s_eshz[k] - s_ez[e]; if (ex * ex + ez * ez < 100 * 100) own = 1; }
            if (!own) { hit = 1; vx_emit(s_fx_spark, nv_roundi(s_eshx[k]), 40, nv_roundi(s_eshz[k]), 0, 80, 0, 90, 8); }
        }
        if (hit || s_eshlife[k] <= 0) { s_eshlife[k] = 0; vx_obj_show(s_esh[k], 0); }
    }
    if (s_hp > 0 && now - s_hurt_at < 1100) tank_show((now / 90) & 1);   // blinking: hit
    else if (s_hp > 0) tank_show(1);
}
static void play_update(float dt, In in, int now) {
    // driving: tracks bite, the hull turns in place too
    s_speed = nv_approach(s_speed, in.gas * 430.0f, (in.gas != 0 ? 520.0f : 380.0f) * dt);
    static float steer;                                // digital buttons, eased a touch
    steer = nv_approach(steer, nv_clampf(in.steer, -1, 1), 7.0f * dt);
    s_yaw += steer * dt * (1.6f + 0.8f * nv_clampf(nv_absf(s_speed) / 400, 0, 1)) * (s_speed < -20 ? -1 : 1);
    s_x += nv_sinf(s_yaw) * s_speed * dt;
    s_z += nv_cosf(s_yaw) * s_speed * dt;
    if (collide(&s_x, &s_z, TANK_R)) {
        if (nv_absf(s_speed) > 150) { s_shake = 5; nv_gfx_tone(70, 50); rumble(16000, 8000, 80); }
        if (nv_absf(s_speed) > 330 && now - s_bump_at > 700) {   // ramming a rock at full speed dents it
            s_bump_at = now; s_hp -= 4;
            vx_emit(s_fx_spark, nv_roundi(s_x + nv_sinf(s_yaw) * 60), 30, nv_roundi(s_z + nv_cosf(s_yaw) * 60), 0, 120, 0, 80, 8);
            if (s_hp < 1) s_hp = 1;
        }
        s_speed *= 0.5f;
    }
    vx_obj_pos(s_hull, nv_roundi(s_x), 0, nv_roundi(s_z));
    vx_obj_rot(s_hull, 0, nv_roundi(nv_deg(s_yaw)), 0);
    if (nv_absf(s_speed) > 60 && (now / 70) % 2 == 0) {
        const float bx = s_x - nv_sinf(s_yaw) * 60, bz = s_z - nv_cosf(s_yaw) * 60;
        vx_emit(s_fx_dust, nv_roundi(bx), 6, nv_roundi(bz), 0, 40, 0, 40, 1);
    }
    // the turret swings toward the nearest crate in front, else straight ahead
    float kx, kz, want = 0, wd = 1e12f;
    if (nearest(0, &kx, &kz) >= 0) {
        const float a = nv_angle_diff(s_yaw, nv_atan2f(kx - s_x, kz - s_z)), dx = kx - s_x, dz = kz - s_z;
        if (nv_absf(a) < 1.1f && dx * dx + dz * dz < 1200.0f * 1200.0f) { want = a; wd = dx * dx + dz * dz; }
    }
    for (int e = 0; e < s_ne; e++) {                   // a tower in front beats a crate: it shoots back
        if (s_ehp[e] <= 0) continue;
        const float dx = s_ex[e] - s_x, dz = s_ez[e] - s_z, d = (dx * dx + dz * dz) * 0.6f;
        const float a = nv_angle_diff(s_yaw, nv_atan2f(dx, dz));
        if (nv_absf(a) < 1.1f && d < wd && d < 1300.0f * 1300.0f) { want = a; wd = d; }
    }
    if (in.aim != 0 || in.aim_rad != 0) {                // by hand: right stick, Q/E, the mouse
        s_tyaw += in.aim * 2.4f * dt + in.aim_rad;
        s_manual_until = now + 3000;
    }
    if (now < s_manual_until) s_tyaw = nv_clampf(nv_angle_diff(0, s_tyaw), -NV_PI, NV_PI);
    else s_tyaw = nv_approach(s_tyaw, want, 2.6f * dt);
    vx_obj_rot(s_turret, 0, nv_roundi(nv_deg(s_tyaw)), 0);
    s_recoil = nv_approach(s_recoil, 0, 4.0f * dt);
    vx_obj_pos(s_barrel, 0, 10, nv_roundi(20 - s_recoil * 16));
    s_fire_cd -= dt;
    if (in.fire && s_fire_cd <= 0) fire();
    // shells
    for (int k = 0; k < NSHELL; k++) {
        if (s_shlife[k] <= 0) continue;
        s_shx[k] += s_shvx[k] * dt; s_shz[k] += s_shvz[k] * dt; s_shlife[k] -= dt;
        vx_obj_pos(s_shell[k], nv_roundi(s_shx[k]), 46, nv_roundi(s_shz[k]));
        vx_obj_rot(s_shell[k], 0, nv_roundi(nv_deg(nv_atan2f(s_shvx[k], s_shvz[k]))), 0);
        int hit = 0;
        for (int c = 0; c < s_ncrate && !hit; c++)
            if (s_crate_on[c]) { const float dx = s_shx[k] - s_kx[c], dz = s_shz[k] - s_kz[c]; if (dx * dx + dz * dz < 50 * 50) { crate_hit(c); hit = 1; } }
        for (int e = 0; e < s_ne && !hit; e++)
            if (s_ehp[e] > 0) { const float dx = s_shx[k] - s_ex[e], dz = s_shz[k] - s_ez[e]; if (dx * dx + dz * dz < 54 * 54) { tower_hit(e); hit = 1; } }
        float px = s_shx[k], pz = s_shz[k];
        if (!hit && collide(&px, &pz, 6)) {                   // a rock, a tree, the fence
            hit = 1;
            vx_emit(s_fx_spark, nv_roundi(s_shx[k]), 40, nv_roundi(s_shz[k]), 0, 100, 0, 120, 12);
            vx_emit(s_fx_smoke, nv_roundi(s_shx[k]), 40, nv_roundi(s_shz[k]), 0, 40, 0, 40, 4);
            nv_gfx_tone(220, 40);
        }
        if (hit || s_shlife[k] <= 0) { s_shlife[k] = 0; vx_obj_show(s_shell[k], 0); }
    }
    towers_update(dt, now);
    if (s_hp_trail > s_hp) { if (now - s_hurt_at > 500) s_hp_trail = nv_approach(s_hp_trail, s_hp, 40 * dt); }
    else s_hp_trail = s_hp;
    if (s_hp <= 0) return;
    // repair kits: drive over one
    for (int c = 0; c < s_ncrate; c++) {
        if (s_kit_on[c] != 2) continue;
        vx_obj_rot(s_kit[c], 0, (now / 8) % 360, 0);
        const float dx = s_kx[c] - s_x, dz = s_kz[c] - s_z;
        if (dx * dx + dz * dz < 80 * 80) {
            s_kit_on[c] = 0; vx_obj_show(s_kit[c], 0);
            s_hp = s_hp + 35 > HP_MAX ? HP_MAX : s_hp + 35;
            say(T("RIPARATO! +35", "REPAIRED! +35"));
            vx_emit(s_fx_glint, nv_roundi(s_x), 50, nv_roundi(s_z), 0, 160, 0, 100, 20);
            nv_gfx_tone(660, 60); nv_gfx_tone(990, 120);
            rumble(8000, 0, 120);
        }
    }
    // coins: spin, picked up by driving through; a quick chain is a combo
    const int spin = (now / 4) % 360;
    for (int i = 0; i < s_ncoin; i++) {
        if (!s_coin_on[i] || s_coin_hidden[i]) continue;
        vx_obj_rot(s_coin[i], 0, spin, 0);
        vx_obj_pos(s_coin[i], nv_roundi(s_cx[i]), nv_roundi(46 + nv_sinf(now * 0.004f + i) * 8), nv_roundi(s_cz[i]));
        const float dx = s_cx[i] - s_x, dz = s_cz[i] - s_z;
        if (dx * dx + dz * dz < 75 * 75) {
            s_coin_on[i] = 0; vx_obj_show(s_coin[i], 0);
            s_combo = now - s_combo_at < 3500 ? s_combo + 1 : 1; s_combo_at = now;
            s_score += 100 * s_combo; add_time(2);
            vx_emit(s_fx_glint, nv_roundi(s_cx[i]), 50, nv_roundi(s_cz[i]), 0, 180, 0, 120, 24);
            nv_gfx_tone(988 + s_combo * 120, 70);
            if (s_combo >= 2) say(s_combo >= 4 ? T("COMBO FANTASTICA!", "AMAZING COMBO!") : "COMBO!");
        }
    }
    // camera: chase, a little behind and above, with shake
    const NvVec3 fwd = nv_v3_dir(s_yaw, 0);
    const NvVec3 eye = nv_v3(s_x - fwd.x * 330, 190, s_z - fwd.z * 330);
    const NvVec3 at = nv_v3(s_x + fwd.x * 220, 30, s_z + fwd.z * 220);
    s_eye = nv_v3_lerp(s_eye, eye, nv_clampf(5 * dt, 0, 1));
    s_at = nv_v3_lerp(s_at, at, nv_clampf(7 * dt, 0, 1));
    s_shake = nv_approach(s_shake, 0, 30 * dt);
    const float sx = (nv_rand_float(&s_rnd) - 0.5f) * s_shake * 2, sy = (nv_rand_float(&s_rnd) - 0.5f) * s_shake * 2;
    vx_lens(62, 16, 7000);
    vx_camera(nv_roundi(s_eye.x + sx), nv_roundi(s_eye.y + sy), nv_roundi(s_eye.z), 0, 0, 0);
    vx_look_at(nv_roundi(s_at.x), nv_roundi(s_at.y), nv_roundi(s_at.z));
    // the clock
    s_time_ms -= now - s_clock_at; s_clock_at = now;
    if (coins_left() == 0) {
        s_state = ST_CLEAR; s_state_at = now;
        nv_gfx_tone(784, 120); nv_gfx_tone(1046, 260);
    } else if (s_time_ms <= 0) {
        s_time_ms = 0; s_dead = 0;
        game_over(now);
    }
}

// ---- 2D -----------------------------------------------------------------------------------------
#define C_PANEL_T NV_RGB(30, 46, 80)
#define C_PANEL_B NV_RGB(8, 16, 34)
static void panel(int x, int y, int w, int h) { nv_gfx_panel(x, y, w, h, 7, C_PANEL_T, C_PANEL_B, 205); }
static void minimap(void) {
    const int cx = W - 46, cy = 82, r = 38;
    nv_gfx_circle(cx, cy, r + 2, NV_RGB(90, 120, 170));
    nv_gfx_circle(cx, cy, r, NV_RGB(12, 22, 40));
    const float k = r / ARENA_R, c = nv_cosf(s_yaw), s = nv_sinf(s_yaw);
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < (pass ? s_ncoin : s_ncrate); i++) {
            const int on = pass ? s_coin_on[i] && !s_coin_hidden[i] : s_crate_on[i];
            if (!on) continue;
            const float dx = (pass ? s_cx[i] : s_kx[i]) - s_x, dz = (pass ? s_cz[i] : s_kz[i]) - s_z;
            const float rx = dx * c - dz * s, rz = dx * s + dz * c;        // the map turns with the tank
            int px = cx + nv_roundi(rx * k), py = cy - nv_roundi(rz * k);
            const int ddx = px - cx, ddy = py - cy;
            if (ddx * ddx + ddy * ddy > (r - 3) * (r - 3)) continue;
            nv_gfx_rect(px - 1, py - 1, 3, 3, pass ? NV_RGB(255, 220, 60) : NV_RGB(200, 140, 80));
        }
    for (int e = 0; e < s_ne; e++) {
        if (s_ehp[e] <= 0) continue;
        const float dx = s_ex[e] - s_x, dz = s_ez[e] - s_z, rx = dx * c - dz * s, rz = dx * s + dz * c;
        const int px = cx + nv_roundi(rx * k), py = cy - nv_roundi(rz * k), ddx = px - cx, ddy = py - cy;
        if (ddx * ddx + ddy * ddy <= (r - 3) * (r - 3)) nv_gfx_rect(px - 2, py - 2, 5, 5, NV_RGB(255, 60, 50));
    }
    nv_gfx_tri(cx, cy - 5, cx - 4, cy + 4, cx + 4, cy + 4, NV_RGB(255, 255, 255));
}
// one round on-screen button: lit while pressed
static void button(int i, int on, const char *label, int top, int bot) {
    const Btn *b = &k_btn[i];
    nv_gfx_panel(b->x - b->r, b->y - b->r, b->r * 2, b->r * 2, b->r, on ? NV_RGB(255, 255, 255) : top, bot, on ? 220 : 120);
    if (label) nv_gfx_text(b->x - nv_gfx_text_width(label, 1) / 2, b->y + b->r / 3, label, NV_RGB(255, 255, 255), 1);
}
static void draw_controls(int btn) {
    const int g = NV_RGB(160, 170, 190), gb = NV_RGB(60, 70, 90), wh = NV_RGB(255, 255, 255);
    button(0, btn & B_LEFT, 0, g, gb);
    button(1, btn & B_RIGHT, 0, g, gb);
    nv_gfx_tri(k_btn[0].x - 14, k_btn[0].y, k_btn[0].x + 10, k_btn[0].y - 14, k_btn[0].x + 10, k_btn[0].y + 14, wh);
    nv_gfx_tri(k_btn[1].x + 14, k_btn[1].y, k_btn[1].x - 10, k_btn[1].y - 14, k_btn[1].x - 10, k_btn[1].y + 14, wh);
    button(2, btn & B_GAS, "GAS", NV_RGB(120, 230, 120), NV_RGB(30, 130, 50));
    nv_gfx_tri(k_btn[2].x, k_btn[2].y - 24, k_btn[2].x - 13, k_btn[2].y - 2, k_btn[2].x + 13, k_btn[2].y - 2, wh);
    button(3, btn & B_BACK, T("RETRO", "BACK"), NV_RGB(150, 180, 230), NV_RGB(40, 70, 130));
    nv_gfx_tri(k_btn[3].x, k_btn[3].y + 4, k_btn[3].x - 10, k_btn[3].y - 12, k_btn[3].x + 10, k_btn[3].y - 12, wh);
    button(4, btn & B_FIRE, T("FUOCO", "FIRE"), NV_RGB(255, 150, 90), NV_RGB(190, 50, 30));
    nv_gfx_circle(k_btn[4].x, k_btn[4].y - 6, 9, wh);
    nv_gfx_circle(k_btn[4].x, k_btn[4].y - 6, 5, NV_RGB(210, 70, 40));
    if (s_fire_cd > 0) {                               // reloading: a bar under the button
        const int w = nv_roundi(56 * nv_clampf(s_fire_cd / 0.55f, 0, 1));
        nv_gfx_rect(k_btn[4].x - 28, k_btn[4].y + k_btn[4].r + 2, w, 3, NV_RGB(255, 200, 120));
    }
}
// the life bar: a heart, the bar (green, then amber, then red), the white trail of what was just lost
static void life_bar(int x, int y, int w, int now) {
    const int beat = s_hp < 30 && (now / 220) & 1;
    const int hc = beat ? NV_RGB(255, 140, 130) : NV_RGB(235, 50, 60);
    nv_gfx_circle(x + 5, y + 5, 5, hc); nv_gfx_circle(x + 13, y + 5, 5, hc);
    nv_gfx_tri(x, y + 7, x + 18, y + 7, x + 9, y + 17, hc);
    const int bx = x + 24, bw = w - 26, h = 14;
    nv_gfx_panel(bx - 2, y - 1, bw + 4, h + 2, 5, NV_RGB(10, 14, 24), NV_RGB(10, 14, 24), 255);
    const float f = nv_clampf(s_hp / HP_MAX, 0, 1), ft = nv_clampf(s_hp_trail / HP_MAX, 0, 1);
    const int wt = nv_roundi(bw * ft), wf = nv_roundi(bw * f);
    if (wt > wf) nv_gfx_panel(bx + wf, y + 1, wt - wf, h - 2, 3, NV_RGB(255, 250, 230), NV_RGB(230, 200, 170), 255);
    const int top = f > 0.5f ? NV_RGB(140, 240, 110) : f > 0.25f ? NV_RGB(255, 210, 80) : NV_RGB(255, 100, 80);
    const int bot = f > 0.5f ? NV_RGB(40, 150, 50) : f > 0.25f ? NV_RGB(200, 120, 20) : NV_RGB(170, 20, 20);
    if (wf > 2) nv_gfx_panel(bx, y + 1, wf, h - 2, 3, top, bot, 255);
    for (int i = 1; i < 4; i++) nv_gfx_rect(bx + bw * i / 4, y + 1, 1, h - 2, NV_RGB(10, 14, 24));   // quarter ticks
    char b[8];
    nv_snprintf(b, sizeof b, "%d", (int)(s_hp + 0.5f));
    nv_gfx_text(bx + bw - nv_gfx_text_width(b, 1) - 4, y + 3, b, NV_RGB(255, 255, 255), 1);
}
static void draw_hud(int now, int btn) {
    char b[48];
    if (now - s_hurt_at < 260) nv_gfx_panel(0, 0, W, H, 0, NV_RGB(255, 40, 20), NV_RGB(160, 0, 0), 70);
    if (s_hp < 30 && s_hp > 0) {                       // badly hurt: the edges pulse red
        const int a = 40 + nv_roundi(nv_sinf(now * 0.008f) * 30);
        nv_gfx_panel(0, 0, W, 10, 0, NV_RGB(255, 30, 20), NV_RGB(255, 30, 20), a);
        nv_gfx_panel(0, H - 10, W, 10, 0, NV_RGB(255, 30, 20), NV_RGB(255, 30, 20), a);
        nv_gfx_panel(0, 10, 8, H - 20, 0, NV_RGB(255, 30, 20), NV_RGB(255, 30, 20), a);
        nv_gfx_panel(W - 8, 10, 8, H - 20, 0, NV_RGB(255, 30, 20), NV_RGB(255, 30, 20), a);
    }
    panel(6, 6, 176, 60);
    life_bar(14, 12, 160, now);
    nv_snprintf(b, sizeof b, "%s %d", T("PUNTI", "SCORE"), s_score);
    nv_gfx_text(14, 34, b, NV_RGB(255, 255, 255), 1);
    nv_snprintf(b, sizeof b, "%s %d/%d", T("MONETE", "COINS"), s_ncoin - coins_left(), s_ncoin);
    nv_gfx_text(14, 49, b, NV_RGB(255, 220, 80), 1);
    const int sec = (s_time_ms + 999) / 1000;
    nv_snprintf(b, sizeof b, "%d", sec);
    panel(W / 2 - 34, 6, 68, 34);
    nv_gfx_text(W / 2 - nv_gfx_text_width(b, 3) / 2, 12, b, sec <= 10 && (now / 250) & 1 ? NV_RGB(255, 80, 60) : NV_RGB(255, 255, 255), 3);
    if (now - s_bonus_at < 900) {
        nv_snprintf(b, sizeof b, "+%d", s_bonus_show);
        nv_gfx_text(W / 2 + 40, 16 - (now - s_bonus_at) / 60, b, NV_RGB(120, 255, 140), 2);
    }
    minimap();
    if (s_msg && now - s_msg_at < 1200) nv_gfx_text_center(64, s_msg, NV_RGB(255, 230, 90), 2);
    // a marker over the nearest coin in view
    float cx, cz;
    int32_t p[3];
    if (nearest(1, &cx, &cz) >= 0 && vx_project(nv_roundi(cx), 120, nv_roundi(cz), p) && p[0] > 0 && p[0] < W && p[1] > 40 && p[1] < H) {
        const int bob = nv_roundi(nv_sinf(now * 0.008f) * 3);
        nv_gfx_tri(p[0] - 8, p[1] - 12 + bob, p[0] + 8, p[1] - 12 + bob, p[0], p[1] + bob, NV_RGB(255, 230, 90));
    }
    for (int e = 0; e < s_ne; e++) {                  // the towers' life over their heads
        if (s_ehp[e] <= 0) continue;
        const float dx = s_ex[e] - s_x, dz = s_ez[e] - s_z;
        if (dx * dx + dz * dz > 2600.0f * 2600.0f) continue;
        if (!vx_project(nv_roundi(s_ex[e]), TOWER_Y + 60, nv_roundi(s_ez[e]), p) || p[0] < 20 || p[0] > W - 20 || p[1] > H) continue;
        if (p[1] < 12) p[1] = 12;                      // close up the head is off the top: keep the bar on screen
        nv_gfx_rect(p[0] - 19, p[1] - 3, 38, 7, NV_RGB(10, 10, 16));
        for (int i = 0; i < TOWER_HP; i++)
            nv_gfx_rect(p[0] - 17 + i * 12, p[1] - 1, 10, 3, i < s_ehp[e] ? NV_RGB(255, 70, 50) : NV_RGB(70, 50, 50));
    }
    if (touch_ui()) draw_controls(btn);
}
static void big_box(int y, int h) { nv_gfx_panel(W / 2 - 170, y, 340, h, 16, C_PANEL_T, C_PANEL_B, 220); }
static void draw_table(int y, int now) {
    char b[48];
    nv_gfx_panel(W / 2 - 130, y, 260, 22 + NHI * 17, 10, C_PANEL_T, C_PANEL_B, 200);
    nv_gfx_text_center(y + 6, T("MIGLIORI PUNTEGGI", "HIGH SCORES"), NV_RGB(255, 210, 60), 1);
    for (int i = 0; i < NHI; i++) {
        const int ry = y + 22 + i * 17, hl = i == s_hi_new && ((now / 250) & 1);
        const int c = hl ? NV_RGB(120, 255, 140) : i == 0 ? NV_RGB(255, 230, 140) : NV_RGB(225, 232, 250);
        nv_snprintf(b, sizeof b, "%d.", i + 1);
        nv_gfx_text(W / 2 - 112, ry, b, c, 2);
        nv_gfx_text(W / 2 - 80, ry, s_hi[i].name, c, 2);
        nv_snprintf(b, sizeof b, "%d", s_hi[i].score);
        nv_gfx_text(W / 2 + 40 - nv_gfx_text_width(b, 2), ry, b, c, 2);
        nv_snprintf(b, sizeof b, "%s%d", T("AR.", "AR."), s_hi[i].arena);
        nv_gfx_text(W / 2 + 64, ry + 4, b, NV_RGB(150, 165, 200), 1);
    }
}
static void draw_title(int now) {
    big_box(10, 78);
    nv_gfx_text_center(20, "TANK RALLY", NV_RGB(255, 210, 60), 5);
    nv_gfx_text_center(62, T("SPARA ALLE CASSE, RACCOGLI LE MONETE", "SHOOT THE CRATES, GRAB THE COINS"), NV_RGB(220, 230, 255), 1);
    nv_gfx_text_center(73, T("PRIMA CHE SCADA IL TEMPO", "BEFORE THE CLOCK RUNS OUT"), NV_RGB(220, 230, 255), 1);
    draw_table(92, now);
    if ((now / 500) & 1) nv_gfx_text_center(H - 66, T("TOCCA O PREMI A PER INIZIARE", "TAP OR PRESS A TO START"), NV_RGB(255, 255, 255), 2);
    nv_gfx_text_center(H - 38, s_dev == DEV_PAD ? T("LEVETTA: GUIDA  RT: GAS  LT: RETRO  A/RB: FUOCO  LEVETTA DX: TORRETTA",
                                                    "STICK: STEER  RT: GAS  LT: BACK  A/RB: FIRE  RIGHT STICK: TURRET")
                             : s_dev == DEV_KEYS ? T("WASD: GUIDA  SPAZIO: FUOCO  Q/E O MOUSE: TORRETTA  ESC: ESCI",
                                                     "WASD: DRIVE  SPACE: FIRE  Q/E OR MOUSE: TURRET  ESC: QUIT")
                             : T("TASTI A SCHERMO, OPPURE COLLEGA JOYPAD, TASTIERA O MOUSE",
                                 "ON-SCREEN BUTTONS, OR PLUG IN A GAMEPAD, KEYBOARD OR MOUSE"), NV_RGB(170, 190, 220), 1);
    nv_gfx_text_center(H - 24, T("LE TORRETTE SPARANO: NELLE CASSE CI SONO KIT DI RIPARAZIONE",
                                 "THE TOWERS SHOOT BACK: SOME CRATES HOLD REPAIR KITS"), NV_RGB(255, 150, 130), 1);
}
static void draw_intro(int now) {
    char b[48];
    const Arena *A = &k_arena[s_level % 5];
    big_box(70, 80);
    nv_snprintf(b, sizeof b, "%s %d", T("ARENA", "ARENA"), s_level + 1);
    nv_gfx_text_center(82, b, NV_RGB(200, 210, 240), 2);
    nv_gfx_text_center(104, T(A->it, A->en), NV_RGB(255, 210, 60), 4);
    const int t = now - s_state_at;
    nv_gfx_text_center(170, t < 1400 ? T("PRONTI...", "READY...") : "GO!", t < 1400 ? NV_RGB(255, 255, 255) : NV_RGB(120, 255, 140), 3);
}
static void draw_clear(int now) {
    char b[48];
    big_box(60, 110);
    nv_gfx_text_center(72, T("ARENA COMPLETATA!", "ARENA CLEAR!"), NV_RGB(120, 255, 140), 3);
    const int left = (s_time_ms + 999) / 1000;
    nv_snprintf(b, sizeof b, "%s %d x 25 = %d", T("TEMPO", "TIME"), left, left * 25);
    nv_gfx_text_center(110, b, NV_RGB(255, 255, 255), 2);
    nv_snprintf(b, sizeof b, "%s %d", T("PUNTI", "SCORE"), s_score + left * 25);
    nv_gfx_text_center(136, b, NV_RGB(255, 220, 80), 2);
    (void)now;
}
static void draw_over(int now) {
    char b[48];
    const int t = now - s_state_at;
    if (t < 400) nv_gfx_panel(0, 0, W, H, 0, NV_RGB(0, 0, 0), NV_RGB(0, 0, 0), t * 120 / 400);
    else nv_gfx_panel(0, 0, W, H, 0, NV_RGB(0, 0, 0), NV_RGB(0, 0, 0), 120);
    big_box(40, 140);
    const int sc = t < 600 ? 7 - t / 120 : 6;           // GAME OVER slams in
    nv_gfx_text_center(56, "GAME OVER", NV_RGB(255, 90, 70), sc < 5 ? 5 : sc);
    nv_gfx_text_center(112, s_dead ? T("CARRO DISTRUTTO", "TANK DESTROYED") : T("TEMPO SCADUTO", "TIME'S UP"), NV_RGB(255, 200, 180), 2);
    nv_snprintf(b, sizeof b, "%s %d   %s %d", T("ARENA", "ARENA"), s_level + 1, T("PUNTI", "SCORE"), s_score);
    nv_gfx_text_center(140, b, NV_RGB(255, 255, 255), 2);
    const int r = hi_rank(s_score);
    if (r < NHI) { if ((now / 300) & 1) nv_gfx_text_center(162, r == 0 ? T("NUOVO RECORD!", "NEW RECORD!") : T("SEI IN CLASSIFICA!", "YOU MADE THE TABLE!"), NV_RGB(120, 255, 140), 1); }
    else { nv_snprintf(b, sizeof b, "%s %d", T("RECORD", "BEST"), s_hi[0].score); nv_gfx_text_center(162, b, NV_RGB(200, 210, 230), 1); }
    if (t > 1500 && ((now / 500) & 1)) nv_gfx_text_center(H - 64, T("TOCCA O PREMI A", "TAP OR PRESS A"), NV_RGB(255, 255, 255), 2);
}
// initials: three letters, arrows above/below each, OK
#define NM_X(i) (W / 2 - 96 + (i) * 56)
#define NM_Y 120
static void draw_name(int now) {
    char b[4] = { 0, 0, 0, 0 };
    big_box(30, 210);
    nv_gfx_text_center(42, hi_rank(s_score) == 0 ? T("NUOVO RECORD!", "NEW RECORD!") : T("SEI IN CLASSIFICA!", "YOU MADE THE TABLE!"), NV_RGB(120, 255, 140), 3);
    nv_gfx_text_center(76, T("SCRIVI LE TUE INIZIALI", "ENTER YOUR INITIALS"), NV_RGB(220, 230, 255), 1);
    for (int i = 0; i < 3; i++) {
        const int x = NM_X(i), cur = i == s_name_pos;
        nv_gfx_tri(x + 20, NM_Y - 22, x + 8, NM_Y - 8, x + 32, NM_Y - 8, cur ? NV_RGB(255, 220, 80) : NV_RGB(140, 150, 180));
        nv_gfx_panel(x, NM_Y, 40, 50, 8, cur ? NV_RGB(80, 110, 190) : NV_RGB(50, 66, 110), NV_RGB(20, 30, 60), 255);
        b[0] = s_name[i];
        nv_gfx_text(x + 20 - nv_gfx_text_width(b, 4) / 2, NM_Y + 10, b, cur && (now / 300) & 1 ? NV_RGB(255, 220, 80) : NV_RGB(255, 255, 255), 4);
        nv_gfx_tri(x + 20, NM_Y + 72, x + 8, NM_Y + 58, x + 32, NM_Y + 58, cur ? NV_RGB(255, 220, 80) : NV_RGB(140, 150, 180));
    }
    nv_gfx_panel(NM_X(3) - 4, NM_Y + 4, 52, 42, 10, NV_RGB(120, 230, 120), NV_RGB(30, 130, 50), 255);
    nv_gfx_text(NM_X(3) + 22 - nv_gfx_text_width("OK", 2) / 2, NM_Y + 17, "OK", NV_RGB(255, 255, 255), 2);
    char s[48];
    nv_snprintf(s, sizeof s, "%s %d", T("PUNTI", "SCORE"), s_score);
    nv_gfx_text_center(NM_Y + 86, s, NV_RGB(255, 220, 80), 2);
}
static int name_char(int i) { for (int k = 0; k < (int)sizeof k_chars - 1; k++) if (k_chars[k] == s_name[i]) return k; return 0; }
static void name_step(int i, int d) {
    const int n = (int)sizeof k_chars - 1;
    s_name[i] = k_chars[(name_char(i) + d + n) % n];
    nv_gfx_tone(660 + d * 60, 30);
}
// returns 1 when the initials are confirmed
static int name_update(In in) {
    if (in.ch) {                                       // typed on a keyboard: set it, move on
        s_name[s_name_pos] = in.ch;
        nv_gfx_tone(780, 30);
        if (s_name_pos < 2) s_name_pos++;
        return 0;                                      // (the same key may also be W/A/S/D on the pad)
    }
    if (in.bksp) { if (s_name_pos > 0) s_name_pos--; return 0; }
    if (in.up) name_step(s_name_pos, 1);
    if (in.down) name_step(s_name_pos, -1);
    if (in.left && s_name_pos > 0) s_name_pos--;
    if (in.right && s_name_pos < 2) s_name_pos++;
    if (in.tap) {
        for (int i = 0; i < 3; i++)
            if (in.tx >= NM_X(i) - 8 && in.tx < NM_X(i) + 48) {
                s_name_pos = i;
                if (in.ty < NM_Y + 25 && in.ty > NM_Y - 40) name_step(i, 1);
                else if (in.ty >= NM_Y + 25 && in.ty < NM_Y + 90) name_step(i, -1);
            }
        if (in.tx >= NM_X(3) - 10 && in.tx < NM_X(3) + 60 && in.ty > NM_Y - 10 && in.ty < NM_Y + 60) return 1;
    }
    return 0;
}

// ---- flow ---------------------------------------------------------------------------------------
static void start_level(int lv, int now) {
    s_level = lv;
    nv_rand_seed(&s_rnd, (uint32_t)now * 2654435761u + (uint32_t)lv * 977u);   // a new layout every run
    build_arena(lv);
    s_x = 0; s_z = SPAWN_Z; s_yaw = 0; s_speed = 0; s_tyaw = 0; s_recoil = 0; s_fire_cd = 0;
    vx_obj_pos(s_hull, 0, 0, nv_roundi(s_z));
    s_eye = nv_v3(0, 190, s_z - 330); s_at = nv_v3(0, 30, s_z + 220);
    const Arena *A = &k_arena[lv % 5];
    s_time_ms = (A->time_s - (lv / 5) * 8) * 1000;              // later laps: less time
    if (s_time_ms < 30000) s_time_ms = 30000;
    s_combo = 0; s_msg = 0;
    for (int k = 0; k < NESH; k++) s_eshlife[k] = 0;
    if (lv == 0) s_hp = HP_MAX;
    else s_hp = s_hp + 25 > HP_MAX ? HP_MAX : s_hp + 25;     // a clean arena patches you up a bit
    s_hp_trail = s_hp;
    s_hurt_at = -10000;
    s_state = ST_INTRO; s_state_at = now;
    nv_gfx_tone(523, 100);
}
static void orbit_camera(int now) {
    const float a = now * 0.00022f;
    vx_lens(62, 16, 7000);
    vx_camera(nv_roundi(nv_sinf(a) * 1600), 480, nv_roundi(nv_cosf(a) * 1600), 0, 0, 0);
    vx_look_at(0, 40, 0);
}

NV_EXPORT("run") void run(void) {
    { char lang[8] = ""; nv_lang(lang, sizeof lang); s_it = lang[0] == 'i' && lang[1] == 't'; }
    hi_load();
    arenas_init();
    nv_rand_seed(&s_rnd, 1);
    build_arena(0);
    int last = nv_millis(), prev_go = 1;
    s_state_at = last;
    while (nv_gfx_present()) {
        const int now = nv_millis();
        const float dt = nv_clampf((now - last) / 1000.0f, 0, 0.05f);
        last = now;
        static int prev_a = 1;
        const In in = read_input();
        const int go_hit = in.go && !prev_go;
        prev_go = in.go;
        switch (s_state) {
        case ST_TITLE:
            orbit_camera(now);
            if (go_hit) { s_score = START_SCORE; s_hp = HP_MAX; start_level(START_LV, now); }
            break;
        case ST_INTRO:
            orbit_camera(now);
            if (now - s_state_at > 1900) {
                s_state = ST_PLAY; s_clock_at = now; nv_gfx_tone(1046, 150);
                s_eye = nv_v3(s_x, 190, s_z - 330);
            }
            break;
        case ST_PLAY:
            play_update(dt, in, now);
            break;
        case ST_CLEAR:
            orbit_camera(now);
            if (now - s_state_at > 2600) { s_score += ((s_time_ms + 999) / 1000) * 25; start_level(s_level + 1, now); }
            break;
        case ST_OVER:
            orbit_camera(now);
            if (go_hit && now - s_state_at > 1500) {
                if (hi_rank(s_score) < NHI) { s_state = ST_NAME; s_name_pos = 0; s_pad_was = nv_gfx_pad(); }
                else s_state = ST_TITLE;
                s_state_at = now;
            }
            break;
        case ST_NAME:
            orbit_camera(now);
            if (name_update(in) || ((nv_gfx_pad() & (NV_PAD_A | NV_PAD_START)) && !prev_a)) {
                hi_insert(); s_state = ST_TITLE; s_state_at = now; nv_gfx_tone(1046, 160);
            }
            break;
        }
        vx_render();
        if (s_state == ST_PLAY) draw_hud(now, in.btn);
        else if (s_state == ST_TITLE) draw_title(now);
        else if (s_state == ST_INTRO) draw_intro(now);
        else if (s_state == ST_CLEAR) draw_clear(now);
        else if (s_state == ST_NAME) draw_name(now);
        else draw_over(now);
        prev_a = (nv_gfx_pad() & (NV_PAD_A | NV_PAD_START)) != 0;
        if (nv_gfx_pad() & NV_PAD_SELECT) break;
    }
}
