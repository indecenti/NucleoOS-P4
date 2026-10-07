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
    { "STORIONE",      "STURGEON",        4.0f, 18.f,  30, 120, 1.9f, { 0.30f, 1.60f, 0.40f } },   // slow, on the bed, huge
    { "LUCCIO ALLIGATORE", "ALLIGATOR GAR", 3.0f, 15.f, 290, 230, 1.7f, { 1.00f, 0.50f, 1.40f } },   // high in the water, ambushes
};
const char *const g_lure_it[NLURES] = { "CRANKBAIT", "POPPER", "VERME", "JIG" };
const char *const g_lure_en[NLURES] = { "CRANKBAIT", "POPPER", "WORM", "JIG" };
// How much each species goes for each lure (crank, popper, worm, jig).
static const float s_aff[NSPECIES][NLURES] = {
    { 1.1f, 1.2f, 1.1f, 1.3f },   // bass: everything, jigs best
    { 1.3f, 0.8f, 0.7f, 0.8f },   // trout: moving baits
    { 1.3f, 1.1f, 0.6f, 0.9f },   // pike
    { 0.6f, 0.4f, 1.3f, 1.5f },   // catfish: on the bottom
    { 0.5f, 0.4f, 1.5f, 1.1f },   // carp: soft baits
    { 1.1f, 0.9f, 1.2f, 1.1f },   // perch
    { 1.1f, 0.5f, 0.9f, 1.5f },   // zander: jigs
    { 1.0f, 1.0f, 1.0f, 1.0f },   // gold
    { 0.3f, 0.1f, 1.6f, 1.4f },   // sturgeon: soft baits on the bottom
    { 1.3f, 1.5f, 0.4f, 0.6f },   // gar: fast and on the surface
};

#define PER_SP 3
#define NSLOT (NSPECIES * PER_SP)
typedef struct {
    int   obj, tail, species, active, state;   // body and tail segments; state 0 wander 1 follow 2 strike 3 flee
    float x, y, z, yaw, speed, interest, kg, hx, hz, t, wig, nib, ft, ft_need;   // ft: time following
    float pitch, wiggle, sc;              // the last pose (fish_mouth) and the size, 1 = 112 units
    float tail_a;                         // the tail's swing (rad), following the beat a little late
    float nib_total;                      // how long this fish's mouthing lasts (s)
    int   peck_n;                         // pecks so far (for the sound and the rumble)
} Fish;
static Fish s_fish[NSLOT];

