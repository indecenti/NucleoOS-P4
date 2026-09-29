// Vertice GP — a 3D kart racer for NucleoOS, and the showcase of Vertice, the OS 3D engine.
// Four karts, three laps, boost pads, coins, drift mini-turbos. Touch controls, or a USB
// keyboard / gamepad (the on-screen controls hide themselves then). The world is rendered by the
// engine on both cores into a 512x300 canvas the OS scales 2x to the panel; the HUD is nv_gfx_*.
#include "game.h"

enum { ST_TITLE, ST_COUNT, ST_RACE, ST_DONE };

static int  s_gas_ms = 0;            // throttle held during the countdown (rocket start)
static int  s_state = ST_TITLE, s_it = 1, s_debug = 0, s_paused = 0, s_pad = 0;
static int  s_go_ms, s_state_ms, s_best_saved, s_prev_down, s_beeps, s_prev_pad;
static float s_cx, s_cy, s_cz, s_orbit, s_fov = 66;

#define W 512
#define H 300
#define C_WHITE  NV_RGB(255, 255, 255)
#define C_YELLOW NV_RGB(255, 214, 40)
#define C_SHADOW NV_RGB(10, 12, 20)
#define C_GREY   NV_RGB(170, 175, 190)
#define C_RED    NV_RGB(235, 50, 40)
#define C_GREEN  NV_RGB(60, 220, 90)
#define C_CYAN   NV_RGB(90, 200, 255)

// ---- text helpers ----------------------------------------------------------------------------------
int fmt_int(char *out, int v) {
    char t[12]; int n = 0, k = 0;
    if (v < 0) { out[k++] = '-'; v = -v; }
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) out[k++] = t[--n];
    out[k] = 0;
    return k;
}
void fmt_time(char *out, int ms) {
    if (ms < 0) ms = 0;
    const int m = ms / 60000, s = (ms / 1000) % 60, c = (ms / 10) % 100;
    int k = fmt_int(out, m);
    out[k++] = ':'; out[k++] = (char)('0' + s / 10); out[k++] = (char)('0' + s % 10);
    out[k++] = '.'; out[k++] = (char)('0' + c / 10); out[k++] = (char)('0' + c % 10); out[k] = 0;
}
static void cat(char *d, const char *s) { while (*d) d++; while ((*d++ = *s++)) {} }
static void text_sh(int x, int y, const char *s, int col, int sc) {   // text with a drop shadow
    nv_gfx_text(x + sc, y + sc, s, C_SHADOW, sc);
    nv_gfx_text(x, y, s, col, sc);
}
static void text_c(int y, const char *s, int col, int sc) { text_sh((W - nv_gfx_text_width(s, sc)) / 2, y, s, col, sc); }
static const char *T(const char *it, const char *en) { return s_it ? it : en; }

// ---- controls: touch zones, or the USB pad ------------------------------------------------------------
typedef struct { int x, y, w, h; } Rect;
static const Rect kLeft = { 6, 214, 66, 80 }, kRight = { 78, 214, 66, 80 };
static const Rect kBrake = { 366, 226, 60, 68 }, kGas = { 432, 206, 74, 88 }, kMap = { 350, 6, 84, 66 };
static int in_rect(const Rect *r, int x, int y) { return x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h; }

static void read_input(Input *in) {
    in->left = in->right = in->gas = in->brake = 0;
    const int n = nv_touch_count();
    for (int i = 0; i < n; i++) {
        int x, y;
        if (!nv_touch_at(i, &x, &y)) continue;
        // Generous hit zones: the whole lower-left quarter steers, the lower-right pedals.
        if (y > 170 && x < 150) { if (x < 75) in->left = 1; else in->right = 1; }
        if (y > 170 && x > 350) { if (x < 429) in->brake = 1; else in->gas = 1; }
    }
    // USB keyboard / gamepad: arrows or D-pad steer, A (or Up) accelerates, B (or Down) brakes
    // and reverses.
    if (s_pad & NV_PAD_LEFT) in->left = 1;
    if (s_pad & NV_PAD_RIGHT) in->right = 1;
    if (s_pad & (NV_PAD_A | NV_PAD_UP | NV_PAD_R)) in->gas = 1;
    if (s_pad & (NV_PAD_B | NV_PAD_DOWN | NV_PAD_L)) in->brake = 1;
}
static int pad_connected(void) { return (s_pad & (NV_PAD_KEYBOARD | NV_PAD_GAMEPAD)) != 0; }
static int pad_pressed(int bits) { return (s_pad & bits) && !(s_prev_pad & bits); }

