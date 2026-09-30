// cars.c — Vertice GP: karts (low-poly models built in code, blob shadows), arcade physics with
// reverse, drift mini-turbo, boost pads and coins, AI drivers, contact, soft walls, respawn and
// lap timing.
#include "game.h"

Car g_car[NCARS];
const char *g_msg = "";
int g_msg_until = 0;

#define VMAX      1250.0f    // units/s (~200 km/h)
#define VMAX_OFF   430.0f    // on the grass
#define VREV      -330.0f    // reverse
#define ACCEL      920.0f
#define BRAKE     1600.0f
#define BOOST_K     1.38f    // top speed multiplier while boosting

static const uint16_t kBody[NCARS] = { 0xE0C3 /* red */, 0x22DB /* blue */, 0x2D48 /* green */, 0xFDA0 /* orange */ };
static int m_tyre, m_rim, m_dark, m_visor, m_suit, m_shadow, m_metal;

// ---- the kart model: nose toward +Z, wheels on the ground (y = 0) -----------------------------------
// Three levels of detail, swapped by the engine by distance (vx_obj_lod): the full kart up close
// (~200 triangles), a blocky one at mid range (~70), a slab with wheels far away (~30). Bottom faces
// are never built: the camera is always above the karts.
static void wheel(float wx, float wz, float r, float hw, int seg) {
    int in[8], out[8];
    for (int k = 0; k < seg; k++) {
        const float a = k * 2 * PI_F / seg + PI_F / seg;
        in[k] = mb_v(wx - hw, r + cosf_(a) * r, wz + sinf_(a) * r, 0, 0);
        out[k] = mb_v(wx + hw, r + cosf_(a) * r, wz + sinf_(a) * r, 0, 0);
    }
    for (int k = 0; k < seg; k++) mb_quad(in[k], in[(k + 1) % seg], out[(k + 1) % seg], out[k], m_tyre, wx, r, wz);
    int *cap = wx > 0 ? out : in;                                  // the outer hub only
    for (int k = 1; k < seg - 1; k++) mb_tri(cap[0], cap[k], cap[k + 1], m_rim, wx > 0 ? wx - 30 : wx + 30, r, wz);
}

static int build_kart(int body, int tail, int lod) {
    mb_no_bottom = 1;
    if (lod == 2) {                                                // far: slab, helmet, wheel blocks
        mb_box(-34, 6, -58, 34, 22, 60, 6, 8, 16, body);
        mb_box(-9, 22, -22, 9, 46, -4, 2, 2, 3, body);
        mb_box(-42, 0, -46, -28, 30, -26, 0, 0, 0, m_tyre); mb_box(28, 0, -46, 42, 30, -26, 0, 0, 0, m_tyre);
        mb_box(-39, 0, 30, -28, 24, 50, 0, 0, 0, m_tyre);  mb_box(28, 0, 30, 39, 24, 50, 0, 0, 0, m_tyre);
    } else if (lod == 1) {                                         // mid: the silhouette in boxes
        mb_box(-30, 7, -46, 30, 15, 44, 0, 0, 0, body);
        mb_box(-19, 9, 24, 19, 24, 64, 5, 0, 20, body);
        mb_box(-11, 15, -26, 11, 40, -6, 2, 0, 3, m_suit);
        mb_box(-10, 38, -22, 10, 54, -3, 3, 3, 4, body);
        mb_box(-36, 26, -66, 36, 34, -54, 0, 0, 0, body);
        wheel(-41, -36, 16, 7, 5); wheel(41, -36, 16, 7, 5);
        wheel(-39, 40, 12, 6, 5); wheel(39, 40, 12, 6, 5);
    } else {
        mb_box(-30, 7, -46, 30, 15, 44, 0, 0, 0, body);           // floor tray
        mb_box(-19, 9, 24, 19, 24, 62, 5, 0, 20, body);            // sloped nose
        mb_box(-37, 7, -22, -29, 21, 22, 0, 4, 4, body);           // side pods
        mb_box(29, 7, -22, 37, 21, 22, 0, 4, 4, body);
        mb_box(-34, 9, 58, 34, 15, 66, 0, 0, 0, m_dark);           // front bumper
        mb_box(-13, 15, -32, 13, 42, -22, 0, 0, 0, m_dark);        // seat back
        mb_box(-11, 15, -24, 11, 38, -6, 2, 0, 3, m_suit);         // driver torso
        mb_box(-10, 38, -22, 10, 54, -3, 3, 3, 4, body);           // helmet
        const float vz = -3.0f;                                      // visor on the helmet front
        const int v0 = mb_v(-7, 42, vz + 0.6f, 0, 0), v1 = mb_v(7, 42, vz + 0.6f, 0, 0);
        const int v2 = mb_v(6, 50, vz - 3.2f, 0, 0), v3 = mb_v(-6, 50, vz - 3.2f, 0, 0);
        mb_quad(v0, v1, v2, v3, m_visor, 0, 46, -14);
        mb_box(-15, 13, -60, 15, 29, -44, 0, 0, 0, m_metal);       // engine block
        mb_box(-10, 22, -64, -5, 34, -58, 0, 0, 0, m_dark);        // exhausts
        mb_box(5, 22, -64, 10, 34, -58, 0, 0, 0, m_dark);
        mb_box(-36, 30, -66, 36, 34, -54, 0, 0, 0, body);          // rear wing
        for (int s = -1; s <= 1; s += 2) {                         // tail lights on the wing edge
            const int t0 = mb_v(s * 22, 30, -66.6f, 0, 0), t1 = mb_v(s * 34, 30, -66.6f, 0, 0);
            const int t2 = mb_v(s * 34, 34, -66.6f, 0, 0), t3 = mb_v(s * 22, 34, -66.6f, 0, 0);
            mb_quad(t0, t1, t2, t3, tail, 0, 32, 0);
        }
        wheel(-41, -36, 16, 7, 8); wheel(41, -36, 16, 7, 8);       // fat rear wheels
        wheel(-39, 40, 12, 6, 7); wheel(39, 40, 12, 6, 7);
    }
    mb_no_bottom = 0;
    return mb_commit(body, 0);
}

