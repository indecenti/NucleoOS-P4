// Vertice game template — start a new 3D game for NucleoOS from here.
//
// A tiny complete game on the Vertice engine: drive a little car round a park and pick up the
// spinning coins. It shows the parts every 3D game needs, each kept short so you can replace it:
//   world     sky, fog, sun, a textured floor, decor built with vx_build.h (rocks, trees, a tower)
//   objects   a car made of several shapes, a blob shadow under it, coins (cloned), particles
//   input     gamepad / keyboard (nv_gfx_pad) and touch (left half steers, right half drives)
//   camera    a chase camera smoothed with nv_smooth (frame-rate independent)
//   HUD       2D drawn over the 3D frame: score, FPS, and a marker over the nearest coin placed
//             with vx_project (the engine's own projection)
// Build:      .\sdk\build_app.ps1 -AppDir sdk\templates\vertice-game -Aot
// Simulator:  bash tools/vertice/sim/run.sh sdk/templates/vertice-game 300   (WSL, PNG frames)
// Read docs/VERTICE.md (engine) and docs/GAME_DEV.md (assets, store) next.
#include "nucleo_sdk.h"
#include "nv_math.h"
#include "vx_build.h"

#define W 512
#define H 300
#define NCOINS 12

static int s_car, s_turret, s_coin[NCOINS], s_got[NCOINS], s_score, s_sparks;
static float s_x, s_z, s_yaw, s_speed;                       // the car
static NvVec3 s_eye, s_at;                                   // the camera, smoothed
static float s_coin_x[NCOINS], s_coin_z[NCOINS];

// Collisions are the game's job (the engine only draws): every solid thing is kept as a circle on the
// ground, and the car is a circle too. Enough for most 3D games on this hardware; walls would be
// segments, a platformer adds heights.
#define MAX_SOLIDS 48
#define CAR_R 46.0f
static float s_sx[MAX_SOLIDS], s_sz[MAX_SOLIDS], s_sr[MAX_SOLIDS];
static int s_nsolid;
static void solid(float x, float z, float r) {
    if (s_nsolid < MAX_SOLIDS) { s_sx[s_nsolid] = x; s_sz[s_nsolid] = z; s_sr[s_nsolid] = r; s_nsolid++; }
}
// Push a circle (x, z, r) out of every solid; returns 1 if it touched one.
static int collide(float *x, float *z, float r) {
    int hit = 0;
    for (int i = 0; i < s_nsolid; i++) {
        const float dx = *x - s_sx[i], dz = *z - s_sz[i], min = r + s_sr[i], d2 = dx * dx + dz * dz;
        if (d2 >= min * min || d2 < 1e-6f) continue;
        const float d = nv_sqrtf(d2), push = (min - d) / d;
        *x += dx * push; *z += dz * push;
        hit = 1;
    }
    return hit;
}

// A procedural checker floor texture (a real game loads painted ones: vx_texture_load("grass", 0)).
static int floor_texture(void) {
    static uint16_t px[64 * 64];
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            const int c = ((x >> 4) + (y >> 4)) & 1, n = (x * 7 + y * 13) % 9;
            px[y * 64 + x] = c ? nv_rgb565(70 + n, 140 + n, 70) : nv_rgb565(60 + n, 124 + n, 62);
        }
    const int t = vx_texture_new(64, 64, 0, 0);
    vx_texture_write(t, 0, 0, 64, 64, px);
    return t;
}

