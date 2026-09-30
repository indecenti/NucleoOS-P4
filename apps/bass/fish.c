// fish.c — Vertice Bass: species, fish models, the hunt (wander -> notice -> follow -> strike) and
// the fight (runs, jumps, line tension, stamina).
#include "bass.h"

//                       name it / en        kg min  max   depth  speed  power  steady stop twitch
const Species g_species[NSPECIES] = {
    { "PERSICO TROTA", "LARGEMOUTH BASS", 0.6f, 4.8f, 210, 170, 1.0f, { 0.55f, 1.15f, 1.60f } },
    { "TROTA IRIDEA",  "RAINBOW TROUT",   0.4f, 3.2f, 300, 240, 0.8f, { 1.45f, 0.40f, 0.80f } },
    { "LUCCIO",        "NORTHERN PIKE",   1.5f, 9.0f, 240, 270, 1.4f, { 1.20f, 0.30f, 1.45f } },
    { "PESCE GATTO",   "CATFISH",         1.5f, 12.f,  45, 110, 1.3f, { 0.30f, 1.55f, 0.45f } },
    { "CARPA",         "COMMON CARP",     1.2f, 14.f,  70, 120, 1.5f, { 0.35f, 1.40f, 0.25f } },
    { "PERSICO REALE", "YELLOW PERCH",    0.2f, 1.6f, 190, 200, 0.6f, { 1.10f, 0.90f, 1.30f } },
    { "LUCIOPERCA",    "ZANDER",          1.0f, 8.0f, 110, 230, 1.2f, { 1.25f, 0.70f, 1.05f } },
    { "PERSICO D'ORO", "GOLDEN BASS",     3.0f, 8.5f, 170, 210, 1.7f, { 1.00f, 1.00f, 1.00f } },
};
const char *const g_lure_it[NLURES] = { "CRANKBAIT", "POPPER", "VERME" };
const char *const g_lure_en[NLURES] = { "CRANKBAIT", "POPPER", "WORM" };

#define PER_SP 3
#define NSLOT (NSPECIES * PER_SP)
typedef struct {
    int   obj, species, active, state;     // state 0 wander 1 follow 2 strike 3 flee
    float x, y, z, yaw, speed, interest, kg, hx, hz, t, wig;
} Fish;
static Fish s_fish[NSLOT];