// Blob shadow: a soft-cornered dark octagon laid on the ground, alpha-blended over road and grass.
static int build_shadow(void) {
    int ring[8];
    for (int k = 0; k < 8; k++) {
        const float a = k * PI_F / 4 + PI_F / 8;
        ring[k] = mb_v(cosf_(a) * 50, 0, sinf_(a) * 78, 0, 0);
    }
    for (int k = 1; k < 7; k++) mb_tri(ring[0], ring[k], ring[k + 1], m_shadow, 0, -1000, 0);
    const int id = mb_commit(m_shadow, 0);
    vx_obj_depth(id, 0, VX_DEPTH_NOTEST | VX_DEPTH_NOWRITE);          // painter's background band
    return id;
}

void cars_build(void) {
    m_tyre = vx_material(NV_RGB(30, 30, 34), VX_FLAT, 255, -1, 0);
    m_rim = vx_material(NV_RGB(200, 204, 214), VX_GOURAUD, 255, -1, 0);
    m_dark = vx_material(NV_RGB(44, 46, 54), VX_FLAT, 255, -1, 0);
    m_visor = vx_material(NV_RGB(30, 40, 60), VX_UNLIT, 255, -1, 0);
    m_suit = vx_material(NV_RGB(240, 240, 244), VX_GOURAUD, 255, -1, 0);
    m_metal = vx_material(NV_RGB(150, 154, 166), VX_GOURAUD, 255, -1, 0);
    m_shadow = vx_material(NV_RGB(0, 0, 0), VX_UNLIT, 120, -1, 0);
    for (int i = 0; i < NCARS; i++) {
        Car *c = &g_car[i];
        c->mat_body = vx_material(kBody[i], VX_GOURAUD, 255, -1, 0);
        c->mat_tail = vx_material(NV_RGB(120, 10, 10), VX_UNLIT, 255, -1, 0);
        c->shadow = build_shadow();
        c->obj = build_kart(c->mat_body, c->mat_tail, 0);
        // Level of detail: the engine swaps in the simpler karts by camera distance.
        vx_obj_lod(c->obj, build_kart(c->mat_body, c->mat_tail, 1), 950);
        vx_obj_lod(c->obj, build_kart(c->mat_body, c->mat_tail, 2), 2300);
        c->ai = i != 0;
    }
}

static void place(Car *c) {
    c->seg = track_nearest(c->x, c->z, c->seg, 4);
    track_frame(c->seg, c->x, c->z, &c->s, &c->lat);
}