static void build_world(void) {
    NvRand rnd;
    nv_rand_seed(&rnd, 7);
    vx_sky(NV_RGB(90, 160, 240), NV_RGB(210, 230, 250));
    vx_fog(2500, 6000);
    vx_sun(210, 50, 0xFFF4E0, 230);
    vx_ambient(0x506070);
    vx_floor(0, floor_texture(), 400, NV_RGB(70, 140, 70));

    const int stone = vx_material(NV_RGB(150, 150, 160), VX_GOURAUD, 255, -1, 0);
    const int bark = vx_material(NV_RGB(120, 84, 50), VX_GOURAUD, 255, -1, 0);
    const int leaf = vx_material(NV_RGB(50, 140, 60), VX_GOURAUD, 255, -1, 0);
    const int brick = vx_material(NV_RGB(190, 90, 60), VX_FLAT, 255, -1, 0);
    // Decor in a few meshes (one per area would be better for culling in a big world).
    for (int i = 0; i < 14; i++) {
        const float a = nv_rand_float(&rnd) * 2 * NV_PI, d = nv_rand_range(&rnd, 600, 2600);
        const float rx = nv_sinf(a) * d, rz = nv_cosf(a) * d, rr = nv_rand_range(&rnd, 30, 80);
        vxb_rock(&rnd, rx, rz, rr, nv_rand_range(&rnd, 30, 90), stone, 120);
        solid(rx, rz, rr * 0.9f);
    }
    vxb_commit(stone, 0, 0);
    for (int i = 0; i < 16; i++) {
        const float a = nv_rand_float(&rnd) * 2 * NV_PI, d = nv_rand_range(&rnd, 700, 3000);
        const float x = nv_sinf(a) * d, z = nv_cosf(a) * d, h = nv_rand_range(&rnd, 160, 260);
        vxb_limb(x, 0, z, 16, x, h, z, 10, 6, 0, bark, 120);                     // trunk
        solid(x, z, 18);                                                         // the trunk stops you
        static const float cr[5] = { 10, 70, 80, 50, 4 }, cy[5] = { 0, 30, 90, 150, 190 };
        float ry[5];
        for (int k = 0; k < 5; k++) ry[k] = h - 40 + cy[k];
        vxb_lathe(x, z, cr, ry, 5, 8, leaf, 200);                                // crown
    }
    vxb_commit(leaf, 0, VX_MESH_SMOOTH);
    {   // a tower in the middle distance
        static const float r[6] = { 120, 110, 110, 130, 130, 0 }, y[6] = { 0, 20, 380, 400, 440, 560 };
        vxb_lathe(0, 3200, r, y, 6, 12, brick, 200);
        solid(0, 3200, 125);
        vxb_commit(brick, 0, 0);
    }
}

static void build_car(void) {
    const int body = vx_material(NV_RGB(220, 40, 40), VX_GOURAUD, 255, -1, 120);
    const int glass = vx_material(NV_RGB(60, 90, 130), VX_GOURAUD, 255, -1, 160);
    const int tyre = vx_material(NV_RGB(30, 30, 34), VX_GOURAUD, 255, -1, 0);
    vxb_box(-34, 14, -60, 34, 40, 60, body, 0);                                 // nose +Z
    vxb_box(-28, 40, -30, 28, 64, 26, glass, 0);
    for (int i = 0; i < 4; i++) {
        const float wx = i & 1 ? 38 : -38, wz = i & 2 ? 38 : -38;
        vxb_limb(wx - 6, 16, wz, 16, wx + 6, 16, wz, 16, 8, VXB_CAP_A | VXB_CAP_B, tyre, 100);
    }
    s_car = vxb_commit(body, 0, 0);
    vx_obj_shadow(s_car, 72, 1, 110);                       // a blob shadow that follows the car
    // A little turret on the roof, attached to the car: its position and angle are now relative to
    // the car and it follows every move; the game only turns it (see update).
    vxb_box(-10, 0, -10, 10, 12, 10, glass, 0);
    vxb_limb(0, 6, 0, 4, 0, 6, 34, 3, 6, VXB_CAP_B, tyre, 100);
    s_turret = vxb_commit(glass, 0, 0);
    vx_obj_parent(s_turret, s_car);
    vx_obj_pos(s_turret, 0, 64, -6);
}