// ---- models ------------------------------------------------------------------------------------------
// A spindle of hexagonal rings, back/flank/belly materials by facet, a forked tail and a dorsal fin.
// Nose toward +Z, 110 units long; vx_obj_scale sizes each catch by its weight.
static int build_fish(int sp) {
    static const uint16_t pal[NSPECIES][5] = {   // back, flank, belly, fin, accent
        { C565(48, 82, 40), C565(112, 142, 72), C565(226, 224, 190), C565(84, 104, 60), C565(38, 58, 30) },
        { C565(70, 96, 84), C565(196, 198, 206), C565(244, 244, 244), C565(150, 140, 130), C565(226, 130, 150) },
        { C565(58, 80, 40), C565(126, 150, 70), C565(232, 230, 196), C565(150, 104, 60), C565(214, 204, 120) },
        { C565(50, 44, 40), C565(92, 82, 70), C565(196, 186, 160), C565(62, 52, 44), C565(40, 34, 30) },
        { C565(92, 76, 36), C565(176, 140, 62), C565(226, 206, 140), C565(150, 96, 60), C565(140, 108, 44) },
        { C565(70, 96, 40), C565(186, 186, 80), C565(236, 232, 200), C565(236, 110, 40), C565(50, 70, 30) },
        { C565(84, 96, 90), C565(150, 160, 150), C565(232, 234, 226), C565(120, 120, 110), C565(70, 80, 76) },
        { C565(206, 140, 20), C565(255, 204, 44), C565(255, 242, 170), C565(255, 160, 40), C565(255, 120, 20) },
    };
    int m[5];
    for (int i = 0; i < 5; i++) m[i] = vx_material(pal[sp][i], i == 4 && sp == SP_GOLD ? VX_UNLIT : VX_GOURAUD, 255, -1, 60);
    const float lz = sp == SP_PIKE ? 1.45f : sp == SP_ZANDER ? 1.25f : 1.0f;   // pike, zander: long
    const float tall = sp == SP_CARP ? 1.35f : sp == SP_PERCH ? 1.2f : 1.0f;   // carp, perch: deep body
    const float fl = sp == SP_CATFISH ? 0.8f : 1.0f;                            // catfish: flat head
    // Eight octagonal sections from tail to snout: a smooth spindle, hump behind the head.
    enum { NR = 8, NS = 8 };
    static const float zs[NR] = { -56, -42, -24, -4, 16, 32, 46, 56 };
    static const float hs[NR] = { 4.5f, 10, 17, 21, 20.5f, 17, 11, 3.5f };
    int ring[NR][NS];
    for (int r = 0; r < NR; r++)
        for (int k = 0; k < NS; k++) {
            const float a = k * 2 * PI_F / NS + PI_F / NS;
            const float h = hs[r] * (sp == SP_PIKE ? 0.82f : tall);
            const float w = hs[r] * (sp == SP_CATFISH ? 0.78f : 0.5f);
            const float y = sinf_(a) * h * (r >= 5 ? fl : 1.0f) + (r >= 2 && r <= 4 && sinf_(a) > 0 ? 2.0f : 0.0f);
            ring[r][k] = mb_v(cosf_(a) * w, y, zs[r] * lz, 0, 0);
        }
    for (int r = 0; r < NR - 1; r++)
        for (int k = 0; k < NS; k++) {
            const float a = (k + 0.5f) * 2 * PI_F / NS + PI_F / NS;      // facet centre angle
            const float sy = sinf_(a);
            int mat = sy > 0.55f ? m[0] : sy < -0.55f ? m[2] : m[1];
            if (mat == m[1]) {                                            // flank markings
                if (sp == SP_BASS && r >= 1 && r <= 4 && (k == 0 || k == 3)) mat = m[4];     // lateral stripe
                if (sp == SP_TROUT && r >= 1 && r <= 5) mat = m[4];                          // rainbow band
                if (sp == SP_PIKE && ((r + k) & 1)) mat = m[4];                              // spots
                if ((sp == SP_PERCH || sp == SP_ZANDER) && (r & 1) && r < 6) mat = m[4];     // bars
                if (sp == SP_CARP && ((r * 3 + k) % 4 == 0)) mat = m[4];                     // big scales
                if (sp == SP_GOLD && (r & 1)) mat = m[4];
            }
            if (r == NR - 2 && sy < 0.3f && sy > -0.9f) mat = m[3];       // mouth / gill edge
            mb_quad(ring[r][k], ring[r][(k + 1) % NS], ring[r + 1][(k + 1) % NS], ring[r + 1][k], mat,
                    0, 0, (zs[r] + zs[r + 1]) / 2 * lz);
        }
    for (int k = 1; k < NS - 1; k++) mb_tri(ring[NR - 1][0], ring[NR - 1][k], ring[NR - 1][k + 1], m[3], 0, 0, 0);
    for (int k = 1; k < NS - 1; k++) mb_tri(ring[0][0], ring[0][k], ring[0][k + 1], m[3], 0, 0, 0);
    // Forked tail (two lobes), visible from both sides.
    const float tz = zs[0] * lz;
    const int t0 = mb_v(0, 0, tz, 0, 0), t1 = mb_v(0, 24, tz - 30, 0, 0), t2 = mb_v(0, 3, tz - 17, 0, 0);
    const int t3 = mb_v(0, -24, tz - 30, 0, 0), t4 = mb_v(0, -3, tz - 17, 0, 0);
    mb_tri(t0, t1, t2, m[3], 5, 0, tz); mb_tri(t0, t1, t2, m[3], -5, 0, tz);
    mb_tri(t0, t3, t4, m[3], 5, 0, tz); mb_tri(t0, t3, t4, m[3], -5, 0, tz);
    // Dorsal fin (two for perch/zander/bass: spiny + soft), anal fin, pectorals.
    const int twin = sp == SP_PERCH || sp == SP_ZANDER || sp == SP_BASS || sp == SP_GOLD;
    for (int f = 0; f < (twin ? 2 : 1); f++) {
        const float z0 = (f ? -30 : -8) * lz, z1 = (f ? -8 : 22) * lz, hy = hs[3] * tall;
        const int d0 = mb_v(0, hy - 1, z0, 0, 0), d1 = mb_v(0, hy - 1, z1, 0, 0), d2 = mb_v(0, hy + (f ? 10 : 15), (z0 + z1) / 2 - 4, 0, 0);
        mb_tri(d0, d1, d2, m[3], 5, 0, 0); mb_tri(d0, d1, d2, m[3], -5, 0, 0);
    }
    {
        const float hy = -hs[2] * tall;
        const int a0 = mb_v(0, hy + 1, -36 * lz, 0, 0), a1 = mb_v(0, hy + 1, -18 * lz, 0, 0), a2 = mb_v(0, hy - 9, -32 * lz, 0, 0);
        mb_tri(a0, a1, a2, m[3], 5, 0, 0); mb_tri(a0, a1, a2, m[3], -5, 0, 0);
    }
    for (int s = -1; s <= 1; s += 2) {                                   // pectoral fins, swept back
        const float x = s * hs[5] * 0.5f;
        const int p0 = mb_v(x, -6, 30 * lz, 0, 0), p1 = mb_v(x + s * 12, -12, 14 * lz, 0, 0), p2 = mb_v(x, -2, 18 * lz, 0, 0);
        mb_tri(p0, p1, p2, m[3], 0, 10, 20); mb_tri(p0, p1, p2, m[3], 0, -10, 20);
    }
    if (sp == SP_CATFISH)                                               // whiskers
        for (int s = -1; s <= 1; s += 2) {
            const int w0 = mb_v(s * 6, -2, 54, 0, 0), w1 = mb_v(s * 32, -12, 70, 0, 0), w2 = mb_v(s * 6, -5, 54, 0, 0);
            mb_tri(w0, w1, w2, m[4], 0, 5, 50); mb_tri(w0, w1, w2, m[4], 0, -5, 50);
        }
    // Eyes: a pale iris with a black pupil on each side of the head.
    const int iris = vx_material(sp == SP_ZANDER ? C565(230, 230, 180) : C565(236, 210, 120), VX_UNLIT, 255, -1, 0);
    const int pupil = vx_material(C565(12, 12, 16), VX_UNLIT, 255, -1, 0);
    for (int s = -1; s <= 1; s += 2) {
        const float ex = s * (hs[6] * (sp == SP_CATFISH ? 0.78f : 0.5f) + 0.8f), ez = 45 * lz, ey = 5;
        const int i0 = mb_v(ex, ey - 3.5f, ez - 3.5f, 0, 0), i1 = mb_v(ex, ey + 3.5f, ez - 3.5f, 0, 0);
        const int i2 = mb_v(ex, ey + 3.5f, ez + 3.5f, 0, 0), i3 = mb_v(ex, ey - 3.5f, ez + 3.5f, 0, 0);
        mb_quad(i0, i1, i2, i3, iris, -s * 20.0f, ey, ez);
        const float px = ex + s * 0.4f;
        const int q0 = mb_v(px, ey - 1.8f, ez - 1.2f, 0, 0), q1 = mb_v(px, ey + 1.8f, ez - 1.2f, 0, 0);
        const int q2 = mb_v(px, ey + 1.8f, ez + 2.4f, 0, 0), q3 = mb_v(px, ey - 1.8f, ez + 2.4f, 0, 0);
        mb_quad(q0, q1, q2, q3, pupil, -s * 20.0f, ey, ez);
    }
    return mb_commit(m[1], 0);
}

