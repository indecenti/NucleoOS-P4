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
#define ACCEL      700.0f
#define BRAKE     1600.0f
#define BOOST_K     1.38f    // top speed multiplier while boosting

static const uint16_t kBody[NCARS] = { 0xE0C3 /* red */, 0x22DB /* blue */, 0x2D48 /* green */, 0xFDA0 /* orange */ };
static int m_tyre, m_rim, m_dark, m_visor, m_suit, m_shadow, m_metal;

// ---- the kart model: nose toward +Z, wheels on the ground (y = 0) -----------------------------------
static void wheel(float wx, float wz, float r, float hw) {
    int in[8], out[8];
    for (int k = 0; k < 8; k++) {
        const float a = k * PI_F / 4 + PI_F / 8;
        in[k] = mb_v(wx - hw, r + cosf_(a) * r, wz + sinf_(a) * r, 0, 0);
        out[k] = mb_v(wx + hw, r + cosf_(a) * r, wz + sinf_(a) * r, 0, 0);
    }
    for (int k = 0; k < 8; k++) mb_quad(in[k], in[(k + 1) % 8], out[(k + 1) % 8], out[k], m_tyre, wx, r, wz);
    int *cap = wx > 0 ? out : in;                                  // the outer hub
    for (int k = 1; k < 7; k++) mb_tri(cap[0], cap[k], cap[k + 1], m_rim, wx > 0 ? wx - 30 : wx + 30, r, wz);
}

static int build_kart(int body, int tail) {
    mb_box(-30, 7, -46, 30, 15, 44, 0, 0, 0, body);               // floor tray
    mb_box(-19, 9, 24, 19, 24, 62, 5, 0, 20, body);                // sloped nose
    mb_box(-37, 7, -22, -29, 21, 22, 0, 4, 4, body);               // side pods
    mb_box(29, 7, -22, 37, 21, 22, 0, 4, 4, body);
    mb_box(-34, 9, 58, 34, 15, 66, 0, 0, 0, m_dark);               // front bumper
    mb_box(-13, 15, -32, 13, 42, -22, 0, 0, 0, m_dark);            // seat back
    mb_box(-11, 15, -24, 11, 38, -6, 2, 0, 3, m_suit);             // driver torso
    mb_box(-10, 38, -22, 10, 54, -3, 3, 3, 4, body);               // helmet
    const float vz = -3.0f;                                          // visor on the helmet front
    const int v0 = mb_v(-7, 42, vz + 0.6f, 0, 0), v1 = mb_v(7, 42, vz + 0.6f, 0, 0);
    const int v2 = mb_v(6, 50, vz - 3.2f, 0, 0), v3 = mb_v(-6, 50, vz - 3.2f, 0, 0);
    mb_quad(v0, v1, v2, v3, m_visor, 0, 46, -14);
    mb_box(-15, 13, -60, 15, 29, -44, 0, 0, 0, m_metal);           // engine block
    mb_box(-10, 22, -64, -5, 34, -58, 0, 0, 0, m_dark);            // exhausts
    mb_box(5, 22, -64, 10, 34, -58, 0, 0, 0, m_dark);
    mb_box(-36, 30, -66, 36, 34, -54, 0, 0, 0, body);              // rear wing
    for (int s = -1; s <= 1; s += 2) {                             // tail lights on the wing edge
        const int t0 = mb_v(s * 22, 30, -66.6f, 0, 0), t1 = mb_v(s * 34, 30, -66.6f, 0, 0);
        const int t2 = mb_v(s * 34, 34, -66.6f, 0, 0), t3 = mb_v(s * 22, 34, -66.6f, 0, 0);
        mb_quad(t0, t1, t2, t3, tail, 0, 32, 0);
    }
    wheel(-41, -36, 16, 7); wheel(41, -36, 16, 7);                 // fat rear wheels
    wheel(-39, 40, 12, 6); wheel(39, 40, 12, 6);
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
        c->obj = build_kart(c->mat_body, c->mat_tail);
        c->ai = i != 0;
    }
}