static void outline(const Rect *r, int col) {
    nv_gfx_rect(r->x, r->y, r->w, 2, col); nv_gfx_rect(r->x, r->y + r->h - 2, r->w, 2, col);
    nv_gfx_rect(r->x, r->y, 2, r->h, col); nv_gfx_rect(r->x + r->w - 2, r->y, 2, r->h, col);
}
static void draw_controls(const Input *in) {
    if (pad_connected()) return;                         // a physical controller: no touch overlay
    const Rect *rs[4] = { &kLeft, &kRight, &kBrake, &kGas };
    const int on[4] = { in->left, in->right, in->brake, in->gas };
    for (int i = 0; i < 4; i++) {
        const Rect *r = rs[i];
        const int col = on[i] ? C_YELLOW : C_GREY, cx = r->x + r->w / 2, cy = r->y + r->h / 2;
        outline(r, C_SHADOW);
        Rect in2 = { r->x + 2, r->y + 2, r->w - 4, r->h - 4 };
        outline(&in2, col);
        if (i == 0) nv_gfx_tri(cx + 12, cy - 16, cx + 12, cy + 16, cx - 14, cy, col);
        if (i == 1) nv_gfx_tri(cx - 12, cy - 16, cx - 12, cy + 16, cx + 14, cy, col);
        if (i == 2) nv_gfx_rect(cx - 14, cy - 5, 28, 10, on[i] ? C_RED : col);
        if (i == 3) nv_gfx_tri(cx - 16, cy + 12, cx + 16, cy + 12, cx, cy - 18, on[i] ? C_GREEN : col);
    }
}

// ---- HUD ---------------------------------------------------------------------------------------------
static float s_mm_x0, s_mm_z0, s_mm_k;           // minimap transform
static void minimap_setup(void) {
    float minx = 1e9f, maxx = -1e9f, minz = 1e9f, maxz = -1e9f;
    for (int i = 0; i < TRACK_N; i++) {
        if (g_trk[i].x < minx) minx = g_trk[i].x;
        if (g_trk[i].x > maxx) maxx = g_trk[i].x;
        if (g_trk[i].z < minz) minz = g_trk[i].z;
        if (g_trk[i].z > maxz) maxz = g_trk[i].z;
    }
    const float kx = (kMap.w - 10) / (maxx - minx), kz = (kMap.h - 10) / (maxz - minz);
    s_mm_k = kx < kz ? kx : kz;
    s_mm_x0 = (minx + maxx) / 2; s_mm_z0 = (minz + maxz) / 2;
}
static void mm_pt(float x, float z, int *px, int *py) {
    *px = kMap.x + kMap.w / 2 + iroundf((x - s_mm_x0) * s_mm_k);
    *py = kMap.y + kMap.h / 2 - iroundf((z - s_mm_z0) * s_mm_k);   // +Z up on the map
}
static void draw_minimap(void) {
    int x0, y0, x1, y1;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < TRACK_N; i += 4) {
            mm_pt(g_trk[i].x, g_trk[i].z, &x0, &y0);
            mm_pt(g_trk[(i + 4) % TRACK_N].x, g_trk[(i + 4) % TRACK_N].z, &x1, &y1);
            if (pass == 0) nv_gfx_line(x0, y0, x1, y1, C_SHADOW);
            else nv_gfx_line(x0 - 1, y0 - 1, x1 - 1, y1 - 1, C_WHITE);
        }
    static const int dot[NCARS] = { 0xF8A3, 0x3B7F, 0x2E88, 0xFD20 };
    for (int i = NCARS - 1; i >= 0; i--) {
        mm_pt(g_car[i].x, g_car[i].z, &x0, &y0);
        nv_gfx_circle(x0, y0, i == 0 ? 4 : 3, C_SHADOW);
        nv_gfx_circle(x0, y0, i == 0 ? 3 : 2, dot[i]);
    }
}