void fish_build(void) {
    for (int sp = 0; sp < NSPECIES; sp++) {
        const int proto = build_fish(sp);
        for (int k = 0; k < PER_SP; k++) {
            Fish *f = &s_fish[sp * PER_SP + k];
            f->obj = k ? vx_clone(proto) : proto;
            f->species = sp; f->active = 0;
            vx_obj_show(f->obj, 0);
        }
    }
}

void fish_hide(void) {
    for (int i = 0; i < NSLOT; i++) { s_fish[i].active = 0; vx_obj_show(s_fish[i].obj, 0); }
}

static int pick_species(int stage, int spot_kind) {
    int w[NSPECIES];
    for (int s = 0; s < NSPECIES; s++) w[s] = g_stage[stage].mix[s];
    // Structure matters: weeds and pads hold bass and pike, logs bass and catfish, rocks trout.
    if (spot_kind == SPOT_WEEDS) { w[SP_BASS] *= 2; w[SP_PIKE] *= 2; w[SP_PERCH] *= 2; }
    if (spot_kind == SPOT_PADS) { w[SP_BASS] *= 2; w[SP_CARP] *= 3; }
    if (spot_kind == SPOT_LOG) { w[SP_BASS] *= 2; w[SP_CATFISH] *= 3; w[SP_CARP] *= 2; }
    if (spot_kind == SPOT_ROCKS) { w[SP_TROUT] *= 3; w[SP_ZANDER] *= 2; w[SP_PERCH] *= 2; }
    if (spot_kind < 0) { w[SP_TROUT] *= 2; w[SP_ZANDER] *= 2; w[SP_BASS] /= 2; w[SP_PIKE] /= 2; }   // open water
    int tot = 0;
    for (int s = 0; s < NSPECIES; s++) tot += w[s];
    int r = rnd(tot > 0 ? tot : 1);
    for (int s = 0; s < NSPECIES; s++) { if (r < w[s]) return s; r -= w[s]; }
    return SP_BASS;
}

