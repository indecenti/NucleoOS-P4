// Vertice GP — a 3D kart racer for NucleoOS, and the showcase of Vertice, the OS 3D engine.
// Three circuits, four drivers, three engine classes, three laps: boost pads, coins, drift
// mini-turbos, rocket starts, slipstream. The OS gestures are off in the game: every screen has its
// own on-screen buttons (and a pause key in the race), and a USB/Bluetooth pad or keyboard drives the
// menus with a focus ring. Art: Qwen-Image 2.1 paintings (art/gen.py); music: ACE-Step on the menus,
// the game's own mixed stream in the race (audio.c). The world is rendered by the engine on both cores
// into a 512x300 canvas the OS scales 2x to the panel; the HUD and menus are nv_gfx_*.
#include "game.h"

enum { ST_TITLE, ST_TRACK, ST_DRIVER, ST_RECORDS, ST_LOAD, ST_COUNT, ST_RACE, ST_DONE };

#define W 512
#define H 300
#define C_WHITE  NV_RGB(255, 255, 255)
#define C_YELLOW NV_RGB(255, 214, 40)
#define C_SHADOW NV_RGB(10, 12, 20)
#define C_GREY   NV_RGB(170, 175, 190)
#define C_RED    NV_RGB(235, 50, 40)
#define C_GREEN  NV_RGB(60, 220, 90)
#define C_CYAN   NV_RGB(90, 200, 255)
#define C_PANEL  NV_RGB(16, 22, 48)
#define C_EDGE   NV_RGB(80, 120, 220)

static int  s_state = ST_TITLE, s_it = 1, s_debug = 0, s_paused = 0, s_pad = 0, s_prev_pad;
static int  s_gas_ms = 0, s_go_ms, s_state_ms, s_beeps, s_art, s_final_lap;
static int  s_sel_track = 0, s_sel_driver = 0, s_sel_class = 1;
static int  s_built_track = -1, s_built_driver = -1, s_built_class = -1;
static int  s_music_at, s_music_part = -1;
static float s_cx, s_cy, s_cz, s_orbit, s_fov = 66;

static const char *const kDriver[NCARS] = { "LUCA", "MIA", "BRUNO", "ZOE" };
static const uint16_t kDot[NCARS] = { 0xF8A3, 0x3B7F, 0x2E88, 0xFD20 };

// Records per circuit: best lap and best race, in ms (0 = none). Preferences: last picks.
typedef struct { int32_t lap[NTRACKS], race[NTRACKS]; } Records;
static Records s_rec;
static int s_new_lap_rec, s_new_race_rec;

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
static void text_at_c(int cx, int y, const char *s, int col, int sc) { text_sh(cx - nv_gfx_text_width(s, sc) / 2, y, s, col, sc); }
static void text_c(int y, const char *s, int col, int sc) { text_at_c(W / 2, y, s, col, sc); }
static const char *T(const char *it, const char *en) { return s_it ? it : en; }

typedef struct { int x, y, w, h; } Rect;
static int in_rect(const Rect *r, int x, int y) { return x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h; }
static void frame(int x, int y, int w, int h, int t, int col) {
    nv_gfx_rect(x, y, w, t, col); nv_gfx_rect(x, y + h - t, w, t, col);
    nv_gfx_rect(x, y, t, h, col); nv_gfx_rect(x + w - t, y, t, h, col);
}
static void panel(int x, int y, int w, int h) {
    nv_gfx_rect(x + 3, y + 4, w, h, C_SHADOW);
    nv_gfx_rect(x, y, w, h, C_PANEL);
    nv_gfx_rect(x, y, w, h / 3, NV_RGB(22, 30, 62));              // a little sheen on top
    frame(x, y, w, h, 2, C_EDGE);
}

// ---- input: touch (tap = released finger), pad/keyboard ----------------------------------------------
static int s_down, s_prev_down, s_tx, s_ty, s_tap;
static int pad_connected(void) { return (s_pad & (NV_PAD_KEYBOARD | NV_PAD_GAMEPAD)) != 0; }
static int pressed(int bits) { return (s_pad & bits) && !(s_prev_pad & bits); }
static void poll_touch(void) {
    int x, y;
    s_prev_down = s_down;
    s_down = nv_touch(&x, &y);
    if (s_down) { s_tx = x; s_ty = y; }
    s_tap = !s_down && s_prev_down;
}

// ---- menu widgets with a focus ring for the pad ---------------------------------------------------------
// Every screen registers its widgets in draw order with a row number; the pad moves the focus
// left/right inside a row and up/down to the nearest widget of the next row (the layout of the
// previous frame), A presses it.
typedef struct { Rect r; int row; } Foc;
#define MAXFOC 20
static Foc s_foc[MAXFOC], s_foc_prev[MAXFOC];
static int s_nfoc, s_nfoc_prev, s_focus;
static int s_pad_act;                        // the focused widget was activated with A this frame