void cars_grid(void) {
    for (int i = 0; i < NCARS; i++) {
        Car *c = &g_car[i];
        const float back = 150.0f + (i / 2) * 260.0f, lat = (i & 1) ? 70.0f : -70.0f;
        track_point(g_trk_len - back, lat, &c->x, &c->z, &c->heading);
        c->v = 0; c->steer = 0; c->lap = -1; c->finished = 0; c->coins = 0;
        c->lane = lat * 0.6f;
        c->skill = 0.85f + 0.035f * i;            // the fastest AI starts at the back
        c->best_lap_ms = 0; c->last_lap_ms = 0; c->finish_ms = 0;
        c->bump_cool = c->boost_t = c->drift_t = c->stuck_t = c->wrong_t = 0; c->drift_dir = 0;
        c->bvx = c->bvz = c->spin = 0; c->draft_t = 0;
        c->fx = sinf_(c->heading); c->fz = cosf_(c->heading);
        c->seg = TRACK_N - 1;
        c->seg = track_nearest(c->x, c->z, c->seg, TRACK_N / 2);
        track_frame(c->seg, c->x, c->z, &c->s, &c->lat);
        c->total = c->s - g_trk_len;              // behind the line: slightly negative
        vx_obj_pos(c->obj, iroundf(c->x), 0, iroundf(c->z));
        vx_obj_rot(c->obj, 0, iroundf(deg(c->heading)), 0);
    }
    for (int k = 0; k < NCOINS; k++) { g_coin[k].respawn_ms = 0; vx_obj_show(g_coin[k].obj, 1); }
}

float car_speed_kmh(int i) { const float v = g_car[i].v; return (v < 0 ? -v : v) * 0.162f; }

int car_position(int i) {
    int p = 1;
    for (int j = 0; j < NCARS; j++) {
        if (j == i) continue;
        const Car *a = &g_car[j], *b = &g_car[i];
        if (a->finished && b->finished ? a->finish_ms < b->finish_ms : a->total > b->total) p++;
    }
    return p;
}

// AI: steer toward a point ahead on its lane; slow for the curvature coming up; aim at boost pads
// and coins it can reach; a little rubber band keeps the race close.
static void ai_drive(Car *c, Input *out, int now_ms) {
    const int idx = (int)(c - g_car);
    // Each driver weaves a little around its lane (no two take the same line), heads for a boost
    // pad just ahead, and steps out to pass a kart it is catching.
    float lane = c->lane + sinf_(now_ms * 0.00045f + idx * 2.1f) * 45.0f;
    for (int k = 0; k < NPADS; k++) {
        float d = g_pad[k].s - c->s;
        if (d < 0) d += g_trk_len;
        if (d > 150 && d < 900) lane = g_pad[k].lat;
    }
    for (int j = 0; j < NCARS; j++) {
        const Car *o = &g_car[j];
        if (o == c) continue;
        float ds = o->s - c->s;
        if (ds < -g_trk_len / 2) ds += g_trk_len;
        if (ds > 40 && ds < 420 && fabsf_(o->lat - lane) < 95 && c->v > o->v - 40)
            lane = o->lat + (o->lat > 0 ? -130.0f : 130.0f);
    }
    lane = clampf(lane, -ROAD_HW + 60, ROAD_HW - 60);
    float tx, tz;
    const float look = 320.0f + (c->v > 0 ? c->v : 0) * 0.42f;
    track_point(c->s + look, lane, &tx, &tz, 0);
    const float err = wrap_pi(atan2f_(tx - c->x, tz - c->z) - c->heading);
    out->left = err < -0.03f; out->right = err > 0.03f;
    // Brake for the bend coming up (precomputed per sample), further ahead the faster we go.
    const int ahead = (c->seg + 2 + (int)(c->v * 0.004f)) % TRACK_N;
    float vt = VMAX * c->skill * (1.0f - clampf(g_trk_bend[ahead] * 0.75f, 0, 0.5f)) * (1.0f + 0.012f * c->coins);
    const float gap = g_car[0].total - c->total;                    // rubber band, gently
    vt *= 1.0f + clampf(gap / 6000.0f, -0.08f, 0.05f);
    out->gas = c->v < vt;
    out->brake = c->v > vt + 90.0f;
    c->steer = clampf(err * 2.6f, -1, 1);
}