void fish_spawn(float x, float z, int stage) {
    fish_hide();
    const int spot = lake_spot_near(x, z);
    const int kind = spot >= 0 ? g_spot[spot].kind : -1;
    const float cx = spot >= 0 ? g_spot[spot].x : x, cz = spot >= 0 ? g_spot[spot].z : z;
    const int n = spot >= 0 ? 3 + rnd(3) : 1 + rnd(2);
    for (int i = 0; i < n; i++) {
        const int sp = pick_species(stage, kind);
        int slot = -1;
        for (int k = 0; k < PER_SP && slot < 0; k++) if (!s_fish[sp * PER_SP + k].active) slot = sp * PER_SP + k;
        if (slot < 0) continue;
        Fish *f = &s_fish[slot];
        const Species *S = &g_species[sp];
        const float r = rnd(1000) / 1000.0f;
        f->kg = S->kg_min + (S->kg_max - S->kg_min) * r * r * r;          // big ones are rare
        f->active = 1; f->state = 0; f->interest = 0; f->t = rnd(1000) / 100.0f;
        f->hx = cx + rnd(400) - 200; f->hz = cz + rnd(400) - 200;
        f->x = f->hx; f->z = f->hz; f->y = clampf(S->depth + rnd(80) - 40, 30, SURF - 30);
        f->yaw = rnd(628) / 100.0f; f->speed = S->speed * 0.3f;
        vx_obj_scale(f->obj, iroundf(70 + f->kg * 16 > 220 ? 220 : 70 + f->kg * 16));
        vx_obj_show(f->obj, 1);
    }
}

int fish_species(int i) { return s_fish[i].species; }
float fish_kg(int i) { return s_fish[i].kg; }

void fish_pose(int i, float x, float y, float z, float yaw, float wiggle) {
    Fish *f = &s_fish[i];
    f->x = x; f->y = y; f->z = z; f->yaw = yaw;
    vx_obj_pos(f->obj, iroundf(x), iroundf(y), iroundf(z));
    vx_obj_rot(f->obj, 0, iroundf(deg(yaw + wiggle)), 0);
}