static void focus_begin(void) {
    for (int i = 0; i < s_nfoc; i++) s_foc_prev[i] = s_foc[i];
    s_nfoc_prev = s_nfoc; s_nfoc = 0;
    s_pad_act = 0;
    if (!s_nfoc_prev) return;
    if (s_focus >= s_nfoc_prev) s_focus = s_nfoc_prev - 1;
    const Foc *f = &s_foc_prev[s_focus];
    int best = -1, bd = 1 << 30;
    const int cx = f->r.x + f->r.w / 2;
    if (pressed(NV_PAD_LEFT) || pressed(NV_PAD_RIGHT)) {
        const int dir = pressed(NV_PAD_RIGHT) ? 1 : -1;
        for (int i = s_focus + dir; i >= 0 && i < s_nfoc_prev; i += dir)
            if (s_foc_prev[i].row == f->row) { best = i; break; }
    }
    if (pressed(NV_PAD_UP) || pressed(NV_PAD_DOWN)) {
        const int dir = pressed(NV_PAD_DOWN) ? 1 : -1;
        int row = -1;
        for (int i = 0; i < s_nfoc_prev; i++) {              // the nearest row in that direction
            const int d = (s_foc_prev[i].row - f->row) * dir;
            if (d > 0 && (row < 0 || d < (row - f->row) * dir)) row = s_foc_prev[i].row;
        }
        for (int i = 0; i < s_nfoc_prev && row >= 0; i++) {
            if (s_foc_prev[i].row != row) continue;
            const int d = s_foc_prev[i].r.x + s_foc_prev[i].r.w / 2 - cx, ad = d < 0 ? -d : d;
            if (ad < bd) { bd = ad; best = i; }
        }
    }
    if (best >= 0 && best != s_focus) { s_focus = best; sfx_click(); }
    if (pressed(NV_PAD_A)) s_pad_act = 1;
}
static void focus_reset(int i) { s_focus = i; s_nfoc = 0; s_nfoc_prev = 0; }
// Registers a focusable rect; returns 1 when it was tapped or pressed with A.
static int focusable(const Rect *r, int row) {
    const int id = s_nfoc < MAXFOC ? s_nfoc++ : MAXFOC - 1;
    s_foc[id].r = *r; s_foc[id].row = row;
    if (pad_connected() && id == s_focus) {
        frame(r->x - 4, r->y - 4, r->w + 8, r->h + 8, 3, C_YELLOW);
        frame(r->x - 5, r->y - 5, r->w + 10, r->h + 10, 1, C_SHADOW);
    }
    return (s_tap && in_rect(r, s_tx, s_ty)) || (s_pad_act && id == s_focus);
}
// A bevelled push button. accent = the colour strip on top. Returns 1 when activated.
static int ui_btn(int x, int y, int w, int h, const char *label, int accent, int row) {
    const Rect r = { x, y, w, h };
    const int held = s_down && in_rect(&r, s_tx, s_ty), dy = held ? 3 : 0;
    nv_gfx_rect(x + 2, y + 5, w, h, C_SHADOW);
    nv_gfx_rect(x, y + 4, w, h, NV_RGB(8, 12, 30));                          // lip
    nv_gfx_rect(x, y + dy, w, h, held ? NV_RGB(66, 90, 170) : NV_RGB(30, 42, 100));
    nv_gfx_rect(x, y + dy, w, h / 2, held ? NV_RGB(84, 110, 190) : NV_RGB(40, 56, 124));   // sheen
    nv_gfx_rect(x, y + dy, w, 3, accent);
    nv_gfx_rect(x, y + dy + h - 2, w, 2, NV_RGB(16, 22, 60));
    const int cx = x + w / 2, cy = y + dy + h / 2 + 1;
    if (label[0] == '<' && !label[1]) nv_gfx_tri(cx + 8, cy - 10, cx + 8, cy + 10, cx - 10, cy, C_WHITE);
    else if (label[0] == 'I' && label[1] == 'I' && !label[2]) {                                     // pause
        nv_gfx_rect(cx - 7, cy - 8, 5, 16, C_WHITE); nv_gfx_rect(cx + 2, cy - 8, 5, 16, C_WHITE);
    } else {
        const int sc = nv_gfx_text_width(label, 2) <= w - 10 ? 2 : 1;
        text_sh(x + (w - nv_gfx_text_width(label, sc)) / 2, y + dy + (h - 7 * sc) / 2, label, C_WHITE, sc);
    }
    const int hit = focusable(&r, row);
    if (hit) sfx_click();
    return hit;
}
// A row of buttons along the bottom; returns the index activated or -1.
static int ui_row(const char *const *labels, int n, int row) {
    const int w = n <= 2 ? 150 : n == 3 ? 132 : 112, gap = 10, total = n * w + (n - 1) * gap;
    int hit = -1;
    for (int i = 0; i < n; i++)
        if (ui_btn((W - total) / 2 + i * (w + gap), H - 42, w, 32, labels[i], i == n - 1 ? C_GREEN : C_CYAN, row)) hit = i;
    return hit;
}
// What the keys do, when a pad or keyboard is plugged in.
static void pad_hint(void) {
    if (!pad_connected()) return;
    const int kb = (s_pad & NV_PAD_KEYBOARD) && !(s_pad & NV_PAD_GAMEPAD);
    text_c(H - 54, kb ? T("FRECCE SCEGLI  INVIO OK  ESC INDIETRO", "ARROWS MOVE  ENTER OK  ESC BACK")
                      : T("CROCE SCEGLI  A OK  B INDIETRO", "D-PAD MOVE  A OK  B BACK"), C_GREY, 1);
}

// ---- music on the menus: the ACE-Step theme in 5 s parts, so the race stream can take over within
// one part (the OS plays one stream at a time and a WAV holds the speaker to its end) --------------------
#define MENU_PARTS 6
static void menu_music(int now) {
    if (audio_on()) return;
    if (s_music_part < 0 || now - s_music_at >= 4950) {
        s_music_part = (s_music_part + 1) % MENU_PARTS;
        s_music_at = now;
        char n[8] = "menu0";
        n[4] = (char)('0' + s_music_part);
        nv_sound(n);
    }
}

