// cars.c — Vertice GP: the cars (low-poly meshes built in code), arcade physics, AI drivers,
// contact, lap timing and the particle effects they kick up.
#include "game.h"

Car g_car[NCARS];

#define VMAX      1250.0f    // units/s (~200 km/h)
#define VMAX_OFF   480.0f    // on the grass
#define ACCEL      640.0f
#define BRAKE     1500.0f
#define LAPS 3

static const uint16_t kBody[NCARS] = { 0xE0C3 /* red */, 0x22DB /* blue */, 0x2D48 /* green */, 0xF5C2 /* yellow */ };
static int m_glass, m_tyre, m_rim, m_head;

// One car: nose toward +Z, length 124, width 58, wheels on the ground (y = 0).
static int build_car(int body, int tail) {
    const float cx = 0, cy = 22, cz = 0;
    mb_box(-29, 9, -62, 29, 28, 60, 0, 0, 16, body);                 // body, sloped nose
    mb_box(-23, 28, -30, 23, 47, 14, 4, 8, 18, m_glass);            // cabin / glasshouse
    mb_box(-19, 47, -22, 19, 48, -4, 0, 0, 0, body);                 // roof panel
    mb_box(-27, 34, -62, 27, 38, -52, 0, 0, 0, body);                // rear wing
    mb_box(-4, 28, -58, -2, 34, -55, 0, 0, 0, m_rim);                // wing posts
    mb_box(2, 28, -58, 4, 34, -55, 0, 0, 0, m_rim);
    for (int w = 0; w < 4; w++) {                                     // hexagonal wheels
        const float wx = (w & 1) ? 31 : -31, wz = (w & 2) ? 38 : -40, r = 12, hw = 6;
        int in[6], out[6];
        for (int k = 0; k < 6; k++) {
            const float a = k * PI_F / 3 + PI_F / 6;
            in[k] = mb_v(wx - hw, r + cosf_(a) * r, wz + sinf_(a) * r, 0, 0);
            out[k] = mb_v(wx + hw, r + cosf_(a) * r, wz + sinf_(a) * r, 0, 0);
        }
        for (int k = 0; k < 6; k++) mb_quad(in[k], in[(k + 1) % 6], out[(k + 1) % 6], out[k], m_tyre, wx, r, wz);
        int *cap = wx > 0 ? out : in;                                 // the outer hub
        for (int k = 1; k < 5; k++) mb_tri(cap[0], cap[k], cap[k + 1], m_rim, wx > 0 ? wx - 20 : wx + 20, r, wz);
    }
    for (int s = -1; s <= 1; s += 2) {                                // head and tail lights
        // On the sloped nose: the front face runs from (y 9, z 60) to (y 28, z 44); sit 0.8 proud.
        const float zl = 60.0f - (17 - 9) * 16.0f / 19.0f + 0.8f, zh = 60.0f - (23 - 9) * 16.0f / 19.0f + 0.8f;
        const int h0 = mb_v(s * 16, 17, zl, 0, 0), h1 = mb_v(s * 26, 17, zl, 0, 0);
        const int h2 = mb_v(s * 26, 23, zh, 0, 0), h3 = mb_v(s * 16, 23, zh, 0, 0);
        mb_quad(h0, h1, h2, h3, m_head, cx, cy, cz);
        const int t0 = mb_v(s * 14, 19, -62.5f, 0, 0), t1 = mb_v(s * 27, 19, -62.5f, 0, 0);
        const int t2 = mb_v(s * 27, 26, -62.5f, 0, 0), t3 = mb_v(s * 14, 26, -62.5f, 0, 0);
        mb_quad(t0, t1, t2, t3, tail, cx, cy, cz);
    }
    return mb_commit(body, 0);
}

void cars_build(void) {
    m_glass = vx_material(NV_RGB(40, 55, 80), VX_PHONG, 255, -1, 220);
    m_tyre = vx_material(NV_RGB(28, 28, 30), VX_FLAT, 255, -1, 0);
    m_rim = vx_material(NV_RGB(175, 180, 190), VX_GOURAUD, 255, -1, 0);
    m_head = vx_material(NV_RGB(255, 250, 215), VX_UNLIT, 255, -1, 0);
    for (int i = 0; i < NCARS; i++) {
        Car *c = &g_car[i];
        c->mat_body = vx_material(kBody[i], VX_PHONG, 255, -1, 160);
        c->mat_tail = vx_material(NV_RGB(120, 10, 10), VX_UNLIT, 255, -1, 0);
        c->obj = build_car(c->mat_body, c->mat_tail);
        c->ai = i != 0;
    }
}