void fish_release_others(int keep) {
    for (int i = 0; i < NSLOT; i++)
        if (i != keep && s_fish[i].active) s_fish[i].state = 3;
}

// The hunt. A fish that sees the lure (in front, within ~5 m, near its depth) gets interested at a
// rate set by how the lure moves (its species' taste); interest decays when the lure does something
// it dislikes. Interested fish follow; a keen one close behind strikes.
int fish_update(const LureState *l, float dt, int now_ms) {
    int striker = -1;
    for (int i = 0; i < NSLOT; i++) {
        Fish *f = &s_fish[i];
        if (!f->active) continue;
        const Species *S = &g_species[f->species];
        f->t += dt;
        const float dx = l->lx - f->x, dy = l->ly - f->y, dz = l->lz - f->z;
        const float d = sqrtf_(dx * dx + dy * dy + dz * dz);
        float tx, ty, tz, spd;
        if (f->state == 3) {                                  // spooked: bolt away and vanish
            tx = f->x - dx * 4; ty = f->y; tz = f->z - dz * 4; spd = S->speed * 1.6f;
            if (d > 900) { f->active = 0; vx_obj_show(f->obj, 0); continue; }
        } else {
            const float ahead = sinf_(f->yaw) * dx + cosf_(f->yaw) * dz;   // lure in front of it?
            const float depthk = 1.0f - clampf(fabsf_(l->ly - S->depth) / 260.0f, 0, 0.75f);
            const float sees = (d < 520 && (ahead > -60 || d < 150)) ? 1.0f : 0.0f;
            const float like = S->like[l->action];
            f->interest += dt * sees * depthk * (like - 0.7f) * (d < 260 ? 1.4f : 1.0f);
            if (!sees) f->interest -= dt * 0.25f;
            f->interest = clampf(f->interest, 0, 2.0f);
            if (f->interest > 0.35f) f->state = 1;
            else if (f->state == 1) f->state = 0;
            if (f->state == 1) {                              // follow a little behind the lure
                const float back = 70 - f->interest * 30;
                const float lx = l->lx + (f->x - l->lx) * back / (d + 1), lz = l->lz + (f->z - l->lz) * back / (d + 1);
                tx = lx; ty = l->ly; tz = lz; spd = S->speed * (0.6f + f->interest * 0.45f);
                if (d < 55 && f->interest > 1.0f && striker < 0) { striker = i; f->state = 2; }
            } else {                                          // cruise around home
                const float a = f->t * 0.35f + i;
                tx = f->hx + sinf_(a) * 160; ty = S->depth + sinf_(f->t * 0.5f) * 40; tz = f->hz + cosf_(a * 0.8f) * 160;
                spd = S->speed * 0.3f;
            }
        }
        const float ex = tx - f->x, ey = ty - f->y, ez = tz - f->z, ed = sqrtf_(ex * ex + ey * ey + ez * ez) + 1e-3f;
        f->speed += (spd - f->speed) * clampf(dt * 3, 0, 1);
        const float step = f->speed * dt < ed ? f->speed * dt : ed;
        const float want = atan2f_(ex, ez);
        f->yaw = wrap_pi(f->yaw + clampf(wrap_pi(want - f->yaw), -dt * 3.5f, dt * 3.5f));
        f->x += sinf_(f->yaw) * step * (fabsf_(wrap_pi(want - f->yaw)) < 1.2f ? 1.0f : 0.3f);
        f->z += cosf_(f->yaw) * step * (fabsf_(wrap_pi(want - f->yaw)) < 1.2f ? 1.0f : 0.3f);
        f->y = clampf(f->y + ey / ed * step, 20, SURF - 20);
        f->wig = sinf_(now_ms * 0.012f * (0.6f + f->speed / 200) + i) * (0.08f + f->speed / 2400);
        vx_obj_pos(f->obj, iroundf(f->x), iroundf(f->y), iroundf(f->z));
        vx_obj_rot(f->obj, 0, iroundf(deg(f->yaw + f->wig)), 0);
    }
    return striker;
}