// ---- HUD ---------------------------------------------------------------------------------------------
static const Rect kLeft = { 6, 214, 66, 80 }, kRight = { 78, 214, 66, 80 };
static const Rect kBrake = { 366, 226, 60, 68 }, kGas = { 432, 206, 74, 88 }, kMap = { 350, 6, 84, 66 };
static const Rect kPause = { W / 2 - 18, 4, 36, 28 };

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
    // USB/Bluetooth pad or keyboard: arrows or D-pad steer, A (or Up, R) accelerates, B (or Down,
    // L) brakes and reverses.
    if (s_pad & NV_PAD_LEFT) in->left = 1;
    if (s_pad & NV_PAD_RIGHT) in->right = 1;
    if (s_pad & (NV_PAD_A | NV_PAD_UP | NV_PAD_R)) in->gas = 1;
    if (s_pad & (NV_PAD_B | NV_PAD_DOWN | NV_PAD_L)) in->brake = 1;
}

// On-screen pedals: bevelled faces that sink when pressed; steering arrows, a red brake, green gas.
static void pedal(const Rect *r, int on, int kind) {
    const int x = r->x, y = r->y, w = r->w, h = r->h, dy = on ? 3 : 0;
    nv_gfx_rect(x + 2, y + 5, w, h - 2, C_SHADOW);
    nv_gfx_rect(x, y + 4, w, h - 4, NV_RGB(12, 16, 28));
    const int face = kind == 3 ? (on ? NV_RGB(60, 170, 90) : NV_RGB(34, 90, 56))
                   : kind == 2 ? (on ? NV_RGB(200, 70, 60) : NV_RGB(100, 40, 40))
                               : (on ? NV_RGB(70, 110, 180) : NV_RGB(44, 54, 80));
    nv_gfx_rect(x, y + dy, w, h - 4, face);
    nv_gfx_rect(x, y + dy, w, 3, NV_RGB(200, 210, 230));
    for (int k = 0; k < 3 && kind >= 2; k++) nv_gfx_rect(x + 8, y + dy + 14 + k * 16, w - 16, 5, NV_RGB(20, 24, 36));   // ribs
    const int cx = x + w / 2, cy = y + dy + h / 2 - 2, col = on ? C_YELLOW : C_WHITE;
    if (kind == 0) nv_gfx_tri(cx + 12, cy - 16, cx + 12, cy + 16, cx - 14, cy, col);
    if (kind == 1) nv_gfx_tri(cx - 12, cy - 16, cx - 12, cy + 16, cx + 14, cy, col);
}
static void draw_controls(const Input *in) {
    if (pad_connected()) return;                         // a physical controller: no touch overlay
    pedal(&kLeft, in->left, 0); pedal(&kRight, in->right, 1);
    pedal(&kBrake, in->brake, 2); pedal(&kGas, in->gas, 3);
}

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
            if (pass == 0) { nv_gfx_line(x0 + 1, y0 + 1, x1 + 1, y1 + 1, C_SHADOW); nv_gfx_line(x0, y0 + 1, x1, y1 + 1, C_SHADOW); }
            else { nv_gfx_line(x0, y0, x1, y1, C_WHITE); nv_gfx_line(x0 + 1, y0, x1 + 1, y1, C_WHITE); }
        }
    mm_pt(g_trk[0].x, g_trk[0].z, &x0, &y0);                    // the start line
    nv_gfx_rect(x0 - 1, y0 - 4, 3, 9, C_RED);
    for (int i = NCARS - 1; i >= 0; i--) {
        mm_pt(g_car[i].x, g_car[i].z, &x0, &y0);
        nv_gfx_circle(x0, y0, i == 0 ? 5 : 3, C_SHADOW);
        nv_gfx_circle(x0, y0, i == 0 ? 4 : 2, kDot[g_car_driver[i]]);
    }
}

// Live standings on the left: position, colour chip and the driver's name.
static void draw_standings(void) {
    for (int i = 0; i < NCARS; i++) {
        const int p = car_position(i), y = 70 + (p - 1) * 13, me = i == 0;
        char t[4]; fmt_int(t, p);
        nv_gfx_rect(6, y - 2, me ? 74 : 66, 12, me ? NV_RGB(60, 50, 10) : NV_RGB(14, 18, 36));
        text_sh(9, y, t, me ? C_YELLOW : C_WHITE, 1);
        nv_gfx_rect(19, y, 6, 7, kDot[g_car_driver[i]]);
        text_sh(29, y, kDriver[g_car_driver[i]], me ? C_YELLOW : C_GREY, 1);
    }
}