static int lap_check(Car *c, float prev_s, int now_ms) {
    const float d = c->s - prev_s;
    int ev = 0;
    if (d < -g_trk_len / 2) {                 // crossed the line forwards
        c->lap++;
        if (c->lap >= 1 && !c->finished) {
            c->last_lap_ms = (float)(now_ms - c->lap_start_ms);
            if (c->best_lap_ms == 0 || c->last_lap_ms < c->best_lap_ms) c->best_lap_ms = c->last_lap_ms;
            ev |= 1;
        }
        if (c->lap >= 0) c->lap_start_ms = (float)now_ms;
        if (c->lap >= LAPS && !c->finished) { c->finished = 1; c->finish_ms = (float)now_ms; }
    } else if (d > g_trk_len / 2) {
        c->lap--;                             // reversed over it
    }
    c->total = c->lap * g_trk_len + c->s;
    return ev;
}

static void respawn(Car *c) {
    float x, z, h;
    track_point(c->s, clampf(c->lat, -ROAD_HW * 0.4f, ROAD_HW * 0.4f), &x, &z, &h);
    c->x = x; c->z = z; c->heading = h; c->v = 0; c->steer = 0;
    c->stuck_t = c->wrong_t = c->drift_t = 0;
    c->bvx = c->bvz = c->spin = 0;
    place(c);
}

int cars_launch(int gas_ms) {
    // AI: the better drivers nail it more often.
    for (int i = 1; i < NCARS; i++)
        if (rnd(100) < (int)((g_car[i].skill - 0.8f) * 400)) g_car[i].boost_t = 0.8f;
    Car *p = &g_car[0];
    if (gas_ms > 0 && gas_ms <= 700) {                // floored right on the last beep
        p->boost_t = 1.2f;
        return 1;
    }
    if (gas_ms > 1600) {                              // held since the first light: flooded
        p->v = 0; p->stuck_t = 0;
        vx_emit(g_fx_smoke, iroundf(p->x - p->fx * 60), 20, iroundf(p->z - p->fz * 60), 0, 60, 0, 40, 8);
        return -1;
    }
    return 0;
}

// A hit against a surface with outward normal (nx,nz): the kart's full velocity (driven + shove)
// loses the part going into the surface (a little of it comes back as a bounce), keeps most of the
// part along it (it slides), and is split back into forward speed and sideways shove. The nose then
// swings toward the new direction, so a glancing hit scrapes along instead of stopping dead.
// Returns the impact speed (how hard it went in).
static float hit_surface(Car *c, float nx, float nz, float bounce) {
    const float fx = c->fx, fz = c->fz;
    float vx = fx * c->v + c->bvx, vz = fz * c->v + c->bvz;
    const float vn = vx * nx + vz * nz;
    if (vn >= 0) return 0;
    vx -= (1.0f + bounce) * vn * nx; vz -= (1.0f + bounce) * vn * nz;
    const float keep = vn < -500 ? 0.75f : 0.9f;                 // scrape
    vx *= keep; vz *= keep;
    c->v = vx * fx + vz * fz;
    c->bvx = vx - fx * c->v; c->bvz = vz - fz * c->v;
    const float sp = sqrtf_(vx * vx + vz * vz);
    if (sp > 60 && c->v > 0) {
        const float want = atan2f_(vx, vz);
        c->heading = wrap_pi(c->heading + clampf(wrap_pi(want - c->heading) * 0.35f, -0.3f, 0.3f));
        c->fx = sinf_(c->heading); c->fz = cosf_(c->heading);
    }
    return -vn;
}