static void draw_hud(int now) {
    char b[40], t[16];
    const Car *p = &g_car[0];
    int lap = p->lap + 1;
    if (lap < 1) lap = 1;
    if (lap > LAPS) lap = LAPS;
    b[0] = 0; cat(b, T("GIRO ", "LAP ")); fmt_int(t, lap); cat(b, t); cat(b, "/3");
    text_sh(8, 8, b, C_WHITE, 2);
    fmt_time(t, s_state == ST_RACE ? now - s_go_ms : 0);
    b[0] = 0; cat(b, T("TEMPO ", "TIME ")); cat(b, t);
    text_sh(8, 28, b, C_WHITE, 1);
    if (p->best_lap_ms > 0) {
        fmt_time(t, (int)p->best_lap_ms);
        b[0] = 0; cat(b, T("MIGLIORE ", "BEST ")); cat(b, t);
        text_sh(8, 38, b, C_YELLOW, 1);
    }
    // Coins: a little gold disc and the count (each one is +1.5% top speed).
    nv_gfx_circle(15, 57, 7, C_SHADOW); nv_gfx_circle(14, 56, 6, NV_RGB(250, 196, 40));
    nv_gfx_circle(13, 55, 3, NV_RGB(255, 236, 140));
    fmt_int(t, p->coins); b[0] = 0; cat(b, "x"); cat(b, t);
    text_sh(24, 52, b, C_WHITE, 1);
    fmt_int(t, car_position(0));
    text_sh(452, 8, t, C_YELLOW, 5);
    text_sh(484, 30, "/4", C_WHITE, 2);
    draw_minimap();
    fmt_int(t, iroundf(car_speed_kmh(0)));
    text_sh(W / 2 - nv_gfx_text_width(t, 4) / 2, 250, t, p->boost_t > 0 ? C_CYAN : C_WHITE, 4);
    text_sh(W / 2 - 12, 282, p->v < -10 ? "R" : "KM/H", C_GREY, 1);
    if (p->drift_t > 0.55f) {                             // mini-turbo charge meter
        const int lvl = p->drift_t > 1.3f ? 2 : 1, w = iroundf(clampf(p->drift_t / 1.3f, 0, 1) * 60);
        nv_gfx_rect(W / 2 - 31, 238, 62, 6, C_SHADOW);
        nv_gfx_rect(W / 2 - 30, 239, w, 4, lvl == 2 ? NV_RGB(255, 140, 20) : C_CYAN);
    }
    if (g_msg[0] && now < g_msg_until) text_c(96, g_msg, C_YELLOW, 3);
    if (s_debug) {
        b[0] = 0; cat(b, "3D MS "); fmt_int(t, vx_stat(VX_STAT_US) / 1000); cat(b, t);
        cat(b, " TRI "); fmt_int(t, vx_stat(VX_STAT_TRIS)); cat(b, t);
        cat(b, " PREP "); fmt_int(t, vx_stat(VX_STAT_PREP_US) / 1000); cat(b, t);
        text_sh(8, 66, b, C_GREEN, 1);
    }
}

