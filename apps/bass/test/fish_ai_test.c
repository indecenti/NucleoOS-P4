// fish_ai_test.c — Vertice Bass fish behaviour, measured on the PC (no engine, no device).
//
//   wsl gcc -O1 -std=gnu11 -DNV_SIM -I sdk/include -I apps/bass apps/bass/test/fish_ai_test.c -lm -o /tmp/fishai && /tmp/fishai
//
// Includes fish.c whole (its statics are inspected) and stubs the engine and the lake. Scenarios, each
// over many seeds and every lure:
//   still    the lure lies still in front of a spot for 60 s (mid-water and on the bed)
//   retrieve the lure comes in at reeling speed with pauses and twitches, the line to the rod tip
// Measured: bites, how long a fish stays "following" without deciding, laps round a still lure,
// time a fish spends on the line, how much the tail moves. Exit 1 when a gate fails.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../fish.c"

// ---- stubs ---------------------------------------------------------------------------------------------
static uint32_t s_rng = 1;
int rnd(int n) { s_rng = s_rng * 1103515245u + 12345u; return n > 0 ? (int)((s_rng >> 8) % (uint32_t)n) : 0; }
void rnd_seed(uint32_t s) { s_rng = s ? s : 1; }
float g_lure_half = 22.0f * 0.46f;
int g_fx_splash, g_fx_bubble, g_fx_dust, g_fx_spark, g_fx_glint, g_boat, g_boat_trim;
Spot g_spot[NSPOTS];
const Stage g_stage[NSTAGES] = {
    [0] = { .mix = { 30, 10, 10, 10, 10, 10, 10, 2, 4, 4 } },
};
int  mb_v(float x, float y, float z, int u, int v) { (void)x; (void)y; (void)z; (void)u; (void)v; return 0; }
void mb_tri(int a, int b, int c, int mat, float ix, float iy, float iz) { (void)a; (void)b; (void)c; (void)mat; (void)ix; (void)iy; (void)iz; }
void mb_quad(int a, int b, int c, int d, int mat, float ix, float iy, float iz) { (void)a; (void)b; (void)c; (void)d; (void)mat; (void)ix; (void)iy; (void)iz; }
int  mb_commit_ex(int m, int uv, int fl) { (void)m; (void)uv; (void)fl; static int id; return id++; }
int32_t vx_clone(int32_t id) { static int c = 1000; (void)id; return c++; }
int32_t vx_texture_load(const char *n, int32_t f) { (void)n; (void)f; return -1; }
int32_t vx_material(int32_t c, int32_t sh, int32_t a, int32_t t, int32_t sp) { (void)c; (void)sh; (void)a; (void)t; (void)sp; return 0; }
void vx_obj_pos(int32_t id, int32_t x, int32_t y, int32_t z) { (void)id; (void)x; (void)y; (void)z; }
void vx_obj_rot(int32_t id, int32_t rx, int32_t ry, int32_t rz) { (void)rx; (void)rz; (void)id; (void)ry; }
void vx_obj_show(int32_t id, int32_t on) { (void)id; (void)on; }
void vx_obj_scale(int32_t id, int32_t pct) { (void)id; (void)pct; }
void vx_emit(int32_t e, int32_t x, int32_t y, int32_t z, int32_t vx, int32_t vy, int32_t vz, int32_t sp, int32_t n) {
    (void)e; (void)x; (void)y; (void)z; (void)vx; (void)vy; (void)vz; (void)sp; (void)n;
}
void nv_log(int32_t lvl, const char *m) { (void)lvl; (void)m; }
int lake_spot_near(float x, float z) {
    for (int i = 0; i < NSPOTS; i++) { const float dx = x - g_spot[i].x, dz = z - g_spot[i].z; if (dx * dx + dz * dz < (g_spot[i].r + 200) * (g_spot[i].r + 200)) return i; }
    return -1;
}
int lake_collide(float *x, float *y, float *z, float r) { (void)x; (void)y; (void)z; (void)r; return 0; }
#ifdef OLD_FISH
const float (*g_line)[3]; int g_line_n;      // the old fish.c knew nothing of the line
#endif

// ---- the measuring -------------------------------------------------------------------------------------
typedef struct {
    int runs, bites, max_follow_runs;
    double t_first_bite, max_follow, laps, line_s, fish_s, tail_sum, tail_n, legs;
} Stats;
#define ROPE 15
static float s_rope[ROPE][3];
static void rope_set(float lx, float ly, float lz) {
    const float ax = 30, ay = SURF + 150, az = 40;
    for (int k = 0; k < ROPE; k++) {
        const float t = (float)k / (ROPE - 1), sag = sinf(t * 3.14159f) * 30;
        s_rope[k][0] = ax + (lx - ax) * t; s_rope[k][1] = ay + (ly - ay) * t - sag; s_rope[k][2] = az + (lz - az) * t;
    }
}
static float line_dist(const Fish *f) {
    float best = 1e9f;
    for (int k = 0; k + 1 < ROPE; k++) {
        const float *a = s_rope[k], *b = s_rope[k + 1];
        if (a[1] > SURF && b[1] > SURF) continue;
        const float abx = b[0] - a[0], aby = b[1] - a[1], abz = b[2] - a[2], L2 = abx * abx + aby * aby + abz * abz + 1e-3f;
        float t = ((f->x - a[0]) * abx + (f->y - a[1]) * aby + (f->z - a[2]) * abz) / L2;
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        const float px = f->x - (a[0] + abx * t), py = f->y - (a[1] + aby * t), pz = f->z - (a[2] + abz * t);
        const float q = sqrtf(px * px + py * py + pz * pz);
        if (q < best) best = q;
    }
    return best;
}