static void place(Car *c) {
    c->seg = track_nearest(c->x, c->z, c->seg, TRACK_N / 2);
    track_frame(c->seg, c->x, c->z, &c->s, &c->lat);
}

void cars_grid(void) {
    for (int i = 0; i < NCARS; i++) {
        Car *c = &g_car[i];
        const float back = 150.0f + (i / 2) * 260.0f, lat = (i & 1) ? 70.0f : -70.0f;
        track_point(g_trk_len - back, lat, &c->x, &c->z, &c->heading);
        c->v = 0; c->steer = 0; c->seg = 0; c->lap = -1; c->finished = 0;
        c->lane = lat * 0.6f;
        c->skill = 0.90f + 0.03f * i;             // the leader-to-be starts at the back
        c->best_lap_ms = 0; c->last_lap_ms = 0; c->finish_ms = 0; c->bump_cool = 0;
        c->seg = TRACK_N - 1;
        place(c);
        c->total = c->s - g_trk_len;              // behind the line: slightly negative
        vx_obj_pos(c->obj, iroundf(c->x), 0, iroundf(c->z));
        vx_obj_rot(c->obj, 0, iroundf(deg(c->heading)), 0);
    }
}

float car_speed_kmh(int i) { return g_car[i].v * 0.162f; }

int car_position(int i) {
    int p = 1;
    for (int j = 0; j < NCARS; j++) {
        if (j == i) continue;
        const Car *a = &g_car[j], *b = &g_car[i];
        if (a->finished && b->finished ? a->finish_ms < b->finish_ms : a->total > b->total) p++;
    }
    return p;
}

// AI: steer toward a point ahead on the racing lane; slow for the curvature coming up; a little
// rubber band so the race stays close.
static void ai_drive(Car *c, Input *out) {
    float tx, tz;
    const float look = 320.0f + c->v * 0.42f;
    track_point(c->s + look, c->lane, &tx, &tz, 0);
    const float want = atan2f_(tx - c->x, tz - c->z);
    const float err = wrap_pi(want - c->heading);
    out->left = err < -0.03f; out->right = err > 0.03f;
    float h0, h1;
    float dummy_x, dummy_z;
    track_point(c->s + 250.0f, 0, &dummy_x, &dummy_z, &h0);
    track_point(c->s + 250.0f + 600.0f + c->v * 0.35f, 0, &dummy_x, &dummy_z, &h1);
    const float curv = fabsf_(wrap_pi(h1 - h0));
    float vt = VMAX * c->skill * (1.0f - clampf(curv * 0.75f, 0, 0.5f));
    const float gap = g_car[0].total - c->total;
    vt *= 1.0f + clampf(gap / 5000.0f, -0.07f, 0.10f);
    out->gas = c->v < vt;
    out->brake = c->v > vt + 90.0f;
    c->steer = clampf(err * 2.6f, -1, 1);
}

static void lap_check(Car *c, float prev_s, int now_ms) {
    const float d = c->s - prev_s;
    if (d < -g_trk_len / 2) {                 // crossed the line forwards
        c->lap++;
        if (c->lap >= 1 && !c->finished) {
            c->last_lap_ms = (float)(now_ms - c->lap_start_ms);
            if (c->best_lap_ms == 0 || c->last_lap_ms < c->best_lap_ms) c->best_lap_ms = c->last_lap_ms;
        }
        if (c->lap >= 0) c->lap_start_ms = (float)now_ms;
        if (c->lap >= LAPS && !c->finished) { c->finished = 1; c->finish_ms = (float)now_ms; }
    } else if (d > g_trk_len / 2) {
        c->lap--;                             // reversed over it
    }
    c->total = c->lap * g_trk_len + c->s;
}