// ---- camera --------------------------------------------------------------------------------------------
// Chase camera on a smoothed heading: bumps, spins and steering wobble move the kart, not the whole
// world. `swing` (radians) orbits the camera around the kart — the countdown flies it from the
// front of the grid round to behind the player.
static float s_ch;                                   // camera heading (smoothed)
static void camera_chase(float dt, int snap, float swing) {
    const Car *c = &g_car[0];
    if (snap) s_ch = c->heading;
    s_ch = wrap_pi(s_ch + wrap_pi(c->heading - s_ch) * clampf(dt * 3.2f, 0, 1));
    const float h = s_ch + swing, fx = sinf_(h), fz = cosf_(h);
    const float dist = 360 + 140 * clampf(swing, 0, 3.2f) / 3.2f;
    const float tx = c->x - fx * dist, ty = 158 + 40 * clampf(swing, 0, 3.2f) / 3.2f, tz = c->z - fz * dist;
    const float k = snap ? 1.0f : clampf(dt * 6.0f, 0, 1);
    s_cx += (tx - s_cx) * k; s_cy += (ty - s_cy) * k; s_cz += (tz - s_cz) * k;
    const float want = 64 + 16.0f * clampf(fabsf_(c->v) / 1250.0f, 0, 1) + (c->boost_t > 0 ? 9 : 0);
    s_fov += (want - s_fov) * clampf(dt * 4.0f, 0, 1);        // wider at speed, kick on boost
    vx_lens(iroundf(s_fov), 24, 16000);
    vx_camera(iroundf(s_cx), iroundf(s_cy), iroundf(s_cz), 0, 0, 0);
    const float lx = sinf_(s_ch), lz = cosf_(s_ch);
    const float ahead = 170.0f * (1.0f - clampf(swing, 0, 1));   // look at the kart while swinging round
    vx_look_at(iroundf(c->x + lx * ahead), 38, iroundf(c->z + lz * ahead));
}
static void camera_orbit(float cx, float cz, float r, float h, float dt) {
    s_orbit += dt * 0.25f;
    vx_lens(58, 24, 16000);
    vx_camera(iroundf(cx + cosf_(s_orbit) * r), iroundf(h), iroundf(cz + sinf_(s_orbit) * r), 0, 0, 0);
    vx_look_at(iroundf(cx), 60, iroundf(cz));
}

// ---- screens -------------------------------------------------------------------------------------------
static void save_best(void) {
    const int b = (int)g_car[0].best_lap_ms;
    if (b > 0 && (s_best_saved == 0 || b < s_best_saved)) {
        s_best_saved = b;
        nv_save("best.bin", &s_best_saved, 4);
    }
}

static void draw_title(int now) {
    text_c(40, "VERTICE GP", C_SHADOW, 6);
    text_sh((W - nv_gfx_text_width("VERTICE GP", 6)) / 2 - 2, 38, "VERTICE GP", C_YELLOW, 6);
    text_c(86, T("MOTORE 3D VERTICE - NUCLEO OS", "VERTICE 3D ENGINE - NUCLEO OS"), C_WHITE, 1);
    if ((now / 500) & 1)
        text_c(200, pad_connected() ? T("PREMI A PER CORRERE", "PRESS A TO RACE") : T("TOCCA PER CORRERE", "TAP TO RACE"),
               C_WHITE, 2);
    text_c(224, T("CURVA A FONDO = TURBO   PASSA SULLE FRECCE", "HOLD A TURN = TURBO   HIT THE ARROWS"), C_CYAN, 1);
    text_c(236, T("GAS SULL ULTIMO BIP = RAZZO   IN SCIA = TURBO", "GAS ON THE LAST BEEP = ROCKET   SLIPSTREAM = TURBO"),
           C_CYAN, 1);
    if (s_best_saved > 0) {
        char b[32], t[16];
        fmt_time(t, s_best_saved);
        b[0] = 0; cat(b, T("RECORD GIRO ", "LAP RECORD ")); cat(b, t);
        text_c(252, b, C_YELLOW, 1);
    }
}

static void draw_countdown(int now) {
    const int left = 3000 - (now - s_state_ms);
    const int n = left > 2000 ? 3 : left > 1000 ? 2 : left > 0 ? 1 : 0;
    for (int i = 0; i < 3; i++) {                  // start lights
        const int cx = W / 2 - 50 + i * 50, lit = (3 - n) > i;
        nv_gfx_circle(cx, 70, 17, C_SHADOW);
        nv_gfx_circle(cx, 70, 14, n == 0 ? C_GREEN : lit ? C_RED : NV_RGB(60, 20, 20));
    }
    char t[4];
    if (n) { fmt_int(t, n); text_c(100, t, C_WHITE, 7); }
    else text_c(100, T("VIA!", "GO!"), C_GREEN, 7);
}