static void draw_hud(int now) {
    char b[40], t[16];
    const Car *p = &g_car[0];
    int lap = p->lap + 1;
    if (lap < 1) lap = 1;
    if (lap > LAPS) lap = LAPS;
    b[0] = 0; cat(b, T("GIRO ", "LAP ")); fmt_int(t, lap); cat(b, t); cat(b, "/3");
    text_sh(8, 8, b, lap == LAPS && s_state == ST_RACE ? C_YELLOW : C_WHITE, 2);
    fmt_time(t, s_state == ST_RACE ? now - s_go_ms : 0);
    b[0] = 0; cat(b, T("TEMPO ", "TIME ")); cat(b, t);
    text_sh(8, 28, b, C_WHITE, 1);
    if (p->best_lap_ms > 0) {
        fmt_time(t, (int)p->best_lap_ms);
        b[0] = 0; cat(b, T("MIGLIORE ", "BEST ")); cat(b, t);
        text_sh(8, 38, b, C_YELLOW, 1);
    }
    // Coins: a little gold disc and the count (each one is +1.5% top speed).
    nv_gfx_circle(15, 55, 7, C_SHADOW); nv_gfx_circle(14, 54, 6, NV_RGB(250, 196, 40));
    nv_gfx_circle(13, 53, 3, NV_RGB(255, 236, 140));
    fmt_int(t, p->coins); b[0] = 0; cat(b, "x"); cat(b, t);
    text_sh(24, 51, b, C_WHITE, 1);
    draw_standings();
    fmt_int(t, car_position(0));
    text_sh(452, 8, t, car_position(0) == 1 ? C_YELLOW : C_WHITE, 5);
    text_sh(484, 30, "/4", C_WHITE, 2);
    draw_minimap();
    // Speedometer: a bar that fills, and the number
    const int kmh = iroundf(car_speed_kmh(0));
    const int bw = kmh * 120 / 260 > 120 ? 120 : kmh * 120 / 260;
    nv_gfx_rect(W / 2 - 62, 246, 124, 8, C_SHADOW);
    nv_gfx_rect(W / 2 - 60, 248, bw, 4, p->boost_t > 0 ? C_CYAN : bw > 100 ? C_YELLOW : C_GREEN);
    fmt_int(t, kmh);
    text_sh(W / 2 - nv_gfx_text_width(t, 3) / 2, 258, t, p->boost_t > 0 ? C_CYAN : C_WHITE, 3);
    text_sh(W / 2 - 12, 284, p->v < -10 ? "R" : "KM/H", C_GREY, 1);
    if (p->drift_t > 0.55f) {                             // mini-turbo charge meter
        const int lvl = p->drift_t > 1.3f ? 2 : 1, w = iroundf(clampf(p->drift_t / 1.3f, 0, 1) * 60);
        nv_gfx_rect(W / 2 - 31, 236, 62, 6, C_SHADOW);
        nv_gfx_rect(W / 2 - 30, 237, w, 4, lvl == 2 ? NV_RGB(255, 140, 20) : C_CYAN);
    }
    if (g_msg[0] && now < g_msg_until) text_c(96, g_msg, C_YELLOW, 3);
    if (s_debug) {
        b[0] = 0; cat(b, "3D MS "); fmt_int(t, vx_stat(VX_STAT_US) / 1000); cat(b, t);
        cat(b, " TRI "); fmt_int(t, vx_stat(VX_STAT_TRIS)); cat(b, t);
        cat(b, " PREP "); fmt_int(t, vx_stat(VX_STAT_PREP_US) / 1000); cat(b, t);
        text_sh(8, 128, b, C_GREEN, 1);
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

// ---- records and preferences ---------------------------------------------------------------------------------
static void save_prefs(void) {
    const int32_t p[3] = { s_sel_track, s_sel_driver, s_sel_class };
    nv_save("prefs.bin", p, sizeof p);
}
static void load_saves(void) {
    if (nv_load("records.bin", &s_rec, sizeof s_rec) != sizeof s_rec) {
        for (int i = 0; i < NTRACKS; i++) s_rec.lap[i] = s_rec.race[i] = 0;
        int32_t old = 0;                                        // 1.0: one lap record, circuit 0
        if (nv_load("best.bin", &old, 4) == 4 && old > 0) s_rec.lap[0] = old;
    }
    int32_t p[3];
    if (nv_load("prefs.bin", p, sizeof p) == sizeof p) {
        if (p[0] >= 0 && p[0] < NTRACKS) s_sel_track = p[0];
        if (p[1] >= 0 && p[1] < NCARS) s_sel_driver = p[1];
        if (p[2] >= 0 && p[2] < 3) s_sel_class = p[2];
    }
}
static void save_records(void) {
    const int t = g_track, lap = (int)g_car[0].best_lap_ms, race = (int)g_car[0].finish_ms - s_go_ms;
    s_new_lap_rec = lap > 0 && (s_rec.lap[t] == 0 || lap < s_rec.lap[t]);
    s_new_race_rec = race > 0 && (s_rec.race[t] == 0 || race < s_rec.race[t]);
    if (s_new_lap_rec) s_rec.lap[t] = lap;
    if (s_new_race_rec) s_rec.race[t] = race;
    if (s_new_lap_rec || s_new_race_rec) nv_save("records.bin", &s_rec, sizeof s_rec);
}

// ---- screens -------------------------------------------------------------------------------------------
static void backdrop(void) {                      // the menus: the title painting, or a gradient
    if (s_art) { nv_gfx_image("title", 0, 0, W, H); return; }
    for (int y = 0; y < H; y += 10) nv_gfx_rect(0, y, W, 10, NV_RGB(20 + y / 12, 40 + y / 10, 110 + y / 6));
}
static void heading(const char *s) {
    nv_gfx_rect(0, 0, W, 34, NV_RGB(12, 16, 40));
    nv_gfx_rect(0, 34, W, 2, C_EDGE);
    text_c(10, s, C_YELLOW, 2);
}

// Returns an action: 0 none, 1 race, 2 records, 3 quit.
static int screen_title(int now) {
    backdrop();
    text_c(29, "VERTICE GP", C_SHADOW, 6);
    text_sh((W - nv_gfx_text_width("VERTICE GP", 6)) / 2 - 2, 26, "VERTICE GP", C_YELLOW, 6);
    text_c(74, T("MOTORE 3D VERTICE - NUCLEO OS", "VERTICE 3D ENGINE - NUCLEO OS"), C_WHITE, 1);
    if (((now / 600) & 1) && !pad_connected()) text_c(H - 58, T("TOCCA GARA PER INIZIARE", "TAP RACE TO START"), C_WHITE, 1);
    pad_hint();
    const char *l[3] = { T("RECORD", "RECORDS"), T("ESCI", "QUIT"), T("GARA", "RACE") };
    const int hit = ui_row(l, 3, 0);
    return hit == 2 ? 1 : hit == 0 ? 2 : hit == 1 ? 3 : 0;
}

// Circuit + engine class. Returns -1 back, 1 next, 0 stay.
static int screen_track(void) {
    backdrop();
    heading(T("SCEGLI IL CIRCUITO", "CHOOSE THE CIRCUIT"));
    const int cw = 150, ch = 88, gap = 12, x0 = (W - 3 * cw - 2 * gap) / 2, y0 = 48;
    for (int t = 0; t < NTRACKS; t++) {
        const int x = x0 + t * (cw + gap), sel = t == s_sel_track;
        nv_gfx_rect(x - 3, y0 - 3, cw + 6, ch + 34, sel ? C_YELLOW : C_SHADOW);
        nv_gfx_rect(x, y0, cw, ch + 28, C_PANEL);
        char n[8] = "card0"; n[4] = (char)('0' + t);
        if (s_art) nv_gfx_image(n, x, y0, cw, ch);
        else nv_gfx_rect(x, y0, cw, ch, t == 0 ? NV_RGB(60, 140, 60) : t == 1 ? NV_RGB(190, 110, 60) : NV_RGB(220, 230, 245));
        text_at_c(x + cw / 2, y0 + ch + 5, track_name(t, s_it), sel ? C_YELLOW : C_WHITE, 1);
        char b[24], tt[12]; b[0] = 0;
        if (s_rec.lap[t]) { fmt_time(tt, s_rec.lap[t]); cat(b, T("GIRO ", "LAP ")); cat(b, tt); }
        else cat(b, "-");
        text_at_c(x + cw / 2, y0 + ch + 17, b, C_GREY, 1);
        const Rect r = { x, y0, cw, ch + 28 };
        if (focusable(&r, 0) && s_sel_track != t) { s_sel_track = t; sfx_click(); }
    }
    nv_gfx_rect(0, 166, W, 58, NV_RGB(12, 16, 40));                  // a strip under the class row
    nv_gfx_rect(0, 166, W, 1, C_EDGE); nv_gfx_rect(0, 223, W, 1, C_EDGE);
    text_c(171, T("CILINDRATA", "ENGINE CLASS"), C_WHITE, 1);
    static const char *const kCls[3] = { "50CC", "100CC", "150CC" };
    for (int k = 0; k < 3; k++) {
        const int x = W / 2 - 165 + k * 115;
        if (k == s_sel_class) nv_gfx_rect(x - 3, 183, 106, 36, C_YELLOW);
        if (ui_btn(x, 186, 100, 28, kCls[k], k == 0 ? C_GREEN : k == 1 ? C_YELLOW : C_RED, 1)) s_sel_class = k;
    }
    pad_hint();
    const char *l[2] = { "<", T("AVANTI", "NEXT") };
    const int hit = ui_row(l, 2, 2);
    if (hit == 0 || pressed(NV_PAD_B)) return -1;
    return hit == 1;
}

static void portrait(int d, int x, int y, int s) {
    if (s_art) { char n[8] = "drv0"; n[3] = (char)('0' + d); nv_gfx_image(n, x, y, s, s); return; }
    nv_gfx_circle(x + s / 2, y + s / 2, s / 2 - 4, kDot[d]);          // a helmet without the art
    nv_gfx_rect(x + s / 4, y + s / 2 - s / 10, s / 2, s / 6, NV_RGB(30, 40, 60));
}

static int screen_driver(void) {
    backdrop();
    heading(T("SCEGLI IL PILOTA", "CHOOSE YOUR DRIVER"));
    const int cw = 112, gap = 10, x0 = (W - 4 * cw - 3 * gap) / 2, y0 = 50;
    for (int d = 0; d < NCARS; d++) {
        const int x = x0 + d * (cw + gap), sel = d == s_sel_driver;
        nv_gfx_rect(x - 3, y0 - 3, cw + 6, 150, sel ? C_YELLOW : C_SHADOW);
        nv_gfx_rect(x, y0, cw, 144, sel ? NV_RGB(40, 50, 100) : C_PANEL);
        nv_gfx_rect(x, y0, cw, 4, kDot[d]);
        portrait(d, x + 8, y0 + 10, 96);
        nv_gfx_rect(x + 8, y0 + 110, cw - 16, 6, kDot[d]);            // the kart's colour
        text_at_c(x + cw / 2, y0 + 124, kDriver[d], sel ? C_YELLOW : C_WHITE, 2);
        const Rect r = { x, y0, cw, 144 };
        if (focusable(&r, 0) && s_sel_driver != d) { s_sel_driver = d; sfx_click(); }
    }
    char b[48]; b[0] = 0;
    cat(b, track_name(s_sel_track, s_it)); cat(b, "  -  ");
    cat(b, s_sel_class == 0 ? "50CC" : s_sel_class == 1 ? "100CC" : "150CC");
    nv_gfx_rect(0, 202, W, 18, NV_RGB(12, 16, 40));
    text_c(207, b, C_WHITE, 1);
    pad_hint();
    const char *l[2] = { "<", T("VIA!", "GO!") };
    const int hit = ui_row(l, 2, 1);
    if (hit == 0 || pressed(NV_PAD_B)) return -1;
    return hit == 1;
}

static int screen_records(void) {
    if (s_art) nv_gfx_image("podium", 0, 0, W, H); else backdrop();
    heading(T("RECORD", "RECORDS"));
    panel(40, 46, W - 80, 196);
    for (int t = 0; t < NTRACKS; t++) {
        const int y = 56 + t * 62;
        char n[8] = "card0"; n[4] = (char)('0' + t);
        if (s_art) nv_gfx_image(n, 52, y, 90, 53);
        else nv_gfx_rect(52, y, 90, 53, C_EDGE);
        frame(52, y, 90, 53, 1, C_SHADOW);
        text_sh(154, y + 4, track_name(t, s_it), C_YELLOW, 2);
        char b[40], tt[12];
        b[0] = 0; cat(b, T("GIRO VELOCE   ", "FASTEST LAP   "));
        if (s_rec.lap[t]) { fmt_time(tt, s_rec.lap[t]); cat(b, tt); } else cat(b, "-");
        text_sh(154, y + 26, b, C_WHITE, 1);
        b[0] = 0; cat(b, T("GARA          ", "RACE          "));
        if (s_rec.race[t]) { fmt_time(tt, s_rec.race[t]); cat(b, tt); } else cat(b, "-");
        text_sh(154, y + 38, b, C_WHITE, 1);
    }
    pad_hint();
    const char *l[1] = { T("INDIETRO", "BACK") };
    return ui_row(l, 1, 0) == 0 || pressed(NV_PAD_B) ? -1 : 0;
}

static void draw_countdown(int now) {
    const int left = 3000 - (now - s_state_ms);
    const int n = left > 2000 ? 3 : left > 1000 ? 2 : left > 0 ? 1 : 0;
    nv_gfx_rect(W / 2 - 84, 48, 168, 44, C_SHADOW);
    nv_gfx_rect(W / 2 - 82, 50, 164, 40, NV_RGB(24, 26, 34));
    for (int i = 0; i < 3; i++) {                  // start lights
        const int cx = W / 2 - 50 + i * 50, lit = (3 - n) > i;
        nv_gfx_circle(cx, 70, 16, C_SHADOW);
        nv_gfx_circle(cx, 70, 14, n == 0 ? C_GREEN : lit ? C_RED : NV_RGB(60, 20, 20));
        if (n == 0 || lit) nv_gfx_circle(cx - 4, 66, 4, n == 0 ? NV_RGB(180, 255, 190) : NV_RGB(255, 160, 150));
    }
    char t[4];
    if (n) { fmt_int(t, n); text_c(108, t, C_WHITE, 7); }
    else text_c(108, T("VIA!", "GO!"), C_GREEN, 7);
}

// Returns -1 menu, 1 race again.
static int screen_results(int now) {
    panel(56, 20, 400, 220);
    const int pos = car_position(0);
    static const char *const kIt[4] = { "VITTORIA!", "SECONDO", "TERZO", "QUARTO" };
    static const char *const kEn[4] = { "WINNER!", "SECOND", "THIRD", "FOURTH" };
    if (pos == 1 && s_art) nv_gfx_image("trophy", 66, 24, 44, 44);
    text_c(32, s_it ? kIt[pos - 1] : kEn[pos - 1], pos == 1 ? C_YELLOW : C_WHITE, 4);
    text_c(66, track_name(g_track, s_it), C_GREY, 1);
    // standings: position, portrait, name, time (or still racing)
    for (int i = 0; i < NCARS; i++) {
        const int p = car_position(i), y = 80 + (p - 1) * 30, me = i == 0, d = g_car_driver[i];
        if (me) nv_gfx_rect(72, y - 2, 368, 28, NV_RGB(60, 52, 14));
        char t[16]; fmt_int(t, p);
        text_sh(80, y + 6, t, me ? C_YELLOW : C_WHITE, 2);
        portrait(d, 100, y - 1, 26);
        nv_gfx_rect(130, y + 6, 6, 12, kDot[d]);
        text_sh(142, y + 6, kDriver[d], me ? C_YELLOW : C_WHITE, 2);
        if (g_car[i].finished) fmt_time(t, (int)g_car[i].finish_ms - s_go_ms);
        else { t[0] = 0; cat(t, T("IN GARA", "RACING")); }
        text_sh(432 - nv_gfx_text_width(t, 2), y + 6, t, me ? C_YELLOW : C_GREY, 2);
    }
    char b[48], t[16];
    fmt_time(t, (int)g_car[0].best_lap_ms);
    b[0] = 0; cat(b, T("GIRO VELOCE ", "FASTEST LAP ")); cat(b, t);
    if (s_new_lap_rec) cat(b, "  RECORD!");
    text_c(204, b, s_new_lap_rec ? C_GREEN : C_YELLOW, 1);
    if (s_new_race_rec) text_c(218, T("NUOVO RECORD DELLA GARA!", "NEW RACE RECORD!"), C_GREEN, 1);
    if (now - s_state_ms < 1200) return 0;                 // no accidental skip right at the line
    pad_hint();
    const char *l[2] = { T("MENU", "MENU"), T("RIGIOCA", "RACE AGAIN") };
    const int hit = ui_row(l, 2, 0);
    if (hit == 0 || pressed(NV_PAD_B)) return -1;
    return hit == 1;
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
    if (ev & 2) sfx_coin();
    if (ev & 4) { sfx_boost(); g_msg = "TURBO!"; g_msg_until = now + 700; }
    if (ev & 8) sfx_bump(g_car[0].v > 500);
    if (ev & 1) {
        if (g_car[0].lap == LAPS - 1 && !s_final_lap) {
            s_final_lap = 1; sfx_final_lap(); audio_music_tempo(1.08f);
            g_msg = T("ULTIMO GIRO!", "FINAL LAP!"); g_msg_until = now + 1600;
        } else if (g_car[0].lap < LAPS) sfx_lap();
    }
    if (ev & 16) { g_msg = T("RIPARTI!", "BACK ON TRACK!"); g_msg_until = now + 1200; }
    if (ev & 32) { sfx_whoosh(); g_msg = T("SCIA!", "SLIPSTREAM!"); g_msg_until = now + 800; }
    if (g_car[0].wrong_t > 0.8f) { g_msg = T("CONTROMANO!", "WRONG WAY!"); g_msg_until = now + 200; }
}

// Engine, squeal and the nearest rival for the mixer, every frame of the race.
static void feed_audio(const Input *in, int racing) {
    const Car *p = &g_car[0];
    float rpm = fabsf_(p->v) / 1250.0f;
    if (!racing && in->gas) rpm = 0.25f + (s_gas_ms > 1500 ? 1500 : s_gas_ms) / 1500.0f * 0.8f;   // revving on the grid
    const float skid = p->drift_t > 0.15f ? clampf(fabsf_(p->steer), 0, 1) : 0;
    float bd = 1e18f; int bi = -1;
    for (int i = 1; i < NCARS; i++) {
        const float dx = g_car[i].x - p->x, dz = g_car[i].z - p->z, d = dx * dx + dz * dz;
        if (d < bd) { bd = d; bi = i; }
    }
    const float d = sqrtf_(bd);
    audio_engine(rpm, in->gas, skid, bi > 0 ? clampf(1.0f - d / 1100.0f, 0, 1) : 0,
                 bi > 0 ? fabsf_(g_car[bi].v) / 1250.0f : 0);
}

static void go(int st, int now) {
    s_state = st; s_state_ms = now;
    focus_reset(st == ST_TITLE ? 2 : st == ST_TRACK ? s_sel_track : st == ST_DRIVER ? s_sel_driver : st == ST_DONE ? 1 : 0);
}
static void to_menu(int st, int now) {             // leaving the race: back to the menus' music
    audio_stop();
    s_paused = 0;
    s_music_part = -1;
    go(st, now);
}
// The scene for the current picks: rebuilt only when the circuit, the driver or the class changed.
static void build_race(void) {
    if (s_built_track != s_sel_track || s_built_driver != s_sel_driver || s_built_class != s_sel_class) {
        world_build(s_sel_track);
        cars_build(s_sel_driver, s_sel_class);
        minimap_setup();
        s_built_track = s_sel_track; s_built_driver = s_sel_driver; s_built_class = s_sel_class;
    }
    cars_grid();
    s_final_lap = 0;
    g_msg = "";
}

NV_EXPORT("run")
void run(void) {
    char lang[8] = "";
    nv_lang(lang, sizeof lang);
    s_it = lang[0] == 'i' && lang[1] == 't';
    load_saves();
    s_art = vx_texture_load("tr_pine", VX_TEX_KEY) >= 0;   // the painted set is installed
    build_race();
    s_state_ms = nv_millis();
    focus_reset(2);
    int last = s_state_ms, built_for = -1;
    Input in = {0, 0, 0, 0};
    while (nv_gfx_present()) {
        const int now = nv_millis();
        float dt = (now - last) / 1000.0f;
        last = now;
        if (dt > 0.05f) dt = 0.05f;
        s_prev_pad = s_pad;
        s_pad = nv_gfx_pad();
        poll_touch();
        const int in_race = s_state == ST_COUNT || s_state == ST_RACE;
        // An OS back request (a key, a pad's Back) acts like the screen's own back button.
        if (nv_gfx_back() || pressed(NV_PAD_SELECT)) {
            if (s_state == ST_TITLE) break;
            if (in_race) { s_paused = !s_paused; focus_reset(0); }
            else if (s_state == ST_DRIVER) go(ST_TRACK, now);
            else if (s_state == ST_DONE) { cars_grid(); to_menu(ST_TITLE, now); }
            else if (s_state != ST_LOAD) go(ST_TITLE, now);
        }

        focus_begin();
        audio_pump();
        if (!in_race && s_state != ST_LOAD && s_state != ST_DONE) menu_music(now);

        // ---- pause (race): resume, restart, menu, quit ----
        if (in_race && s_paused) {
            if (pressed(NV_PAD_START) || pressed(NV_PAD_B)) { s_paused = 0; audio_music_volume(1.0f); last = nv_millis(); continue; }
            audio_engine(0, 0, 0, 0, 0);
            audio_music_volume(0.35f);
            vx_render();
            panel(W / 2 - 110, 40, 220, 214);
            text_c(52, T("PAUSA", "PAUSED"), C_YELLOW, 3);
            static const char *const it[4] = { "RIPRENDI", "RICOMINCIA", "MENU", "ESCI" };
            static const char *const en[4] = { "RESUME", "RESTART", "MENU", "QUIT" };
            int hit = -1;
            for (int i = 0; i < 4; i++)
                if (ui_btn(W / 2 - 90, 88 + i * 40, 180, 30, T(it[i], en[i]), i == 0 ? C_GREEN : i == 3 ? C_RED : C_CYAN, i)) hit = i;
            if (hit == 0) { s_paused = 0; audio_music_volume(1.0f); last = nv_millis(); }
            if (hit == 1) { s_paused = 0; audio_music(-1); go(ST_LOAD, now); }
            if (hit == 2) { cars_grid(); to_menu(ST_TITLE, now); }
            if (hit == 3) break;
            continue;
        }

        switch (s_state) {
        case ST_TITLE: case ST_TRACK: case ST_DRIVER: case ST_RECORDS: {
            // Behind the painted menus nothing 3D is drawn; without the art the grid orbits.
            if (!s_art) {
                cars_update(&in, dt, 0, now);
                camera_orbit(g_trk[0].x + 700, g_trk[0].z, 1300, 420, dt);
                vx_render();
            }
            if (s_state == ST_TITLE) {
                const int r = screen_title(now);
                if (r == 1) go(ST_TRACK, now);
                if (r == 2) go(ST_RECORDS, now);
                if (r == 3) goto quit;
            } else if (s_state == ST_TRACK) {
                const int r = screen_track();
                if (r < 0) go(ST_TITLE, now);
                if (r > 0) go(ST_DRIVER, now);
            } else if (s_state == ST_DRIVER) {
                const int r = screen_driver();
                if (r < 0) go(ST_TRACK, now);
                if (r > 0) { save_prefs(); go(ST_LOAD, now); }
            } else if (screen_records() < 0) {
                go(ST_TITLE, now);
            }
            break;
        }
        case ST_LOAD: {
            // One frame shows the circuit's card, the next builds it; then the race stream needs the
            // speaker, which the menu theme holds to the end of its current 5 s part.
            const int e = now - s_state_ms;
            if (s_art) { char n[8] = "card0"; n[4] = (char)('0' + s_sel_track); nv_gfx_image(n, 0, 0, W, H); }
            else nv_gfx_clear(C_PANEL);
            panel(W / 2 - 150, H - 70, 300, 46);
            text_c(H - 62, track_name(s_sel_track, s_it), C_YELLOW, 2);
            text_c(H - 40, T("CARICAMENTO...", "LOADING..."), C_WHITE, 1);
            if (e > 30 && built_for != s_state_ms) { built_for = s_state_ms; build_race(); }
            if (built_for == s_state_ms && (audio_start() || e > 7000)) {
                audio_music(g_track);
                audio_music_volume(1.0f);
                audio_music_tempo(1.0f);
                s_paused = 0; s_beeps = 0; s_gas_ms = 0;
                camera_chase(dt, 1, PI_F);
                go(ST_COUNT, nv_millis());
            }
            break;
        }
        case ST_COUNT: {
            const int e = now - s_state_ms;
            const int want = e / 1000 + 1 > 4 ? 4 : e / 1000 + 1;   // beeps at 3, 2, 1, then GO
            while (s_beeps < want) { sfx_beep(s_beeps >= 3); s_beeps++; }
            // Rocket start: floor it on the last beep. Holding the throttle from the first light
            // floods the engine.
            read_input(&in);
            s_gas_ms = in.gas ? s_gas_ms + (int)(dt * 1000) : 0;
            cars_update(&in, dt, 0, now);                      // not racing: nobody moves yet
            feed_audio(&in, 0);
            {   // fly from in front of the grid round to behind the kart over the first 2.2 s
                const float t = clampf(e / 2200.0f, 0, 1), sm = t * t * (3 - 2 * t);
                camera_chase(dt, 0, PI_F * (1.0f - sm));
            }
            vx_render();
            draw_hud(now); draw_controls(&in); draw_countdown(now);
            if (ui_btn(kPause.x, kPause.y, kPause.w, kPause.h, "II", C_GREY, 9) || pressed(NV_PAD_START)) {
                s_paused = 1; focus_reset(0);
            }
            if (e >= 3000) {
                s_state = ST_RACE; s_go_ms = now;
                for (int i = 0; i < NCARS; i++) g_car[i].lap_start_ms = now;
                const int r = cars_launch(s_gas_ms);
                if (r > 0) { g_msg = T("PARTENZA RAZZO!", "ROCKET START!"); g_msg_until = now + 1200; sfx_rocket(); }
                if (r < 0) { g_msg = T("MOTORE INGOLFATO", "ENGINE FLOODED"); g_msg_until = now + 1200; sfx_flood(); }
                s_gas_ms = 0;
            }
            break;
        }
        case ST_RACE: {
            read_input(&in);
            const int ev = cars_update(&in, dt, 1, now);
            events_feedback(ev, now);
            feed_audio(&in, 1);
            camera_chase(dt, (ev & 16) != 0, 0);           // respawned: camera straight behind
            vx_render();
            draw_hud(now); draw_controls(&in);
            if (ui_btn(kPause.x, kPause.y, kPause.w, kPause.h, "II", C_GREY, 9) || pressed(NV_PAD_START)) {
                s_paused = 1; focus_reset(0);
            }
            if (s_tap && in_rect(&kMap, s_tx, s_ty)) s_debug ^= 1;   // perf overlay
            if (g_car[0].finished) {
                save_records();
                audio_stop();                              // the speaker goes to the fanfare
                nv_sound(car_position(0) == 1 ? "win" : "finish");
                go(ST_DONE, now);
            }
            break;
        }
        case ST_DONE: {
            in.left = in.right = in.gas = in.brake = 0;
            cars_update(&in, dt, 1, now);
            camera_orbit(g_car[0].x, g_car[0].z, 520, 200, dt);
            if (car_position(0) == 1 && ((now - s_state_ms) / 120) % 3 == 0)
                vx_emit(g_fx_confetti, iroundf(g_car[0].x), 420, iroundf(g_car[0].z), 0, 240, 0, 320, 6);
            vx_render();
            const int r = screen_results(now);
            if (r < 0) { cars_grid(); to_menu(ST_TITLE, now); }
            if (r > 0) { s_music_part = -1; go(ST_LOAD, now); }
            break;
        }
        }
        perf_log(now);
    }
quit:
    audio_stop();
}