// ---- models ------------------------------------------------------------------------------------------
// The painted fish (img/fish<N>.565) are the skins: art/fishtex.py packs them into one keyed atlas
// (img/fx.565) and samples each body's outline (fishprof.h). A fish is a smooth spindle of rings
// following that outline, skinned by side projection (both flanks show the painting), plus a keyed
// plane through its middle that carries the fins and the tail. Nose toward +Z, 112 units long;
// vx_obj_scale sizes each catch by its weight. Without the atlas: plain painted-colour bodies.
#include "fishprof.h"
static int s_fx_tex = -2;
static float s_mouth_y[NSPECIES];       // the snout's height in the model (the nose is at z = +55.4)
// Two segments, jointed at body ring FISH_J (a little behind the deepest part): the body with the
// head and the front fins, and the tail with the caudal fin, built around the joint so it swings
// there. The swing runs a little behind the beat and the body counter-sways: a swimming S.
#define FISH_J 4
static float s_joint_y[NSPECIES], s_joint_z[NSPECIES];   // the joint in the body model
static int fish_segment(int sp, int skin, int tail) {
    const FishSkin *S = &k_skin[sp];
    const float k = 112.0f / S->w;                                     // world units per texel
    const float wk = sp == SP_CATFISH ? 0.80f : sp == SP_CARP ? 0.55f : sp == SP_PIKE || sp == SP_GAR ? 0.55f : sp == SP_STURGEON ? 0.62f : 0.48f;   // girth
    const float fJ = S->st[FISH_J][0];
    const float zJ = (fJ - 0.5f) * S->w * k, yJ = (S->h * 0.5f - S->st[FISH_J][1]) * k;
    const float oy = tail ? yJ : 0, oz = tail ? zJ : 0;               // the tail is modelled around the joint
    // The body reaches one ring past the joint into the tail: the two overlap there, so no crack
    // opens (showing the water through the fish) when the tail swings.
    const int r0 = tail ? 0 : FISH_J - 1, r1 = tail ? FISH_J : FX_NR - 1;
    enum { NS = 10 };
    int ring[FX_NR][NS];
    for (int r = r0; r <= r1; r++) {
        const float f = S->st[r][0], m = S->st[r][1], hh = S->st[r][2];
        const float z = (f - 0.5f) * S->w * k, yc = (S->h * 0.5f - m) * k, Hh = hh * k, Wd = Hh * wk;
        for (int j = 0; j < NS; j++) {
            const float a = j * 2 * PI_F / NS, sa = sinf_(a);
            ring[r][j] = mb_v(cosf_(a) * Wd, yc + sa * Hh - oy, z - oz, iroundf((S->x + f * S->w) * FX_UVK), iroundf((S->y + m - sa * hh) * FX_UVK));
        }
    }
    for (int r = r0; r < r1; r++)
        for (int j = 0; j < NS; j++) {
            const float zc = ((S->st[r][0] + S->st[r + 1][0]) * 0.5f - 0.5f) * S->w * k;
            const float yc = (S->h * 0.5f - S->st[r][1]) * k;
            mb_quad(ring[r][j], ring[r][(j + 1) % NS], ring[r + 1][(j + 1) % NS], ring[r + 1][j], skin, 0, yc - oy, zc - oz);
        }
    if (!tail) {   // the snout, closed with a fan
        const float zn = (0.995f - 0.5f) * S->w * k, mn = S->st[FX_NR - 1][1];
        s_mouth_y[sp] = (S->h * 0.5f - mn) * k;
        s_joint_y[sp] = yJ; s_joint_z[sp] = zJ;
        const int nose = mb_v(0, (S->h * 0.5f - mn) * k, zn, iroundf((S->x + 0.995f * S->w) * FX_UVK), iroundf((S->y + mn) * FX_UVK));
        for (int j = 0; j < NS; j++)
            mb_tri(ring[FX_NR - 1][j], ring[FX_NR - 1][(j + 1) % NS], nose, skin, 0, (S->h * 0.5f - mn) * k, zn - 20);
    } else {       // the tail root
        const float m0 = S->st[0][1], z0 = (S->st[0][0] - 0.5f) * S->w * k;
        const int root = mb_v(0, (S->h * 0.5f - m0) * k - oy, z0 - 2 - oz, iroundf((S->x + S->st[0][0] * S->w) * FX_UVK), iroundf((S->y + m0) * FX_UVK));
        for (int j = 0; j < NS; j++)
            mb_tri(ring[0][j], ring[0][(j + 1) % NS], root, skin, 0, (S->h * 0.5f - m0) * k - oy, z0 + 20 - oz);
    }
    {   // fins: the painting on a plane through the middle (both faces), cut at the joint
        const float fO = S->st[FISH_J - 1][0], zO = (fO - 0.5f) * S->w * k;      // the overlap starts here
        const float za = tail ? -0.5f * S->w * k : zO, zb = tail ? zJ : 0.5f * S->w * k;
        const float y0 = 0.5f * S->h * k, y1 = -0.5f * S->h * k;
        const int ua = iroundf((S->x + (tail ? 0.0f : fO) * S->w) * FX_UVK), ub = iroundf((S->x + (tail ? fJ : 1.0f) * S->w) * FX_UVK);
        const int v0 = S->y * FX_UVK, v1 = (S->y + S->h) * FX_UVK;
        for (int side = -1; side <= 1; side += 2) {
            const int a = mb_v(0, y0 - oy, za - oz, ua, v0), b = mb_v(0, y0 - oy, zb - oz, ub, v0);
            const int c = mb_v(0, y1 - oy, zb - oz, ub, v1), d = mb_v(0, y1 - oy, za - oz, ua, v1);
            mb_quad(a, b, c, d, skin, -side * 10.0f, 0, 0);
        }
    }
    return mb_commit_ex(skin, 1, VX_MESH_SMOOTH);
}
static int build_fish(int sp, int *tail) {
    static const uint16_t pal[NSPECIES] = { C565(112, 142, 72), C565(196, 198, 206), C565(126, 150, 70), C565(92, 82, 70),
                                            C565(176, 140, 62), C565(186, 186, 80), C565(150, 160, 150), C565(255, 204, 44),
                                            C565(120, 116, 104), C565(110, 104, 70) };
    if (s_fx_tex == -2) s_fx_tex = vx_texture_load("fx", VX_TEX_KEY | VX_TEX_CLAMP);
    const int skin = s_fx_tex >= 0 ? vx_material(0xFFFF, VX_GOURAUD, 255, s_fx_tex, sp == SP_GOLD ? 160 : 70)
                                   : vx_material(pal[sp], VX_GOURAUD, 255, -1, 60);
    const int body = fish_segment(sp, skin, 0);
    *tail = fish_segment(sp, skin, 1);
    return body;
}