// ---- the fight -----------------------------------------------------------------------------------------
void fight_start(Fight *f, int fish, float lx, float ly, float lz) {
    f->fish = fish;
    f->dist = sqrtf_(lx * lx + lz * lz);
    f->tension = 0.4f; f->stamina = 1.0f;
    f->run = 0.8f; f->run_dir = 0; f->run_t = 0.8f; f->slack_t = f->over_t = 0;
    f->jumping = 0; f->jump_ok = 0; f->jump_t = 0;
    f->fx = 0; f->fy = ly; f->fz = f->dist;
}

int fight_update(Fight *f, int rod, int reel, int tap, float dt) {
    const Species *S = &g_species[fish_species(f->fish)];
    const float kg = fish_kg(f->fish);
    const float pf = S->power * (0.55f + kg / 7.0f);          // how hard this fish pulls
    // A new run every second or so: strength, direction (-1 left, 0 straight away, 1 right).
    f->run_t -= dt;
    if (f->run_t <= 0) {
        f->run = (0.25f + rnd(75) / 100.0f) * (0.35f + 0.65f * f->stamina);
        f->run_dir = (float)(rnd(3) - 1);
        f->run_t = 0.6f + rnd(120) / 100.0f;
        // A strong run near the surface sometimes ends in a jump.
        if (!f->jumping && f->run > 0.55f && rnd(100) < 22) { f->jumping = 1; f->jump_t = 0.9f; f->jump_ok = 0; }
    }
    // Rod against the run: less strain and the fish tires; rod with it: the line takes it all.
    float k = 1.0f;
    if (f->run_dir != 0 && rod == (int)f->run_dir) k = 1.55f;
    if (f->run_dir != 0 && rod == -(int)f->run_dir) k = 0.6f;
    const float pull = f->run * pf;
    float target = pull * k * 0.55f + (reel ? 0.30f + pull * 0.35f : 0.0f);
    if (f->jumping) target += reel ? 0.25f : 0.0f;
    f->tension += (target - f->tension) * clampf(dt * 5.0f, 0, 1);
    // Line: reeling gains it (less against a strong run), a run takes it.
    if (reel) f->dist -= (190.0f - pull * 110.0f) * dt;
    f->dist += pull * 95.0f * dt;
    if (f->dist < 0) f->dist = 0;
    if (f->dist > 2200) f->dist = 2200;
    f->stamina -= dt * (0.04f + f->tension * 0.10f + (k < 1.0f ? 0.07f : 0.0f)) / (0.45f + kg / 9.0f);
    if (f->stamina < 0) f->stamina = 0;
    // Where the fish is, relative to the line (for the camera and the fish pose).
    f->fx += (f->run_dir * 230.0f - f->fx) * clampf(dt * 1.2f, 0, 1);
    f->fz = f->dist;
    f->fy += ((f->jumping ? SURF - 20 : S->depth) - f->fy) * clampf(dt * (f->jumping ? 4.0f : 1.0f), 0, 1);
    // Jumps: tap to lower the rod in time, or the fish shakes the hook.
    if (f->jumping) {
        if (tap) f->jump_ok = 1;
        f->jump_t -= dt;
        if (f->jump_t <= 0) {
            f->jumping = 0;
            if (!f->jump_ok) return -2;
        }
    }
    if (f->tension >= 1.0f) { f->over_t += dt; if (f->over_t > 0.45f) return -1; }
    else f->over_t = f->over_t > dt ? f->over_t - dt : 0;
    if (f->tension < 0.06f) { f->slack_t += dt; if (f->slack_t > 1.8f) return -2; }
    else f->slack_t = 0;
    if (f->dist < 70) return 1;
    return 0;
}