static void draw_results(int now) {
    nv_gfx_rect(96, 60, 320, 170, C_SHADOW);
    nv_gfx_rect(98, 62, 316, 166, NV_RGB(24, 30, 60));
    const int pos = car_position(0);
    static const char *const kIt[4] = { "PRIMO!", "SECONDO", "TERZO", "QUARTO" };
    static const char *const kEn[4] = { "WINNER!", "SECOND", "THIRD", "FOURTH" };
    text_c(74, s_it ? kIt[pos - 1] : kEn[pos - 1], pos == 1 ? C_YELLOW : C_WHITE, 4);
    char b[40], t[16];
    fmt_time(t, (int)g_car[0].finish_ms - s_go_ms);
    b[0] = 0; cat(b, T("TEMPO GARA ", "RACE TIME ")); cat(b, t);
    text_c(124, b, C_WHITE, 2);
    fmt_time(t, (int)g_car[0].best_lap_ms);
    b[0] = 0; cat(b, T("GIRO VELOCE ", "FASTEST LAP ")); cat(b, t);
    text_c(148, b, C_YELLOW, 2);
    if (s_best_saved == (int)g_car[0].best_lap_ms) text_c(172, T("NUOVO RECORD!", "NEW RECORD!"), C_GREEN, 2);
    if (now - s_state_ms > 1500 && ((now / 500) & 1))
        text_c(204, pad_connected() ? T("PREMI A PER RIGIOCARE", "PRESS A TO PLAY AGAIN")
                                    : T("TOCCA PER RIGIOCARE", "TAP TO PLAY AGAIN"), C_WHITE, 1);
}

static void perf_log(int now) {
    static int last;
    if (now - last < 5000) return;
    last = now;
    char b[96], t[12];
    b[0] = 0; cat(b, "vxgp: render ");
    fmt_int(t, vx_stat(VX_STAT_US)); cat(b, t); cat(b, "us prep ");
    fmt_int(t, vx_stat(VX_STAT_PREP_US)); cat(b, t); cat(b, " tris ");
    fmt_int(t, vx_stat(VX_STAT_TRIS)); cat(b, t); cat(b, "/");
    fmt_int(t, vx_stat(VX_STAT_SCENE_TRIS)); cat(b, t);
    nv_log(NV_LOG_INFO, b);
}

static void events_feedback(int ev, int now) {
    if (ev & 2) nv_gfx_tone(1568, 40);                               // coin
    if (ev & 4) { nv_gfx_tone(880, 90); g_msg = "TURBO!"; g_msg_until = now + 700; }
    if (ev & 8) nv_gfx_tone(140, 60);                                // bump
    if (ev & 1) nv_gfx_tone(1046, 150);                              // lap
    if (ev & 16) { g_msg = T("RIPARTI!", "BACK ON TRACK!"); g_msg_until = now + 1200; }
    if (ev & 32) { nv_gfx_tone(988, 80); g_msg = T("SCIA!", "SLIPSTREAM!"); g_msg_until = now + 800; }
    if (g_car[0].wrong_t > 0.8f) { g_msg = T("CONTROMANO!", "WRONG WAY!"); g_msg_until = now + 200; }
}