// Pose both segments: the body at (x, y, z) heading yaw (pitch < 0: nose up); beat is the swim's
// swing this instant (rad): the tail follows it a little late and wide, the body leans against it.
static void fish_place(Fish *f, float x, float y, float z, float yaw, float pitch, float beat) {
    f->tail_a += (clampf(beat * 1.5f, -0.45f, 0.45f) - f->tail_a) * 0.45f;   // lively, but never a visible bend
    const float body_yaw = yaw - beat * 0.3f;
    f->x = x; f->y = y; f->z = z; f->pitch = pitch; f->wiggle = body_yaw - yaw;
    vx_obj_pos(f->obj, iroundf(x), iroundf(y), iroundf(z));
    vx_obj_rot(f->obj, iroundf(deg(pitch)), iroundf(deg(body_yaw)), 0);
    const float sc = f->sc > 0 ? f->sc : 1.0f, lz = s_joint_z[f->species] * sc, ly = s_joint_y[f->species] * sc;
    const float cp = cosf_(pitch), sp = sinf_(pitch);
    const float fwd = lz * cp + ly * sp, up = -lz * sp + ly * cp;      // same convention as fish_mouth
    vx_obj_pos(f->tail, iroundf(x + sinf_(body_yaw) * fwd), iroundf(y + up), iroundf(z + cosf_(body_yaw) * fwd));
    vx_obj_rot(f->tail, iroundf(deg(pitch)), iroundf(deg(yaw + f->tail_a)), 0);
}
static void fish_show(Fish *f, int on) { vx_obj_show(f->obj, on); vx_obj_show(f->tail, on); }
static void fish_size(Fish *f, int pct) { vx_obj_scale(f->obj, pct); vx_obj_scale(f->tail, pct); f->sc = pct / 100.0f; }

void fish_build(void) {
    s_fx_tex = -2;                        // vx_reset dropped the old atlas: load it again
    for (int sp = 0; sp < NSPECIES; sp++) {
        int tproto;
        const int proto = build_fish(sp, &tproto);
        for (int k = 0; k < PER_SP; k++) {
            Fish *f = &s_fish[sp * PER_SP + k];
            f->obj = k ? vx_clone(proto) : proto;
            f->tail = k ? vx_clone(tproto) : tproto;
            if (f->obj < 0 || f->tail < 0) nv_log(NV_LOG_WARN, "bass: out of scene objects for the fish");
            f->species = sp; f->active = 0; f->tail_a = 0;
            fish_show(f, 0);
        }
    }
}

void fish_hide(void) {
    for (int i = 0; i < NSLOT; i++) { s_fish[i].active = 0; fish_show(&s_fish[i], 0); }
}