static void build_coins(void) {
    NvRand rnd;
    nv_rand_seed(&rnd, 99);
    const int gold = vx_material(NV_RGB(255, 200, 40), VX_GOURAUD, 255, -1, 200);
    vxb_limb(-6, 0, 0, 28, 6, 0, 0, 28, 12, VXB_CAP_A | VXB_CAP_B, gold, 100);   // a coin standing on edge
    const int proto = vxb_commit(gold, 0, VX_MESH_SMOOTH);
    for (int i = 0; i < NCOINS; i++) {
        s_coin[i] = i ? vx_clone(proto) : proto;                                // clones share the mesh
        const float a = i * (2 * NV_PI / NCOINS), d = nv_rand_range(&rnd, 500, 1800);
        s_coin_x[i] = nv_sinf(a) * d; s_coin_z[i] = nv_cosf(a) * d;
        vx_obj_pos(s_coin[i], nv_roundi(s_coin_x[i]), 50, nv_roundi(s_coin_z[i]));
        vx_obj_appear(s_coin[i], 0, 0);
        vx_obj_fade(s_coin[i], 3000, 3800);                                    // far coins dissolve away
    }
    // A flag next to each coin, from a model made with tools/vertice/obj2vxm.py (models/flag.vxm)
    const int flag = vx_model("flag", 0);
    for (int i = 0; flag >= 0 && i < NCOINS; i++) {
        const int f = i ? vx_clone(flag) : flag;
        vx_obj_pos(f, nv_roundi(s_coin_x[i]) + 60, 0, nv_roundi(s_coin_z[i]));
    }
    s_sparks = vx_emitter(64, NV_RGB(255, 240, 120), NV_RGB(255, 120, 20), 10, 2, 700, -300, VX_PART_ADDITIVE);
}

// Input: pad / keyboard, or touch (left half: steer toward the finger; right half: drive).
static void read_input(float *steer, float *throttle) {
    const int pad = nv_gfx_pad();
    *steer = (pad & NV_PAD_RIGHT ? 1.0f : 0.0f) - (pad & NV_PAD_LEFT ? 1.0f : 0.0f);
    *throttle = (pad & (NV_PAD_UP | NV_PAD_A) ? 1.0f : 0.0f) - (pad & (NV_PAD_DOWN | NV_PAD_B) ? 0.6f : 0.0f);
    for (int i = 0; i < nv_touch_count(); i++) {
        int x, y;
        if (!nv_touch_at(i, &x, &y)) continue;
        if (x < W / 2) *steer = nv_clampf((x - W / 4) / (W / 6.0f), -1, 1);
        else *throttle = 1.0f;
    }
}

static void update(float dt) {
    float steer, throttle;
    read_input(&steer, &throttle);
    s_speed = nv_approach(s_speed, throttle * 520.0f, (throttle != 0 ? 600.0f : 300.0f) * dt);
    s_yaw += steer * dt * 2.2f * nv_clampf(s_speed / 200.0f, -1, 1);
    s_x += nv_sinf(s_yaw) * s_speed * dt;
    s_z += nv_cosf(s_yaw) * s_speed * dt;
    if (collide(&s_x, &s_z, CAR_R)) {                 // bumped into something: slide along it, lose speed
        if (nv_absf(s_speed) > 120) { nv_gfx_tone(110, 60); vx_emit(s_sparks, nv_roundi(s_x), 30, nv_roundi(s_z), 0, 120, 0, 80, 6); }
        s_speed *= 0.6f;
    }
    vx_obj_pos(s_car, nv_roundi(s_x), 0, nv_roundi(s_z));
    vx_obj_rot(s_car, 0, nv_roundi(nv_deg(s_yaw)), 0);
    vx_obj_rot(s_turret, 0, (nv_millis() / 8) % 360, 0);                 // relative to the car
    // coins: spin, and picked up when the car drives through them
    const int spin = (nv_millis() / 4) % 360;
    for (int i = 0; i < NCOINS; i++) {
        if (s_got[i]) continue;
        vx_obj_rot(s_coin[i], 0, spin, 0);
        vx_obj_alpha(s_coin[i], (nv_millis() / 200) % 4 ? 255 : 150);       // a glint now and then
        const float dx = s_coin_x[i] - s_x, dz = s_coin_z[i] - s_z;
        if (dx * dx + dz * dz < 70 * 70) {
            s_got[i] = 1; s_score++;
            vx_obj_show(s_coin[i], 0);
            vx_emit(s_sparks, nv_roundi(s_coin_x[i]), 50, nv_roundi(s_coin_z[i]), 0, 220, 0, 160, 30);
            nv_gfx_tone(1320, 60);
        }
    }
    // chase camera: behind and above, looking a little ahead; smoothed so turns feel weighty
    const NvVec3 fwd = nv_v3_dir(s_yaw, 0);
    const NvVec3 eye = nv_v3(s_x - fwd.x * 300, 170, s_z - fwd.z * 300);
    const NvVec3 at = nv_v3(s_x + fwd.x * 200, 30, s_z + fwd.z * 200);
    s_eye = nv_v3_lerp(s_eye, eye, nv_clampf(6 * dt, 0, 1));
    s_at = nv_v3_lerp(s_at, at, nv_clampf(8 * dt, 0, 1));
    vx_lens(62, 16, 7000);
    vx_camera(nv_roundi(s_eye.x), nv_roundi(s_eye.y), nv_roundi(s_eye.z), 0, 0, 0);
    vx_look_at(nv_roundi(s_at.x), nv_roundi(s_at.y), nv_roundi(s_at.z));
}