void cars_update(const Input *in, float dt, int racing, int now_ms) {
    for (int i = 0; i < NCARS; i++) {
        Car *c = &g_car[i];
        Input ai = {0, 0, 0, 0};
        const Input *u = in;
        if (c->ai || c->finished) { ai_drive(c, &ai); u = &ai; }
        if (!racing) ai.gas = ai.brake = 0, u = &ai;
        if (!c->ai && !c->finished && racing) {          // player: smooth the digital steering
            const float target = (float)(in->right - in->left);
            c->steer += clampf(target - c->steer, -dt * 5.0f, dt * 5.0f);
        }
        const int off = fabsf_(c->lat) > ROAD_HW + 6;
        const float vmax = off ? VMAX_OFF : VMAX;
        if (u->gas) c->v += ACCEL * dt * (1.0f - c->v / (vmax + 1.0f));
        if (u->brake) c->v -= BRAKE * dt;
        if (!u->gas && !u->brake) c->v -= (70.0f + c->v * 0.18f) * dt;
        if (c->v > vmax) c->v -= (off ? 1400.0f : 300.0f) * dt;
        if (c->v < 0) c->v = 0;
        // Yaw rate grows with speed up to a point, then tightens: stable at 200 km/h.
        const float grip = clampf(c->v / 260.0f, 0, 1) * (1.0f - 0.38f * c->v / VMAX) * (off ? 0.75f : 1.0f);
        c->heading = wrap_pi(c->heading + c->steer * 2.3f * grip * dt);
        c->x += sinf_(c->heading) * c->v * dt;
        c->z += cosf_(c->heading) * c->v * dt;
        const float prev_s = c->s;
        place(c);
        // Invisible barrier far off the track: slide back, lose speed, sparks.
        const float lim = ROAD_HW + 700;
        if (fabsf_(c->lat) > lim) {
            float bx, bz;
            track_point(c->s, c->lat > 0 ? lim : -lim, &bx, &bz, 0);
            c->x = bx; c->z = bz; c->v *= 0.55f;
            vx_emit(g_fx_spark, iroundf(c->x), 20, iroundf(c->z), 0, 200, 0, 250, 12);
            place(c);
        }
        if (racing || c->finished) lap_check(c, prev_s, now_ms);
        // Effects.
        const float rx = c->x - sinf_(c->heading) * 50, rz = c->z - cosf_(c->heading) * 50;
        if (off && c->v > 150) vx_emit(g_fx_dust, iroundf(rx), 12, iroundf(rz), 0, 80, 0, 60, 1 + (c->v > 400));
        if (u->brake && c->v > 500) vx_emit(g_fx_smoke, iroundf(rx), 8, iroundf(rz), 0, 40, 0, 30, 1);
        vx_mat_color(c->mat_tail, (u->brake && c->v > 30) ? NV_RGB(255, 40, 30) : NV_RGB(120, 10, 10));
        c->bump_cool -= dt;
    }
    // Contact: push overlapping cars apart, scrub speed, sparks.
    for (int i = 0; i < NCARS; i++)
        for (int j = i + 1; j < NCARS; j++) {
            Car *a = &g_car[i], *b = &g_car[j];
            const float dx = b->x - a->x, dz = b->z - a->z, d2 = dx * dx + dz * dz, r = 78.0f;
            if (d2 >= r * r || d2 < 1e-3f) continue;
            const float d = sqrtf_(d2), push = (r - d) * 0.5f, nx = dx / d, nz = dz / d;
            a->x -= nx * push; a->z -= nz * push; b->x += nx * push; b->z += nz * push;
            const float va = a->v, vb = b->v;
            a->v = va * 0.9f + vb * 0.06f; b->v = vb * 0.9f + va * 0.06f;
            if (a->bump_cool <= 0 && b->bump_cool <= 0 && (va > 200 || vb > 200)) {
                vx_emit(g_fx_spark, iroundf(a->x + dx / 2), 26, iroundf(a->z + dz / 2), 0, 260, 0, 320, 18);
                if (!a->ai || !b->ai) nv_gfx_tone(180, 40);
                a->bump_cool = b->bump_cool = 0.3f;
            }
        }
    for (int i = 0; i < NCARS; i++) {
        Car *c = &g_car[i];
        vx_obj_pos(c->obj, iroundf(c->x), 0, iroundf(c->z));
        vx_obj_rot(c->obj, 0, iroundf(deg(c->heading)), 0);
    }
}