static int pick_species(int stage, int spot_kind) {
    int w[NSPECIES];
    for (int s = 0; s < NSPECIES; s++) w[s] = g_stage[stage].mix[s];
    // Structure matters: weeds and pads hold bass and pike, logs bass and catfish, rocks trout.
    if (spot_kind == SPOT_WEEDS) { w[SP_BASS] *= 2; w[SP_PIKE] *= 2; w[SP_PERCH] *= 2; }
    if (spot_kind == SPOT_PADS) { w[SP_BASS] *= 2; w[SP_CARP] *= 3; w[SP_GAR] *= 2; }
    if (spot_kind == SPOT_LOG) { w[SP_BASS] *= 2; w[SP_CATFISH] *= 3; w[SP_CARP] *= 2; }
    if (spot_kind == SPOT_ROCKS) { w[SP_TROUT] *= 3; w[SP_ZANDER] *= 2; w[SP_PERCH] *= 2; w[SP_STURGEON] *= 2; }
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
    const int n = spot >= 0 ? 4 + rnd(3) : 2 + rnd(2);
    for (int i = 0; i < n; i++) {
        const int sp = pick_species(stage, kind);
        int slot = -1;
        for (int k = 0; k < PER_SP && slot < 0; k++) if (!s_fish[sp * PER_SP + k].active) slot = sp * PER_SP + k;
        if (slot < 0) continue;
        Fish *f = &s_fish[slot];
        const Species *S = &g_species[sp];
        // Most fish are small; a giant is a rare event (about 1 in 25 gets the top fifth of the range).
        float r = rnd(1000) / 1000.0f;
        r = r * r * r * r * r * 0.7f;                              // most are small fry
        if (rnd(100) < 2) r = 0.75f + rnd(250) / 1000.0f;          // a monster: rare
        f->kg = S->kg_min + (S->kg_max - S->kg_min) * r;
        f->active = 1; f->state = 0; f->interest = 0; f->t = rnd(1000) / 100.0f;
        f->hx = cx + rnd(400) - 200; f->hz = cz + rnd(400) - 200;
        f->x = f->hx; f->z = f->hz; f->y = clampf(S->depth + g_depth_bias + rnd(80) - 40, 30, SURF - 30);
        f->yaw = rnd(628) / 100.0f; f->speed = S->speed * 0.3f;
        fish_size(f, iroundf(52 + f->kg * 17 > 220 ? 220 : 52 + f->kg * 17));
        fish_show(f, 1);
    }
}

int fish_species(int i) { return s_fish[i].species; }
// For the HUD's "?" / "!" marks: 0 none, 1 noticed the lure, 2 chasing it.
int fish_mark(int i, float *x, float *y, float *z) {
    const Fish *f = &s_fish[i];
    if (!f->active || f->state >= 3) return 0;
    *x = f->x; *y = f->y + 40; *z = f->z;
    if (f->state == 1 || f->state == 2) return 2;
    return f->interest > 0.12f ? 1 : 0;
}
int fish_slots(void) { return NSLOT; }
// The lake's conditions move the fish up or down (the hour, the light, the water's warmth).
float g_depth_bias;
int fish_any_interest(void) {
    for (int i = 0; i < NSLOT; i++) if (s_fish[i].active && s_fish[i].state < 3 && s_fish[i].interest > 0.12f) return 1;
    return 0;
}
float fish_mark_x(int i) { return s_fish[i].x; }
float fish_mark_z(int i) { return s_fish[i].z; }
int g_fish_peck;                          // set on the frame a mouthing fish pecks (main clears it)
// The fish to frame in the bite: the one mouthing the lure, else the closest one following it.
int fish_watch(float lx, float ly, float lz, float *x, float *y, float *z) {
    int best = -1;
    float bd = 340.0f * 340.0f;
    for (int i = 0; i < NSLOT; i++) {
        const Fish *f = &s_fish[i];
        if (!f->active) continue;
        if (f->state == 2) { best = i; break; }
        if (f->state != 1) continue;
        const float dx = f->x - lx, dy = f->y - ly, dz = f->z - lz, d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < bd) { bd = d2; best = i; }
    }
    if (best >= 0) { *x = s_fish[best].x; *y = s_fish[best].y; *z = s_fish[best].z; }
    return best;
}
int fish_nibbling(void) {
    for (int i = 0; i < NSLOT; i++) if (s_fish[i].active && s_fish[i].state == 2) return i;
    return -1;
}
static int nibbling(void) { return fish_nibbling() >= 0; }
void fish_spook(int i) { if (i >= 0 && i < NSLOT) { s_fish[i].state = 3; s_fish[i].interest = 0; } }
float fish_kg(int i) { return s_fish[i].kg; }