static void draw_hud(int fps) {
    char b[32];
    nv_gfx_panel(6, 6, 150, 24, 6, NV_RGB(20, 30, 50), NV_RGB(10, 16, 30), 200);
    nv_snprintf(b, sizeof b, "COINS %d/%d", s_score, NCOINS);
    nv_gfx_text(14, 11, b, NV_RGB(255, 220, 80), 2);
    nv_snprintf(b, sizeof b, "%d FPS", fps);
    nv_gfx_panel(W - 64, 6, 58, 16, 5, NV_RGB(20, 30, 50), NV_RGB(10, 16, 30), 200);
    nv_gfx_text(W - 58, 10, b, NV_RGB(200, 220, 255), 1);
    // a marker over the nearest coin still out there, placed with the engine's projection
    int best = -1;
    float bd = 1e12f;
    for (int i = 0; i < NCOINS; i++) {
        if (s_got[i]) continue;
        const float dx = s_coin_x[i] - s_x, dz = s_coin_z[i] - s_z, d = dx * dx + dz * dz;
        if (d < bd) { bd = d; best = i; }
    }
    int32_t p[3];
    if (best >= 0 && vx_project(nv_roundi(s_coin_x[best]), 110, nv_roundi(s_coin_z[best]), p) &&
        p[0] > 0 && p[0] < W && p[1] > 0 && p[1] < H) {
        nv_gfx_tri(p[0] - 7, p[1] - 10, p[0] + 7, p[1] - 10, p[0], p[1], NV_RGB(255, 230, 90));
    }
    if (s_score == NCOINS) nv_gfx_text_center(H / 2 - 10, "ALL COINS!", NV_RGB(255, 240, 120), 3);
}

NV_EXPORT("run") void run(void) {
    build_world();
    build_car();
    build_coins();
    s_eye = nv_v3(0, 170, -300); s_at = nv_v3(0, 30, 200);
    int last = nv_millis(), frames = 0, fps = 0, fps_at = last;
    while (nv_gfx_present()) {
        const int now = nv_millis();
        const float dt = nv_clampf((now - last) / 1000.0f, 0, 0.05f);   // clamp: a hiccup is not a teleport
        last = now;
        update(dt);
        vx_render();                                                     // the 3D frame...
        draw_hud(fps);                                                   // ...then the 2D on top
        frames++;
        if (now - fps_at >= 1000) { fps = frames; frames = 0; fps_at = now; }
        if (nv_gfx_pad() & NV_PAD_SELECT) break;                         // Esc / Select: quit
    }
}