// One run: spawn at the spot, then mode 0 still mid-water, 1 still on the bed, 2 retrieve.
static void run_one(Stats *S, int seed, int lure, int mode) {
    rnd_seed(0xC0FFEEu + seed * 7919u);
    g_spot[0] = (Spot){ 0, 1100, 220, (seed % 4) };
    g_boat_x = 0; g_boat_z = 0; g_depth_bias = 0;
    fish_spawn(0, 1100, 0);
    float lx = rnd(200) - 100, lz = 1100 + rnd(200) - 100, ly = mode == 1 ? 7 : 200;
    const float dt = 1.0f / 25;
    float follow[NSLOT] = { 0 }, ang[NSLOT] = { 0 }, prev[NSLOT] = { 0 };
    int had[NSLOT] = { 0 };
    float prev_tail[NSLOT] = { 0 };
    int bitten = 0;
    S->runs++;
    for (int fr = 0; fr < 25 * 60 && !bitten; fr++) {
        const float t = fr * dt;
        int action = 1;
        if (mode == 2) {                                   // reel 3 s, stop 1.5 s, a twitch now and then
            const float c = fmodf(t, 4.5f);
            action = c < 3.0f ? 0 : 1;
            if (fmodf(t, 9.0f) < 0.3f && t > 1) action = 2;
            if (action != 1) {
                const float d = sqrtf(lx * lx + lz * lz) + 1e-3f;
                lx -= lx / d * 110 * dt; lz -= lz / d * 110 * dt;
                ly += (150 - ly) * dt;
                if (d < 150) break;                        // at the boat
            }
        }
        rope_set(lx, ly, lz);
        g_line = (const float (*)[3])s_rope; g_line_n = ROPE;
        LureState L = { action, lx, ly, lz, lure };
        const int st = fish_update(&L, dt, (int)(t * 1000));
        if (st >= 0) { bitten = 1; S->bites++; S->t_first_bite += t; }
        for (int i = 0; i < NSLOT; i++) {
            Fish *f = &s_fish[i];
            if (!f->active || f->state == 3) continue;
            S->fish_s += dt;
            // on the line: the body (girth ~38 * scale) crossing the line, away from the lure
            const float dl = sqrtf((f->x - lx) * (f->x - lx) + (f->y - ly) * (f->y - ly) + (f->z - lz) * (f->z - lz));
            if (f->state != 2 && dl > 90 * f->sc && line_dist(f) < 30 * f->sc) S->line_s += dt;
            S->tail_sum += fabsf(f->tail_a - prev_tail[i]); S->tail_n += 1; prev_tail[i] = f->tail_a;
            if (f->state == 1) {
                follow[i] += dt;
                if (follow[i] > S->max_follow) S->max_follow = follow[i];
                const float a = atan2f(f->x - lx, f->z - lz);
                if (had[i] && dl < 300) { float da = a - prev[i]; while (da > 3.14159f) da -= 6.28318f; while (da < -3.14159f) da += 6.28318f; ang[i] += da; }
                prev[i] = a; had[i] = 1;
            } else { follow[i] = 0; had[i] = 0; }
        }
    }
    for (int i = 0; i < NSLOT; i++) { const double laps = fabs(ang[i]) / 6.28318; if (laps > S->laps) S->laps = laps; }
}

int main(void) {
    fish_build();
    static const char *mname[3] = { "still mid-water", "still on the bed", "retrieve" };
    int fail = 0;
    for (int mode = 0; mode < 3; mode++) {
        Stats S = { 0 };
        for (int lure = 0; lure < NLURES; lure++)
            for (int seed = 0; seed < 50; seed++) run_one(&S, seed + lure * 100, lure, mode);
        const double bite = 100.0 * S.bites / S.runs;
        printf("%-17s runs %3d  bites %5.1f%%  first bite %5.1fs  longest follow %5.1fs  max laps %4.1f  on line %5.2f%%  tail %.4f rad/frame\n",
               mname[mode], S.runs, bite, S.bites ? S.t_first_bite / S.bites : 0, S.max_follow, S.laps,
               100.0 * S.line_s / (S.fish_s + 1e-9), S.tail_sum / (S.tail_n + 1e-9));
#ifndef OLD_FISH
        if (S.max_follow > 25) { printf("  FAIL: a fish followed %.0f s without deciding\n", S.max_follow); fail = 1; }
        if (mode < 2 && S.laps > 1.5) { printf("  FAIL: a fish circled a still lure %.1f times\n", S.laps); fail = 1; }
        if (100.0 * S.line_s / (S.fish_s + 1e-9) > 0.5) { printf("  FAIL: fish on the line\n"); fail = 1; }
        if (mode < 2 && bite < 25) { printf("  FAIL: a still lure is almost never taken\n"); fail = 1; }
#endif
    }
    return fail;
}