void fish_pose(int i, float x, float y, float z, float yaw, float wiggle, float pitch) {
    Fish *f = &s_fish[i];
    f->yaw = yaw;
    fish_place(f, x, y, z, yaw, pitch, wiggle);
}

// Where the hook sits: the corner of the mouth, at the snout of the posed, scaled model (the line
// is tied here in the fight, not to the middle of the body).
void fish_mouth(int i, float *x, float *y, float *z) {
    const Fish *f = &s_fish[i];
    const float sc = f->sc > 0 ? f->sc : 1.0f;
    const float len = 52.0f * sc, my = s_mouth_y[f->species] * sc;    // just behind the very tip
    const float a = f->yaw + f->wiggle, cp = cosf_(f->pitch), sp = sinf_(f->pitch);
    // vx_obj_rot pitch: a negative angle lifts the nose (the jump climbs with pitch < 0)
    const float fwd = len * cp + my * sp, up = -len * sp + my * cp;
    *x = f->x + sinf_(a) * fwd;
    *z = f->z + cosf_(a) * fwd;
    *y = f->y + up;
}

void fish_pose_test(int i, float x, float y, float z, float yaw) {
    fish_show(&s_fish[i], 1);
    fish_size(&s_fish[i], 100);
    fish_pose(i, x, y, z, yaw, sinf_(yaw * 9.0f) * 0.25f, 0);   // swimming in place
}

// Hooked (keep >= 0): the others are gone at once, the fight is one fish's. Otherwise they bolt.
void fish_release_others(int keep) {
    for (int i = 0; i < NSLOT; i++)
        if (i != keep && s_fish[i].active) {
            if (keep >= 0) { s_fish[i].active = 0; fish_show(&s_fish[i], 0); }
            else s_fish[i].state = 3;
        }
}