int cars_update(const Input *in, float dt, int racing, int now_ms) {
    int events = 0;
    for (int i = 0; i < NCARS; i++) {
        Car *c = &g_car[i];
        Input ai = {0, 0, 0, 0};
        const Input *u = in;
#ifdef VX_AUTOPILOT   // simulator only (store screenshots): the AI drives the player's kart too
        const int player = 0;
#else
        const int player = !c->ai && !c->finished;
#endif
        if (!player) { ai_drive(c, &ai, now_ms); u = &ai; }
        if (!racing) { ai.gas = ai.brake = 0; u = &ai; }
        if (player && racing) {                            // smooth the digital steering
            const float target = (float)(in->right - in->left);
            c->steer += clampf(target - c->steer, -dt * 5.0f, dt * 5.0f);
        }
        const int off = fabsf_(c->lat) > ROAD_HW + 6;
        float vmax = (off ? VMAX_OFF : VMAX) * (1.0f + 0.015f * c->coins);
        float accel = ACCEL;
        if (c->boost_t > 0) { vmax *= BOOST_K; accel *= 2.0f; c->boost_t -= dt; }

        // Throttle, brake, reverse. Braking below walking pace engages reverse.
        if (u->gas) {
            if (c->v < 0) c->v += BRAKE * dt;
            else c->v += accel * dt * (1.0f - c->v / (vmax + 1.0f));
        } else if (u->brake) {
            if (c->v > 20) c->v -= BRAKE * dt;
            else if (c->v > VREV) c->v -= accel * 0.6f * dt;
        } else {
            const float drag = (70.0f + fabsf_(c->v) * 0.18f) * dt;
            c->v = c->v > 0 ? (c->v > drag ? c->v - drag : 0) : (c->v < -drag ? c->v + drag : 0);
        }
        if (c->v > vmax) c->v -= (off ? 1500.0f : 320.0f) * dt;
        if (c->v < VREV) c->v = VREV;

        // Steering: yaw rate grows with speed up to a point, then tightens; reversing turns the
        // other way, like a real car.
        const float av = fabsf_(c->v);
        const float grip = clampf(av / 240.0f, 0, 1) * (1.0f - 0.34f * av / VMAX) * (off ? 0.7f : 1.0f);
        const float dir = c->v < 0 ? -1.0f : 1.0f;
        c->heading = wrap_pi(c->heading + c->steer * 2.35f * grip * dt * dir);

        // Drift mini-turbo: hold a hard turn at speed, sparks charge blue then orange; straighten
        // up to cash it in as a boost.
        if (!off && av > VMAX * 0.55f && fabsf_(c->steer) > 0.75f) {
            const int d = c->steer > 0 ? 1 : -1;
            if (c->drift_dir != d) { c->drift_dir = d; c->drift_t = 0; }
            c->drift_t += dt;
            if (c->drift_t > 0.55f) {
                const float rx = c->x - c->fx * 40, rz = c->z - c->fz * 40;
                vx_emit(c->drift_t > 1.3f ? g_fx_spark : g_fx_drift, iroundf(rx), 8, iroundf(rz), 0, 120, 0, 90, 2);
            }
        } else if (fabsf_(c->steer) < 0.3f) {
            if (c->drift_t > 0.55f) {
                c->boost_t = c->drift_t > 1.3f ? 1.1f : 0.6f;
                if (player) events |= 4;
            }
            c->drift_t = 0; c->drift_dir = 0;
        }

        c->heading = wrap_pi(c->heading + c->spin * dt);
        c->fx = sinf_(c->heading); c->fz = cosf_(c->heading);      // once per frame, reused below
        c->x += (c->fx * c->v + c->bvx) * dt;
        c->z += (c->fz * c->v + c->bvz) * dt;
        {   // shove and spin die out quickly (tyres grip again); faster on the road than on grass
            const float k = 1.0f - clampf(dt * (off ? 3.5f : 5.5f), 0, 1);
            c->bvx *= k; c->bvz *= k; c->spin *= 1.0f - clampf(dt * 6.0f, 0, 1);
        }
        const float prev_s = c->s;
        place(c);

        // Soft wall well off the road: slide back along it, scrub speed, sparks, drop coins.
        if (fabsf_(c->lat) > LIMIT_HW) {
            float bx, bz, th;
            track_point(c->s, c->lat > 0 ? LIMIT_HW - 4 : -LIMIT_HW + 4, &bx, &bz, &th);
            c->x = bx; c->z = bz;
            // the wall's normal points back toward the road: across the track, against the side
            const float side = c->lat > 0 ? 1.0f : -1.0f;
            const float nx = -side * cosf_(th), nz = side * sinf_(th);
            const float imp = hit_surface(c, nx, nz, 0.2f);
            if (c->bump_cool <= 0 && imp > 220) {
                vx_emit(g_fx_spark, iroundf(c->x), 20, iroundf(c->z), 0, 200, 0, 250, 6);
                if (imp > 450 && c->coins > 0) c->coins -= c->coins > 1 ? 2 : 1;
                if (player) events |= 8;
                c->bump_cool = 0.4f;
            }
            place(c);
        }
        // Solid scenery: push the kart out of the closest point, scrub the speed going into it and
        // bounce back off a head-on hit (Mario Kart style).
        for (int k = fabsf_(c->lat) > ROAD_HW + 10 ? 0 : g_nsolid; k < g_nsolid; k++) {   // all off-road
            const Solid *o = &g_solid[k];
            const float R = 36.0f;
            float px = o->x0, pz = o->z0, rr = o->r + R;
            if (o->r <= 0) { px = clampf(c->x, o->x0, o->x1); pz = clampf(c->z, o->z0, o->z1); rr = R; }
            float dx = c->x - px, dz = c->z - pz, d2 = dx * dx + dz * dz;
            if (d2 >= rr * rr) continue;
            float d = sqrtf_(d2);
            if (d < 1e-3f) {                                  // centre inside a box: back out the way we came
                dx = -sinf_(c->heading) * (c->v >= 0 ? 1 : -1); dz = -cosf_(c->heading) * (c->v >= 0 ? 1 : -1); d = 1;
            }
            const float nx = dx / d, nz = dz / d;
            c->x = px + nx * rr; c->z = pz + nz * rr;
            const float imp = hit_surface(c, nx, nz, 0.35f);
            if (imp > 380) c->spin += (c->steer >= 0 ? 1.0f : -1.0f) * clampf(imp / 300.0f, 0, 3.0f);
            if (c->bump_cool <= 0 && imp > 200) {
                vx_emit(g_fx_spark, iroundf(c->x - nx * R), 24, iroundf(c->z - nz * R), 0, 220, 0, 260, 6);
                if (player) events |= 8;
                if (imp > 500 && c->coins > 0) c->coins--;
                c->bump_cool = 0.35f;
            }
            place(c);
        }
        if (racing || c->finished) events |= player ? lap_check(c, prev_s, now_ms) : (lap_check(c, prev_s, now_ms), 0);

        // Boost pads and coins.
        for (int k = 0; k < NPADS; k++) {
            float ds = c->s - g_pad[k].s;
            if (ds > g_trk_len / 2) ds -= g_trk_len;
            if (ds < -g_trk_len / 2) ds += g_trk_len;
            if (fabsf_(ds) < 95 && fabsf_(c->lat - g_pad[k].lat) < 65 && c->v > 0) {
                if (c->boost_t < 0.9f && player) events |= 4;
                c->boost_t = c->boost_t > 1.2f ? c->boost_t : 1.2f;
            }
        }
        for (int k = 0; k < NCOINS; k++) {
            Pickup *p = &g_coin[k];
            if (p->respawn_ms) continue;
            float ds = c->s - p->s;
            if (ds > g_trk_len / 2) ds -= g_trk_len;
            if (ds < -g_trk_len / 2) ds += g_trk_len;
            if (ds > 70 || ds < -70) continue;                        // cheap along-track reject
            const float ex = c->x - p->x, ez = c->z - p->z;
            if (ex * ex + ez * ez < 70 * 70) {
                if (c->coins < 10) c->coins++;
                p->respawn_ms = now_ms + 8000;
                vx_obj_show(p->obj, 0);
                if (player) events |= 2;
            }
        }

        // Slipstream: tucked in behind another kart at speed for a second = a short tow boost.
        if (c->v > VMAX * 0.6f && c->boost_t <= 0) {
            int tucked = 0;
            for (int j = 0; j < NCARS && !tucked; j++) {
                const Car *o = &g_car[j];
                if (o == c) continue;
                float ds = o->s - c->s;
                if (ds < -g_trk_len / 2) ds += g_trk_len;
                tucked = ds > 90 && ds < 650 && fabsf_(o->lat - c->lat) < 65;
            }
            c->draft_t = tucked ? c->draft_t + dt : (c->draft_t > dt * 2 ? c->draft_t - dt * 2 : 0);
            if (c->draft_t > 1.1f) {
                c->boost_t = 0.9f; c->draft_t = 0;
                if (player) events |= 32;
            }
        } else {
            c->draft_t = 0;
        }

        // Stuck (off track, barely moving) or driving the wrong way for a while: back on track.
        float th; float tx, tz;
        track_point(c->s, 0, &tx, &tz, &th);
        const int wrong = fabsf_(wrap_pi(th - c->heading)) > 2.1f && c->v > 150;
        c->wrong_t = wrong ? c->wrong_t + dt : 0;
        c->stuck_t = (off && av < 60 && (u->gas || u->brake)) ? c->stuck_t + dt : 0;
        if (racing && (c->stuck_t > 2.5f || c->wrong_t > 3.0f)) {
            respawn(c);
            if (player) events |= 16;
        }

        // Effects.
        const float rx = c->x - c->fx * 50, rz = c->z - c->fz * 50;
        // A few small dust puffs off the road (every other frame); no brake smoke.
        if (off && av > 250 && ((now_ms >> 5) & 1)) vx_emit(g_fx_dust, iroundf(rx), 10, iroundf(rz), 0, 60, 0, 40, 1);
        if (c->boost_t > 0) vx_emit(g_fx_boost, iroundf(c->x - c->fx * 66), 28, iroundf(c->z - c->fz * 66),
                                    iroundf(-c->fx * 200), 30, iroundf(-c->fz * 200), 40, 2);
        vx_mat_color(c->mat_tail, (u->brake && c->v > 30) ? NV_RGB(255, 40, 30) : NV_RGB(120, 10, 10));
        c->bump_cool -= dt;
    }

    // Contact: overlapping karts are pushed apart and trade momentum along the line between them
    // (equal masses, a springy 0.45 restitution): rear-ending slows you and shoves the one ahead,
    // a side hit knocks both sideways and gives each a little spin.
    for (int i = 0; i < NCARS; i++)
        for (int j = i + 1; j < NCARS; j++) {
            Car *a = &g_car[i], *b = &g_car[j];
            const float dx = b->x - a->x, dz = b->z - a->z, d2 = dx * dx + dz * dz, r = 78.0f;
            if (d2 >= r * r || d2 < 1e-3f) continue;
            const float d = sqrtf_(d2), push = (r - d) * 0.5f + 0.5f, nx = dx / d, nz = dz / d;
            a->x -= nx * push; a->z -= nz * push; b->x += nx * push; b->z += nz * push;
            const float fax = a->fx, faz = a->fz, fbx = b->fx, fbz = b->fz;
            float vax = fax * a->v + a->bvx, vaz = faz * a->v + a->bvz;
            float vbx = fbx * b->v + b->bvx, vbz = fbz * b->v + b->bvz;
            const float closing = (vax - vbx) * nx + (vaz - vbz) * nz;   // > 0: moving into each other
            if (closing <= 0) continue;
            const float J = closing * (1.0f + 0.45f) * 0.5f + 40.0f;       // + a minimum nudge
            vax -= J * nx; vaz -= J * nz; vbx += J * nx; vbz += J * nz;
            a->v = vax * fax + vaz * faz; a->bvx = vax - fax * a->v; a->bvz = vaz - faz * a->v;
            b->v = vbx * fbx + vbz * fbz; b->bvx = vbx - fbx * b->v; b->bvz = vbz - fbz * b->v;
            // spin: which side of each kart got hit (cross of its heading with the normal)
            a->spin -= clampf(J / 260.0f, 0, 2.2f) * (fax * nz - faz * nx > 0 ? 1.0f : -1.0f);
            b->spin += clampf(J / 260.0f, 0, 2.2f) * (fbx * nz - fbz * nx > 0 ? 1.0f : -1.0f);
            if (a->bump_cool <= 0 && b->bump_cool <= 0 && closing > 120) {
                vx_emit(g_fx_spark, iroundf(a->x + dx / 2), 26, iroundf(a->z + dz / 2), 0, 260, 0, 320, 6);
                if (!a->ai || !b->ai) events |= 8;
                a->bump_cool = b->bump_cool = 0.3f;
            }
        }

    // Coins come back; karts, shadows and the coin spin follow the simulation.
    for (int k = 0; k < NCOINS; k++) {
        Pickup *p = &g_coin[k];
        if (p->respawn_ms && now_ms >= p->respawn_ms) { p->respawn_ms = 0; vx_obj_show(p->obj, 1); }
        if (!p->respawn_ms) vx_obj_pos(p->obj, iroundf(p->x), 48 + iroundf(sinf_(now_ms * 0.004f + k) * 8), iroundf(p->z));
    }
    for (int i = 0; i < NCARS; i++) {
        Car *c = &g_car[i];
        const int bob = c->v > 300 ? (int)(sinf_(now_ms * 0.03f + i) * 1.5f) : 0;
        vx_obj_pos(c->obj, iroundf(c->x), bob, iroundf(c->z));
        vx_obj_rot(c->obj, 0, iroundf(deg(c->heading)), 0);
        vx_obj_pos(c->shadow, iroundf(c->x), 3, iroundf(c->z));
        vx_obj_rot(c->shadow, 0, iroundf(deg(c->heading)), 0);
    }
    return events;
}