NV_EXPORT("run")
void run(void) {
    char lang[8] = "";
    nv_lang(lang, sizeof lang);
    s_it = lang[0] == 'i' && lang[1] == 't';
    if (nv_load("best.bin", &s_best_saved, 4) != 4) s_best_saved = 0;
    world_build();
    cars_build();
    cars_grid();
    minimap_setup();
    s_state_ms = nv_millis();
    int last = s_state_ms;
    Input in = {0, 0, 0, 0};
    while (nv_gfx_present()) {
        const int now = nv_millis();
        float dt = (now - last) / 1000.0f;
        last = now;
        if (dt > 0.05f) dt = 0.05f;
        s_prev_pad = s_pad;
        s_pad = nv_gfx_pad();
        const int back = nv_gfx_back() || pad_pressed(NV_PAD_SELECT);
        if (back) {
            if (s_state == ST_TITLE) return;          // Back at the title: leave the app
            s_state = ST_TITLE; s_state_ms = now; s_paused = 0; cars_grid();
        }
        int tx, ty;
        const int down = nv_touch(&tx, &ty);
        const int touch_tap = !down && s_prev_down;
        const int tap = touch_tap || pad_pressed(NV_PAD_A | NV_PAD_START);
        s_prev_down = down;
        if (touch_tap && in_rect(&kMap, tx, ty) && s_state == ST_RACE) s_debug ^= 1;   // perf overlay
        if (s_state == ST_RACE && pad_pressed(NV_PAD_START)) s_paused ^= 1;

        switch (s_state) {
        case ST_TITLE:
            cars_update(&in, dt, 0, now);
            camera_orbit(g_trk[0].x + 700, g_trk[0].z, 1300, 420, dt);
            if (tap) { s_state = ST_COUNT; s_state_ms = now; s_beeps = 0; camera_chase(dt, 1, PI_F); }
            break;
        case ST_COUNT: {
            const int e = now - s_state_ms;
            const int want = e / 1000 + 1 > 4 ? 4 : e / 1000 + 1;   // beeps at 3, 2, 1, then GO
            while (s_beeps < want) { nv_gfx_tone(s_beeps < 3 ? 520 : 1040, s_beeps < 3 ? 140 : 320); s_beeps++; }
            // Rocket start: floor it on the last beep. Holding the throttle from the first light
            // floods the engine.
            read_input(&in);
            s_gas_ms = in.gas ? s_gas_ms + (int)(dt * 1000) : 0;
            cars_update(&in, dt, 0, now);                      // not racing: nobody moves yet
            {   // fly from in front of the grid round to behind the kart over the first 2.2 s;
                // while the throttle is held the engine revs (a tone rising with the hold)
                const float t = clampf(e / 2200.0f, 0, 1), sm = t * t * (3 - 2 * t);
                camera_chase(dt, 0, PI_F * (1.0f - sm));
                static int s_rev_at;
                if (in.gas && now - s_rev_at > 140) {
                    s_rev_at = now;
                    nv_gfx_tone(90 + (s_gas_ms > 1500 ? 1500 : s_gas_ms) / 6, 60);
                }
            }
            if (e >= 3000) {
                s_state = ST_RACE; s_go_ms = now;
                for (int i = 0; i < NCARS; i++) g_car[i].lap_start_ms = now;
                const int r = cars_launch(s_gas_ms);
                if (r > 0) { g_msg = T("PARTENZA RAZZO!", "ROCKET START!"); g_msg_until = now + 1200; nv_gfx_tone(1320, 120); }
                if (r < 0) { g_msg = T("MOTORE INGOLFATO", "ENGINE FLOODED"); g_msg_until = now + 1200; nv_gfx_tone(110, 200); }
                s_gas_ms = 0;
            }
            break;
        }
        case ST_RACE:
            if (s_paused) break;
            read_input(&in);
            events_feedback(cars_update(&in, dt, 1, now), now);
            camera_chase(dt, 0, 0);
            if (g_car[0].finished) {
                s_state = ST_DONE; s_state_ms = now; save_best();
                nv_gfx_tone(784, 160);
            }
            break;
        case ST_DONE:
            in.left = in.right = in.gas = in.brake = 0;
            cars_update(&in, dt, 1, now);
            camera_orbit(g_car[0].x, g_car[0].z, 520, 200, dt);
            if (((now - s_state_ms) / 120) % 3 == 0)
                vx_emit(g_fx_confetti, iroundf(g_car[0].x), 420, iroundf(g_car[0].z), 0, 240, 0, 320, 6);
            if (tap && now - s_state_ms > 1500) { s_state = ST_TITLE; s_state_ms = now; cars_grid(); }
            break;
        }

        vx_render();
        switch (s_state) {
        case ST_TITLE: draw_title(now); break;
        case ST_COUNT: draw_hud(now); draw_controls(&in); draw_countdown(now); break;
        case ST_RACE:
            draw_hud(now); draw_controls(&in);
            if (s_paused) text_c(120, T("PAUSA", "PAUSED"), C_WHITE, 4);
            break;
        case ST_DONE:  draw_results(now); break;
        }
        perf_log(now);
    }
}