// The hunt. A fish that sees the lure (in front, within ~5 m, near its depth) gets interested at a
// rate set by how the lure moves (its species' taste); interest decays when the lure does something
// it dislikes. Interested fish follow; a keen one close behind strikes.
int fish_update(const LureState *l, float dt, int now_ms) {
    int striker = -1;
    const int nibbler = fish_nibbling();
    for (int i = 0; i < NSLOT; i++) {
        Fish *f = &s_fish[i];
        if (!f->active) continue;
        const Species *S = &g_species[f->species];
        f->t += dt;
        const float dx = l->lx - f->x, dy = l->ly - f->y, dz = l->lz - f->z;
        const float d = sqrtf_(dx * dx + dy * dy + dz * dz);
        float tx, ty, tz, spd;
        if (f->state == 2) {                                  // nibbling: nose on the lure, pecking
            // Pecks every 0.55 s (a dart of the nose at the lure); somewhere past the middle it backs
            // off for a moment, as if it had lost interest, then comes back to it.
            const float e = f->nib_total - f->nib;
            const float b0 = f->nib_total * 0.45f, bk = e > b0 && e < b0 + 0.9f ? sinf_((e - b0) / 0.9f * PI_F) : 0.0f;
            // the nose (55 units ahead of the centre at scale 1) just touching the lure's tail
            const float hold = 55.0f * (f->sc > 0 ? f->sc : 1.0f) + g_lure_half + 3 + 60 * bk;
            const float bx = sinf_(f->yaw), bz = cosf_(f->yaw);
            f->x += ((l->lx - bx * hold) - f->x) * clampf(dt * 10, 0, 1);
            f->z += ((l->lz - bz * hold) - f->z) * clampf(dt * 10, 0, 1);
            f->y += (l->ly - f->y) * clampf(dt * 10, 0, 1);
            const float want = atan2f_(l->lx - f->x, l->lz - f->z);
            f->yaw = wrap_pi(f->yaw + clampf(wrap_pi(want - f->yaw), -dt * 5, dt * 5));
            const int cyc = (int)(e / 0.55f);
            const float ph = e / 0.55f - cyc;
            if (cyc != f->peck_n && bk < 0.05f) { f->peck_n = cyc; g_fish_peck = 1; }
            const float peck = bk < 0.05f && ph < 0.3f ? sinf_(ph / 0.3f * PI_F) * 8 : 0.0f;   // a short dart
            const float px = f->x, pz = f->z;                 // pecking: the nose bobs, the tail fans to hold
            fish_place(f, px + bx * peck, f->y, pz + bz * peck, f->yaw, 0, sinf_(now_ms * 0.018f) * 0.07f);
            f->x = px; f->z = pz;
            f->nib -= dt;
            if (f->nib <= 0 && striker < 0) { striker = i; f->state = 4; }
            continue;
        }
        if (f->state == 4) continue;                          // striking: main poses it
        if (f->state == 3) {                                  // spooked: bolt away and vanish
            tx = f->x - dx * 4; ty = f->y; tz = f->z - dz * 4; spd = S->speed * 1.6f;
            if (d > 900) { f->active = 0; fish_show(f, 0); continue; }
        } else {
            const float ahead = sinf_(f->yaw) * dx + cosf_(f->yaw) * dz;   // lure in front of it?
            const float depthk = 1.0f - clampf(fabsf_(l->ly - clampf(S->depth + g_depth_bias, 30, SURF - 40)) / 260.0f, 0, 0.75f);
            const float sees = (d < 700 && (ahead > -80 || d < 220)) ? 1.0f : 0.0f;
            const float like = S->like[l->action] * s_aff[f->species][l->lure];
            f->interest += dt * sees * depthk * (like - 0.35f) * (d < 300 ? 2.6f : 1.8f);   // arcade: keen fish
            if (!sees) f->interest -= dt * 0.25f;
            f->interest = clampf(f->interest, 0, 2.0f);
            if (f->interest > 0.35f) { if (f->state != 1) { f->state = 1; f->ft = 0; f->ft_need = 1.0f + rnd(160) / 100.0f; } }
            else if (f->state == 1) f->state = 0;
            if (f->state == 1) f->ft += dt;
            const int crowd = f->state == 1 && nibbler >= 0 && nibbler != i;   // another fish has the lure
            if (crowd) f->interest -= dt * 0.45f;               // ...and soon goes back to its business
            if (f->state == 1) {                              // follow a little behind the lure
                const float back = crowd ? 320.0f : 70 - f->interest * 30;
                const float lx = l->lx + (f->x - l->lx) * back / (d + 1), lz = l->lz + (f->z - l->lz) * back / (d + 1);
                tx = lx; ty = l->ly; tz = lz;
                spd = S->speed * (0.7f + f->interest * 0.5f) + 170;   // arcade: a chaser always catches up
                // Predators (pike, zander, bass) ambush: a dash when the lure passes close.
                if ((f->species == SP_PIKE || f->species == SP_ZANDER || f->species == SP_BASS) && d < 240 && d > 90) spd *= 1.6f;
                // Close and keen: it starts mouthing the lure (the "touch" before the bite).
                // It follows a while first (Fisherman's Bait: you watch it come), then mouths the lure.
                if (!crowd && d < 70 && f->interest > 0.75f && f->ft > f->ft_need && !nibbling()) {
                    f->state = 2;                             // mouthing: a long, nervy taste (3-6 s)
                    f->nib = f->nib_total = 3.0f + rnd(300) / 100.0f;
                    f->peck_n = 0;
                }
            } else {                                          // cruise around home, pausing to hover
                const float a = f->t * 0.35f + i;
                tx = f->hx + sinf_(a) * 160; ty = clampf(S->depth + g_depth_bias, 30, SURF - 40) + sinf_(f->t * 0.5f) * 40; tz = f->hz + cosf_(a * 0.8f) * 160;
                const float phase = sinf_(f->t * 0.4f + i * 1.7f);
                spd = S->speed * (phase > 0.4f ? 0.08f : 0.3f);  // idles for a while, then moves on
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
        for (int j = 0; j < NSLOT; j++) {                   // keep a fish's length from the others
            const Fish *o = &s_fish[j];
            if (j == i || !o->active) continue;
            const float sx = f->x - o->x, sz = f->z - o->z, s2 = sx * sx + sz * sz;
            if (s2 < 70 * 70 && s2 > 1) { const float k = (70 - sqrtf_(s2)) * 0.5f / sqrtf_(s2); f->x += sx * k; f->z += sz * k; }
        }
        {   // fish don't swim through rocks or logs either
            float fx = f->x, fy = f->y, fz = f->z;
            if (lake_collide(&fx, &fy, &fz, 16)) { f->x = fx; f->y = fy; f->z = fz; }
        }
        f->wig = sinf_(now_ms * 0.012f * (0.6f + f->speed / 200) + i) * (0.05f + f->speed / 1600);   // the swim beat
        fish_place(f, f->x, f->y, f->z, f->yaw, 0, f->wig);
    }
    return striker;
}

int g_rod_lift;
// ---- the fight -----------------------------------------------------------------------------------------
void fight_start(Fight *f, int fish, float lx, float ly, float lz) {
    f->fish = fish;
    f->dist = sqrtf_(lx * lx + lz * lz);
    f->tension = 0.4f; f->stamina = 1.0f;
    f->run = 0.8f; f->run_dir = 0; f->run_t = 0.8f; f->slack_t = f->over_t = 0;
    f->jumping = 0; f->jump_ok = 0; f->jump_t = 0; f->surge = 0; f->drag = 0; f->strain = 0;
    f->fx = 0; f->fy = ly; f->fz = f->dist;
    const float kg = fish_kg(fish);
    f->bolts = kg < 1.0f ? 1 : kg < 3.0f ? 2 : 3;            // big fish come back more often
    f->winds = kg < 1.0f ? 1 : kg < 3.0f ? 2 : 3;
    f->bolt_now = 0; f->tired = 0;
}

int fight_update(Fight *f, int rod, float reel, int tap, float dt) {
    const Species *S = &g_species[fish_species(f->fish)];
    const float kg = fish_kg(f->fish);
    const float pf = S->power * (0.55f + kg / 7.0f);          // how hard this fish pulls
    // A new run every second or so: strength, direction (-1 left, 0 straight away, 1 right).
    f->run_t -= dt;
    if (f->run_t <= 0) {
        f->run = (0.25f + rnd(75) / 100.0f) * (0.45f + 0.55f * f->stamina);   // a tired fish still pulls
        f->run_dir = (float)(rnd(3) - 1);
        f->run_t = 0.45f + rnd(90) / 100.0f;
        // A strong run near the surface sometimes ends in a jump.
        // Bass, trout, pike, perch and the golden bass leap; catfish, carp and zander bore deep.
        const int sp = fish_species(f->fish);
        const int leaper = sp != SP_CATFISH && sp != SP_CARP && sp != SP_ZANDER;
        if (leaper && !f->jumping && f->run > 0.5f && f->stamina > 0.15f && f->dist > 260 && rnd(100) < 30) {
            f->jumping = 1; f->jump_t = JUMP_T; f->jump_ok = 0;
        }
        // A sudden hard run: the line takes a jolt (let go of the reel!).
        if (f->run > 0.7f) { f->tension += 0.1f * pf; f->surge = 0.4f; }
    }
    f->surge = f->surge > dt ? f->surge - dt : 0;
    // Boat-side bolt (the classic last dash): a fish brought close sees the boat and runs again,
    // hard, with some of its strength back. Ease off (stop reeling, give line) or the line goes.
    f->bolt_now = 0;
    if (f->bolts > 0 && f->dist < 420 && f->dist > 120 && f->stamina < 0.55f && rnd(1000) < (int)(dt * 900)) {
        f->bolts--;
        f->bolt_now = 1;
        f->stamina += 0.3f + kg * 0.03f;
        if (f->stamina > 0.85f) f->stamina = 0.85f;
        f->run = 1.0f; f->run_dir = (float)(rnd(3) - 1); f->run_t = 1.2f + rnd(60) / 100.0f;
        f->tension += 0.18f * pf; f->surge = 0.6f;
    }
    // Rod against the run: less strain and the fish tires; rod with it: the line takes it all.
    float k = 1.0f;
    if (f->run_dir != 0 && rod == (int)f->run_dir) k = 1.7f;
    if (f->run_dir != 0 && rod == -(int)f->run_dir) k = 0.55f;
    const float pull = f->run * pf;
    float target = pull * k * 0.62f + (0.30f + pull * 0.4f) * reel;
    if (g_rod_lift > 0) target += 0.12f + pull * 0.2f;         // rod high: pressure on the fish
    if (g_rod_lift < 0) target *= 0.45f;                       // rod dropped: the line eases
    if (f->jumping) target += 0.25f * reel;
    // The drag (as on a real reel): when you stop cranking, the spool lets line go before it can
    // break — the fish takes line instead. Only cranking (or the rod held high) into a hard pull snaps it.
    f->drag = 0;
    if (reel < 0.15f && g_rod_lift <= 0 && target > 0.9f) {
        f->dist += (target - 0.9f) * 260.0f * dt;
        target = 0.9f;
        f->drag = 1;
    }
    f->tension += (target - f->tension) * clampf(dt * (target > f->tension ? 3.0f : 6.0f), 0, 1);   // rises slower than it eases
    // Line: reeling gains it (less against a strong run), a run takes it.
    if (reel > 0 && g_rod_lift >= 0) f->dist -= (130.0f - pull * 105.0f) * (g_rod_lift > 0 ? 0.8f : 1.0f) * reel * dt;
    f->dist += pull * 115.0f * dt * (g_rod_lift < 0 ? 1.4f : 1.0f);
    if (f->dist < 0) f->dist = 0;
    if (f->dist > 2200) f->dist = 2200;
    // Stamina: pressure tires it (the square of the tension: keep it high, short of the red), the
    // rod against its run tires it more; a slack line lets it get its breath back. Heavier fish last
    // longer: a ~1 kg trout gives ~15 s of good play, a 5 kg bass half a minute or more.
    f->stamina -= dt * (0.02f + f->tension * f->tension * 0.12f + (k < 1.0f ? 0.04f : 0.0f) +
                        (g_rod_lift > 0 ? 0.03f : 0.0f)) / (0.5f + kg / 4.0f);
    // It gets its breath back all the time, the less the line holds it the faster: keep it under
    // pressure or a worn-out fish recovers.
    f->stamina += dt * 0.035f * clampf(1.0f - f->tension * 2.0f, 0, 1);   // none from half tension up
    f->stamina = clampf(f->stamina, 0, 1);
    // Second wind: a fish that was worn out and has got some strength back picks its moment — you
    // ease off, or just when you think it's done — and charges again.
    if (f->stamina < 0.12f) f->tired = 1;
    if (f->tired && f->winds > 0 && f->stamina > 0.28f && !f->jumping &&
        (reel < 0.1f || rnd(1000) < (int)(dt * 700))) {
        f->winds--; f->tired = 0; f->bolt_now = 2;
        f->stamina += 0.15f;
        f->run = 1.0f; f->run_dir = (float)(rnd(3) - 1); f->run_t = 1.4f + rnd(60) / 100.0f;
        f->tension += 0.15f * pf; f->surge = 0.6f;
    }
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
    if (f->tension > 0.92f) f->strain += dt * (0.3f + (f->tension - 0.92f) * 3.0f);   // ~2 s of red to break
    else f->strain -= dt * 0.6f;
    f->strain = clampf(f->strain, 0, 1);
    if (f->strain >= 1.0f) return -1;
    f->over_t = 0;
    if (f->tension < 0.06f) { f->slack_t += dt; if (f->slack_t > 2.0f) return -2; }   // slack: the hook falls out
    else f->slack_t = 0;
    if (f->dist < 70) return 1;
    return 0;
}