static void place(Car *c) {
    c->seg = track_nearest(c->x, c->z, c->seg, 12);
    track_frame(c->seg, c->x, c->z, &c->s, &c->lat);
}

void cars_grid(void) {
    for (int i = 0; i < NCARS; i++) {
        Car *c = &g_car[i];
        const float back = 150.0f + (i / 2) * 260.0f, lat = (i & 1) ? 70.0f : -70.0f;
        track_point(g_trk_len - back, lat, &c->x, &c->z, &c->heading);
        c->v = 0; c->steer = 0; c->lap = -1; c->finished = 0; c->coins = 0;
        c->lane = lat * 0.6f;
        c->skill = 0.88f + 0.03f * i;             // the fastest AI starts at the back
        c->best_lap_ms = 0; c->last_lap_ms = 0; c->finish_ms = 0;
        c->bump_cool = c->boost_t = c->drift_t = c->stuck_t = c->wrong_t = 0; c->drift_dir = 0;
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
static void ai_drive(Car *c, Input *out) {
    float tx, tz, lane = c->lane;
    for (int k = 0; k < NPADS; k++) {                 // swing over to a pad just ahead
        float d = g_pad[k].s - c->s;
        if (d < 0) d += g_trk_len;
        if (d > 150 && d < 900) lane = g_pad[k].lat;
    }
    const float look = 320.0f + (c->v > 0 ? c->v : 0) * 0.42f;
    track_point(c->s + look, lane, &tx, &tz, 0);
    const float want = atan2f_(tx - c->x, tz - c->z);
    const float err = wrap_pi(want - c->heading);
    out->left = err < -0.03f; out->right = err > 0.03f;
    float h0, h1, dx, dz;
    track_point(c->s + 250.0f, 0, &dx, &dz, &h0);
    track_point(c->s + 850.0f + c->v * 0.35f, 0, &dx, &dz, &h1);
    const float curv = fabsf_(wrap_pi(h1 - h0));
    float vt = VMAX * c->skill * (1.0f - clampf(curv * 0.75f, 0, 0.5f)) * (1.0f + 0.012f * c->coins);
    const float gap = g_car[0].total - c->total;
    vt *= 1.0f + clampf(gap / 5000.0f, -0.07f, 0.10f);
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
    place(c);
}

int cars_update(const Input *in, float dt, int racing, int now_ms) {
    int events = 0;
    for (int i = 0; i < NCARS; i++) {
        Car *c = &g_car[i];
        Input ai = {0, 0, 0, 0};
        const Input *u = in;
        const int player = !c->ai && !c->finished;
        if (!player) { ai_drive(c, &ai); u = &ai; }
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
                const float rx = c->x - sinf_(c->heading) * 40, rz = c->z - cosf_(c->heading) * 40;
                vx_emit(c->drift_t > 1.3f ? g_fx_spark : g_fx_drift, iroundf(rx), 8, iroundf(rz), 0, 120, 0, 90, 2);
            }
        } else if (fabsf_(c->steer) < 0.3f) {
            if (c->drift_t > 0.55f) {
                c->boost_t = c->drift_t > 1.3f ? 1.1f : 0.6f;
                if (player) events |= 4;
            }
            c->drift_t = 0; c->drift_dir = 0;
        }

        c->x += sinf_(c->heading) * c->v * dt;
        c->z += cosf_(c->heading) * c->v * dt;
        const float prev_s = c->s;
        place(c);

        // Soft wall well off the road: slide back along it, scrub speed, sparks, drop coins.
        if (fabsf_(c->lat) > LIMIT_HW) {
            float bx, bz, th;
            track_point(c->s, c->lat > 0 ? LIMIT_HW - 4 : -LIMIT_HW + 4, &bx, &bz, &th);
            c->x = bx; c->z = bz;
            c->heading = wrap_pi(c->heading + clampf(wrap_pi(th - c->heading), -0.35f, 0.35f));
            if (c->bump_cool <= 0 && av > 250) {
                vx_emit(g_fx_spark, iroundf(c->x), 20, iroundf(c->z), 0, 200, 0, 250, 14);
                if (c->coins > 0) c->coins -= c->coins > 1 ? 2 : 1;
                if (player) events |= 8;
                c->bump_cool = 0.4f;
            }
            c->v *= 0.6f;
            place(c);
        }
        // Solid scenery: push the kart out of the closest point, scrub the speed going into it and
        // bounce back off a head-on hit (Mario Kart style).
        for (int k = 0; k < g_nsolid; k++) {
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
            const float into = -(sinf_(c->heading) * nx + cosf_(c->heading) * nz) * (c->v >= 0 ? 1.0f : -1.0f);
            if (into > 0.15f) {
                const float spd = fabsf_(c->v);
                if (into > 0.7f && spd > 220) c->v = -c->v * 0.3f;       // head-on: bounce back
                else c->v *= 1.0f - 0.6f * into;
                if (c->bump_cool <= 0 && spd > 200) {
                    vx_emit(g_fx_spark, iroundf(c->x - nx * R), 24, iroundf(c->z - nz * R), 0, 220, 0, 260, 12);
                    if (player) events |= 8;
                    if (into > 0.7f && c->coins > 0) c->coins--;
                    c->bump_cool = 0.35f;
                }
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
            const float ex = c->x - p->x, ez = c->z - p->z;
            if (ex * ex + ez * ez < 70 * 70) {
                if (c->coins < 10) c->coins++;
                p->respawn_ms = now_ms + 8000;
                vx_obj_show(p->obj, 0);
                if (player) events |= 2;
            }
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
        const float rx = c->x - sinf_(c->heading) * 50, rz = c->z - cosf_(c->heading) * 50;
        if (off && av > 150) vx_emit(g_fx_dust, iroundf(rx), 12, iroundf(rz), 0, 80, 0, 60, 1 + (av > 400));
        if (u->brake && c->v > 500) vx_emit(g_fx_smoke, iroundf(rx), 8, iroundf(rz), 0, 40, 0, 30, 1);
        if (c->boost_t > 0) vx_emit(g_fx_boost, iroundf(c->x - sinf_(c->heading) * 66), 28,
                                    iroundf(c->z - cosf_(c->heading) * 66), iroundf(-sinf_(c->heading) * 200), 30,
                                    iroundf(-cosf_(c->heading) * 200), 40, 2);
        vx_mat_color(c->mat_tail, (u->brake && c->v > 30) ? NV_RGB(255, 40, 30) : NV_RGB(120, 10, 10));
        c->bump_cool -= dt;
    }

    // Contact: separate overlapping karts, trade speed along the hit, sparks.
    for (int i = 0; i < NCARS; i++)
        for (int j = i + 1; j < NCARS; j++) {
            Car *a = &g_car[i], *b = &g_car[j];
            const float dx = b->x - a->x, dz = b->z - a->z, d2 = dx * dx + dz * dz, r = 74.0f;
            if (d2 >= r * r || d2 < 1e-3f) continue;
            const float d = sqrtf_(d2), push = (r - d) * 0.5f + 1.0f, nx = dx / d, nz = dz / d;
            a->x -= nx * push; a->z -= nz * push; b->x += nx * push; b->z += nz * push;
            // Speed along the contact normal: the rear kart pushes, the front one is nudged.
            const float va = a->v * (sinf_(a->heading) * nx + cosf_(a->heading) * nz);
            const float vb = b->v * (sinf_(b->heading) * nx + cosf_(b->heading) * nz);
            const float impact = va - vb;
            if (impact > 0) { a->v -= impact * 0.35f; b->v += impact * 0.25f; }
            a->heading = wrap_pi(a->heading - 0.04f * (nx * cosf_(a->heading) - nz * sinf_(a->heading)));
            b->heading = wrap_pi(b->heading + 0.04f * (nx * cosf_(b->heading) - nz * sinf_(b->heading)));
            if (a->bump_cool <= 0 && b->bump_cool <= 0 && impact > 120) {
                vx_emit(g_fx_spark, iroundf(a->x + dx / 2), 26, iroundf(a->z + dz / 2), 0, 260, 0, 320, 16);
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
