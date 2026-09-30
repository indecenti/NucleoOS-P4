// Vertice Bass — arcade lake fishing for NucleoOS on the Vertice 3D engine.
// A tournament of six lakes: each stage has a clock and a weight quota. Pick a lure, aim and cast
// from the boat, work the lure under water (reel / pause / twitch) until a fish strikes, set the
// hook, then fight it — rod against its runs, reel without snapping the line, catch the jumps.
// Weigh-in at the bell: make the quota to move on. The ten biggest fish ever go on the record
// wall. Touch, USB keyboard or gamepad; Italian or English from the system language.
#include "bass.h"

enum { ST_TITLE, ST_RECORDS, ST_STAGE, ST_LURE, ST_AIM, ST_CAST, ST_RETRIEVE, ST_STRIKE, ST_FIGHT,
       ST_CATCH, ST_LOST, ST_WEIGH, ST_OVER, ST_INTRO, ST_SELECT };

#define C_WHITE  C565(255, 255, 255)
#define C_YELLOW C565(255, 214, 40)
#define C_SHADOW C565(8, 12, 22)
#define C_GREY   C565(170, 175, 190)
#define C_RED    C565(235, 50, 40)
#define C_GREEN  C565(60, 220, 90)
#define C_CYAN   C565(90, 200, 255)
#define C_PANEL  C565(14, 30, 54)
#define C_EDGE   C565(70, 150, 220)

static int s_it = 1, s_state = ST_TITLE, s_state_ms, s_menu, s_pad, s_prev_pad, s_prev_down;
static int s_stage, s_loop, s_lure, s_time_ms, s_catches, s_msg_until;
static float s_total, s_run_total, s_stage_best, s_quota;
static const char *s_msg = "";
static int s_best_run100;
static Record s_rec[NRECORDS];
static int s_new_rank = -1;

// ---- text ------------------------------------------------------------------------------------------------
static const char *T(const char *it, const char *en) { return s_it ? it : en; }
static void cat(char *d, const char *s) { while (*d) d++; while ((*d++ = *s++)) {} }
static int fmt_int(char *out, int v) {
    char t[12]; int n = 0, k = 0;
    if (v < 0) { out[k++] = '-'; v = -v; }
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) out[k++] = t[--n];
    out[k] = 0;
    return k;
}
static void fmt_kg(char *out, float kg) {      // 12.34 KG
    int c = iroundf(kg * 100);
    int k = fmt_int(out, c / 100);
    out[k++] = '.'; out[k++] = (char)('0' + (c / 10) % 10); out[k++] = (char)('0' + c % 10);
    out[k++] = ' '; out[k++] = 'K'; out[k++] = 'G'; out[k] = 0;
}
static void fmt_clock(char *out, int ms) {     // m:ss
    if (ms < 0) ms = 0;
    const int s = (ms + 999) / 1000;
    int k = fmt_int(out, s / 60);
    out[k++] = ':'; out[k++] = (char)('0' + (s % 60) / 10); out[k++] = (char)('0' + s % 10); out[k] = 0;
}
static void text_sh(int x, int y, const char *s, int col, int sc) {
    nv_gfx_text(x + (sc > 1 ? 2 : 1), y + (sc > 1 ? 2 : 1), s, C_SHADOW, sc);
    nv_gfx_text(x, y, s, col, sc);
}
static void text_c(int y, const char *s, int col, int sc) { text_sh((W - nv_gfx_text_width(s, sc)) / 2, y, s, col, sc); }
static void panel(int x, int y, int w, int h) {
    nv_gfx_rect(x, y, w, h, C_PANEL);
    nv_gfx_rect(x, y, w, 2, C_EDGE); nv_gfx_rect(x, y + h - 2, w, 2, C_EDGE);
    nv_gfx_rect(x, y, 2, h, C_EDGE); nv_gfx_rect(x + w - 2, y, 2, h, C_EDGE);
}
// Painted art (tools/qwen_assets.py, Qwen-Image): full-screen scenes and portraits in img/.
static void art(const char *name) { nv_gfx_image(name, 0, 0, W, H); }
// The painted fish by size: a small one, the regular one, or the big "monster" painting.
static void fish_art_kg(int sp, float kg, int x, int y, int w, int h) {
    const Species *S = &g_species[sp];
    const float q = (kg - S->kg_min) / (S->kg_max - S->kg_min + 1e-3f);
    char n[10] = "fish0";
    n[4] = (char)('0' + sp);
    if (q < 0.12f) { n[5] = '_'; n[6] = 's'; n[7] = 0; }
    else if (q >= 0.5f) { n[5] = '_'; n[6] = 'b'; n[7] = 0; }
    nv_gfx_image(n, x, y, w, h);
}
static int size_class(int sp, float kg) {                  // 0 small fry, 1 keeper, 2 big, 3 monster
    const Species *S = &g_species[sp];
    const float q = (kg - S->kg_min) / (S->kg_max - S->kg_min + 1e-3f);
    return q < 0.12f ? 0 : q < 0.35f ? 1 : q < 0.7f ? 2 : 3;
}
static void pond_art(int st) {                             // the catch backdrop: day, sunset, night, autumn
    static const char pond[NSTAGES] = { '0', '1', '2', '0', '3', '0' };
    char n[8] = "pond0";
    n[4] = pond[st];
    nv_gfx_image(n, 0, 0, W, H);
}
static void lake_art(int st) {
    char n[8] = "lake0";
    n[4] = (char)('0' + st);
    art(n);
}
static const char *sp_name(int sp) { return T(g_species[sp].name_it, g_species[sp].name_en); }
static const char *lake_name(int st) { return T(g_stage[st].name_it, g_stage[st].name_en); }
static void msg(const char *m, int now, int ms) { s_msg = m; s_msg_until = now + ms; }

// ---- sound -------------------------------------------------------------------------------------------------
// Synthesised WAVs in snd/ (tools/gen_bass_sfx.py). The reel ratchet is rate-limited.
static void sfx(const char *name) { nv_sound(name); }
static int s_reel_at;
static void sfx_reel(int now) { if (now - s_reel_at > 150) { s_reel_at = now; sfx("reel"); } }
static void snd_splash(void) { sfx("splash"); }
static void snd_click(void)  { sfx("click"); }
static void snd_strike(void) { sfx("strike"); }
static void snd_fanfare(void) { sfx("catch"); }

// ---- input: touch zones or pad -------------------------------------------------------------------------------
typedef struct { int left, right, up, down, a, b, a_hit, b_hit, d_hit, tap, tx, ty; } Pad;
static Pad s_in;
typedef struct { int x, y, w, h; } Rect;
static const Rect kB = { 362, 222, 64, 72 }, kA = { 432, 204, 74, 90 };
static int in_rect(const Rect *r, int x, int y) { return x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h; }
static int pad_connected(void) { return (s_pad & (NV_PAD_KEYBOARD | NV_PAD_GAMEPAD)) != 0; }

// Analog reel (ABI v11 controllers): how far the right trigger (R2/RT) is pressed, 0..1. The
// harder you squeeze, the faster the reel turns. Buttons, keys and touch reel at full speed.
static float s_reel_amt;
static int s_have_pad;                    // a v11 controller is connected (rumble, triggers)
static void rumble(int low, int high, int ms) { if (s_have_pad) nv_pad_rumble(0, low, high, ms); }
static void read_input(void) {
    static int prev_a, prev_b;
    Pad p = { 0 };
    const int n = nv_touch_count();
    for (int i = 0; i < n; i++) {
        int x, y;
        if (!nv_touch_at(i, &x, &y)) continue;
        if (y > 190 && x < 150) {                 // d-pad: left | up/down | right
            if (x < 50) p.left = 1;
            else if (x >= 100) p.right = 1;
            else if (y < 244) p.up = 1;
            else p.down = 1;
        }
        if (in_rect(&kA, x, y)) p.a = 1;
        if (in_rect(&kB, x, y)) p.b = 1;
    }
    int tx, ty;
    const int down = nv_touch(&tx, &ty);
    if (!down && s_prev_down) { p.tap = 1; p.tx = s_in.tx; p.ty = s_in.ty; } else if (down) { p.tx = tx; p.ty = ty; }
    s_prev_down = down;
    s_prev_pad = s_pad;
    s_pad = nv_gfx_pad();
    if (s_pad & NV_PAD_LEFT) p.left = 1;
    if (s_pad & NV_PAD_RIGHT) p.right = 1;
    if (s_pad & NV_PAD_UP) p.up = 1;
    if (s_pad & NV_PAD_DOWN) p.down = 1;
    if (s_pad & (NV_PAD_A | NV_PAD_R)) p.a = 1;
    if (s_pad & (NV_PAD_B | NV_PAD_L)) p.b = 1;
    s_reel_amt = p.a ? 1.0f : 0.0f;
    s_have_pad = 0;
    if (nv_pad_count() > 0) {                 // player 1: triggers and the left stick
        nv_pad_state_t st;
        if (nv_pad_state(0, &st, sizeof st) > 0) {
            s_have_pad = st.rumble;
            const float rt = st.rt / 32767.0f;
            if (rt > 0.06f) { s_reel_amt = rt; p.a = 1; }       // analog reel overrides the digital bit
            else if (!(st.buttons & NV_PADB_A) && (s_pad & NV_PAD_R) && !(s_pad & NV_PAD_A)) { p.a = 0; s_reel_amt = 0; }
            if (st.lt > 16000) p.b = 1;                          // L2: give line
            if (st.lx < -14000) p.left = 1;
            if (st.lx > 14000) p.right = 1;
            if (st.ly < -16000) p.up = 1;
            if (st.ly > 16000) p.down = 1;
        }
    }
    static int prev_d;
    p.a_hit = p.a && !prev_a; p.b_hit = p.b && !prev_b; p.d_hit = p.down && !prev_d;
    prev_a = p.a; prev_b = p.b; prev_d = p.down;
    if (!p.tx && !p.ty) { p.tx = s_in.tx; p.ty = s_in.ty; }
    s_in = p;
}
static int pressed(int bit) { return (s_pad & bit) && !(s_prev_pad & bit); }
// "Confirm": A on a pad, or a tap anywhere not on the on-screen buttons.
static int confirm(void) {
    return pressed(NV_PAD_A | NV_PAD_START) || (s_in.tap && !in_rect(&kB, s_in.tx, s_in.ty));
}

static void button(const Rect *r, int on, const char *label, int col, const char *icon) {
    const int cx = r->x + r->w / 2, rad = (r->w < r->h ? r->w : r->h) / 2 - 3;
    const int cy0 = r->y + r->h / 2 - 2, cy = cy0 + (on ? 3 : 0);          // pressed: the face sinks
    nv_gfx_circle(cx + 2, cy0 + 6, rad, C_SHADOW);
    nv_gfx_circle(cx, cy0 + 4, rad, C565(10, 14, 24));                     // the lip under the face
    nv_gfx_circle(cx, cy, rad, col);                                       // coloured rim
    nv_gfx_circle(cx, cy, rad - 3, on ? C565(70, 96, 140) : C565(24, 34, 54));
    nv_gfx_circle(cx - rad / 3, cy - rad / 3, rad / 3, on ? C565(96, 124, 170) : C565(36, 48, 72));   // gloss
    if (icon) {
        const int s = on ? 40 : 36;
        nv_gfx_image(icon, cx - s / 2, cy - s / 2 - 6, s, s);
        text_sh(cx - nv_gfx_text_width(label, 1) / 2, cy + rad - 14, label, on ? C_YELLOW : C_GREY, 1);
    } else {
        text_sh(cx - nv_gfx_text_width(label, 1) / 2, cy - 3, label, on ? C_WHITE : C_GREY, 1);
    }
}
// The d-pad: a bevelled cross (horizontal bar x 4..146, vertical bar y 196..292, centre 75,244).
// Pressed arms light up; with mode 1 (left/right only) up and down are drawn dimmed.
static void draw_dpad(int mode) {
    const uint16_t lip = C565(12, 16, 26), body = C565(46, 54, 74), top = C565(78, 90, 116), lit = C565(70, 120, 190);
    nv_gfx_rect(6, 228, 142, 46, C_SHADOW); nv_gfx_rect(54, 200, 46, 98, C_SHADOW);        // shadow
    nv_gfx_rect(4, 226, 142, 44, lip); nv_gfx_rect(52, 200, 46, 96, lip);                   // lip
    nv_gfx_rect(4, 222, 142, 44, body); nv_gfx_rect(52, 196, 46, 96, body);                 // face
    if (s_in.left) nv_gfx_rect(4, 222, 46, 44, lit);
    if (s_in.right) nv_gfx_rect(100, 222, 46, 44, lit);
    if (s_in.up && mode == 2) nv_gfx_rect(52, 196, 46, 26, lit);
    if (s_in.down && mode >= 2) nv_gfx_rect(52, 266, 46, 26, lit);
    nv_gfx_rect(4, 222, 48, 3, top); nv_gfx_rect(98, 222, 48, 3, top); nv_gfx_rect(52, 196, 46, 3, top);   // bevel
    nv_gfx_circle(75, 244, 13, C565(30, 36, 52)); nv_gfx_circle(75, 244, 9, C565(40, 48, 66));             // hub
    const uint16_t on = C_YELLOW, idle = C565(206, 212, 226), off = C565(74, 82, 102);
    nv_gfx_tri(36, 232, 36, 256, 16, 244, s_in.left ? on : idle);
    nv_gfx_tri(114, 232, 114, 256, 134, 244, s_in.right ? on : idle);
    nv_gfx_tri(63, 218, 87, 218, 75, 202, mode != 2 ? off : s_in.up ? on : idle);
    nv_gfx_tri(63, 270, 87, 270, 75, 286, mode < 2 ? off : s_in.down ? on : idle);
}

// ---- keyboard / gamepad: the touch controls hide and a strip of key badges says what each key does
// (keyboard names on a keyboard, the pad's buttons on a gamepad).
enum { K_A, K_B, K_LR, K_UD, K_DPAD, K_START, K_SELECT };
static const char *key_name(int k) {
    const int kb = (s_pad & NV_PAD_KEYBOARD) && !(s_pad & NV_PAD_GAMEPAD);
    switch (k) {
    case K_A: return kb ? T("SPAZIO", "SPACE") : "A";
    case K_B: return kb ? "Z" : "B";
    case K_START: return kb ? "P" : "START";
    case K_SELECT: return kb ? "ESC" : "SELECT";
    }
    return 0;
}
static int key_w(int k) { const char *t = key_name(k); return t ? nv_gfx_text_width(t, 1) + 10 : 17; }
static void key_badge(int x, int y, int k) {
    const char *t = key_name(k);
    if (t) {
        const int w = key_w(k);
        const uint16_t col = k == K_A ? C565(40, 170, 80) : k == K_B ? C565(200, 60, 50) : C565(70, 80, 104);
        nv_gfx_rect(x + 1, y + 2, w, 14, C_SHADOW);
        nv_gfx_rect(x, y, w, 14, col);
        nv_gfx_rect(x, y, w, 2, C565(230, 240, 255));
        text_sh(x + 5, y + 4, t, C_WHITE, 1);
        return;
    }
    const int cx = x + 8, cy = y + 7;                      // a small cross, the used arms yellow
    nv_gfx_rect(cx - 7, cy - 2, 15, 5, C565(70, 80, 104)); nv_gfx_rect(cx - 2, cy - 7, 5, 15, C565(70, 80, 104));
    if (k != K_UD) { nv_gfx_rect(cx - 7, cy - 2, 4, 5, C_YELLOW); nv_gfx_rect(cx + 4, cy - 2, 4, 5, C_YELLOW); }
    if (k != K_LR) { nv_gfx_rect(cx - 2, cy - 7, 5, 4, C_YELLOW); nv_gfx_rect(cx - 2, cy + 4, 5, 4, C_YELLOW); }
}
static void pad_hints(const int *keys, const char *const *labels, int n) {
    int total = 0;
    for (int i = 0; i < n; i++) total += key_w(keys[i]) + 5 + nv_gfx_text_width(labels[i], 1) + (i + 1 < n ? 14 : 0);
    int x = (W - total) / 2;
    panel(x - 8, H - 24, total + 16, 21);
    for (int i = 0; i < n; i++) {
        key_badge(x, H - 20, keys[i]);
        x += key_w(keys[i]) + 5;
        text_sh(x, H - 16, labels[i], C_WHITE, 1);
        x += nv_gfx_text_width(labels[i], 1) + 14;
    }
}

static const char *s_icon_a, *s_icon_b;   // painted icons for the next draw_controls
static const char *s_arrows_label;        // what the arrows do, for the pad hints
static void draw_controls(const char *a_label, const char *b_label, int arrows) {
    if (pad_connected()) {
        int k[4], n = 0;
        const char *l[4];
        if (arrows) { k[n] = arrows >= 2 ? K_DPAD : K_LR; l[n++] = s_arrows_label ? s_arrows_label : T("MUOVI", "MOVE"); }
        if (a_label) { k[n] = K_A; l[n++] = a_label; }
        if (b_label) { k[n] = K_B; l[n++] = b_label; }
        k[n] = K_START; l[n++] = T("PAUSA", "PAUSE");
        pad_hints(k, l, n);
        s_icon_a = s_icon_b = 0; s_arrows_label = 0;
        return;
    }
    if (arrows) draw_dpad(arrows);
    if (a_label) button(&kA, s_in.a, a_label, C_GREEN, s_icon_a);
    if (b_label) button(&kB, s_in.b, b_label, C_CYAN, s_icon_b);
    s_icon_a = s_icon_b = 0; s_arrows_label = 0;
}

// ---- menu buttons: explicit on-screen keys for every screen (the OS gestures are off in the game) --------
typedef struct { Rect r; const char *label; int col; } Btn;
static int ui_btn(int x, int y, int w, int h, const char *label, int accent) {
    const Rect r = { x, y, w, h };
    const int held = s_prev_down && in_rect(&r, s_in.tx, s_in.ty), dy = held ? 3 : 0;
    nv_gfx_rect(x + 2, y + 5, w, h, C_SHADOW);
    nv_gfx_rect(x, y + 4, w, h, C565(8, 16, 32));                          // lip
    nv_gfx_rect(x, y + dy, w, h, held ? C565(66, 104, 164) : C565(26, 52, 92));
    nv_gfx_rect(x, y + dy, w, h / 2, held ? C565(84, 124, 186) : C565(36, 68, 116));   // sheen
    nv_gfx_rect(x, y + dy, w, 3, accent);
    nv_gfx_rect(x, y + dy + h - 2, w, 2, C565(14, 30, 58));
    const int cx = x + w / 2, cy = y + dy + h / 2 + 1;
    if (label[0] == '<' && !label[1]) nv_gfx_tri(cx + 8, cy - 10, cx + 8, cy + 10, cx - 10, cy, C_WHITE);
    else if (label[0] == '>' && !label[1]) nv_gfx_tri(cx - 8, cy - 10, cx - 8, cy + 10, cx + 10, cy, C_WHITE);
    else text_sh(x + (w - nv_gfx_text_width(label, 2)) / 2, y + dy + (h - 14) / 2 + 1, label, C_WHITE, 2);
    return s_in.tap && in_rect(&r, s_in.tx, s_in.ty);
}
// A row of up to four buttons along the bottom; returns the index tapped or -1.
static int ui_row(const char *const *labels, int n) {
    if (pad_connected()) return -1;                        // a keyboard or pad: hints instead (pad_hints)
    const int w = n <= 2 ? 150 : n == 3 ? 130 : 112, gap = 8, total = n * w + (n - 1) * gap;
    int hit = -1;
    for (int i = 0; i < n; i++)
        if (ui_btn((W - total) / 2 + i * (w + gap), H - 40, w, 32, labels[i], i == 0 ? C_GREEN : C_CYAN)) hit = i;
    return hit;
}
static const Rect kPause = { W / 2 + 62, 6, 30, 28 };
static int s_paused, s_pause_sel;

// ---- camera + projection (to draw the fishing line and markers over the 3D frame) --------------------------
static float s_cp[3], s_ct[3], s_fov = 62;
static float s_shake;                       // camera shake (world units), decays every frame
static void cam(float px, float py, float pz, float tx, float ty, float tz, float fov) {
    if (s_shake > 0.5f) { px += (rnd(200) - 100) * s_shake / 100; py += (rnd(200) - 100) * s_shake / 150; pz += (rnd(200) - 100) * s_shake / 100; }
    s_cp[0] = px; s_cp[1] = py; s_cp[2] = pz; s_ct[0] = tx; s_ct[1] = ty; s_ct[2] = tz; s_fov = fov;
    vx_lens(iroundf(fov), 16, 12000);
    vx_camera(iroundf(px), iroundf(py), iroundf(pz), 0, 0, 0);
    vx_look_at(iroundf(tx), iroundf(ty), iroundf(tz));
}
static int project(float x, float y, float z, int *sx, int *sy) {
    float fx = s_ct[0] - s_cp[0], fy = s_ct[1] - s_cp[1], fz = s_ct[2] - s_cp[2];
    float l = sqrtf_(fx * fx + fy * fy + fz * fz) + 1e-6f;
    fx /= l; fy /= l; fz /= l;
    float rx = fz, rz = -fx, rl = sqrtf_(rx * rx + rz * rz) + 1e-6f;
    rx /= rl; rz /= rl;
    const float ux = fy * rz, uy = fz * rx - fx * rz, uz = -fy * rx;   // f x r
    const float dx = x - s_cp[0], dy = y - s_cp[1], dz = z - s_cp[2];
    const float cz = dx * fx + dy * fy + dz * fz;
    if (cz < 10) return 0;
    const float t = sinf_(s_fov * PI_F / 360) / cosf_(s_fov * PI_F / 360);
    const float f = (W / 2) / t;
    *sx = W / 2 + iroundf((dx * rx + dz * rz) * f / cz);
    *sy = H / 2 - iroundf((dx * ux + dy * uy + dz * uz) * f / cz);
    return 1;
}

// ---- lures -----------------------------------------------------------------------------------------------------
static int s_lure_obj[NLURES];
#define WORM_SEGS 4
static int s_worm_seg[WORM_SEGS];
static float s_lure_wave = 0.3f;          // how hard the lure works (set by the retrieve: reel, twitch)
// A spindle body along Z (nose +Z): NR rings of 8 sides; `prof` = radius per ring, `tall` = height/width.
// Facets are coloured by where they face: back, flank, belly.
static void lure_body(const float *zs, const float *prof, int nr, float tall, int back, int flank, int belly) {
    enum { NS = 8 };
    int ring[8][NS];
    for (int r = 0; r < nr; r++)
        for (int k = 0; k < NS; k++) {
            const float a = k * 2 * PI_F / NS + PI_F / NS;
            ring[r][k] = mb_v(cosf_(a) * prof[r], sinf_(a) * prof[r] * tall, zs[r], 0, 0);
        }
    for (int r = 0; r < nr - 1; r++)
        for (int k = 0; k < NS; k++) {
            const float sy = sinf_((k + 0.5f) * 2 * PI_F / NS + PI_F / NS);
            const int m = sy > 0.5f ? back : sy < -0.5f ? belly : flank;
            mb_quad(ring[r][k], ring[r][(k + 1) % NS], ring[r + 1][(k + 1) % NS], ring[r + 1][k], m, 0, 0, (zs[r] + zs[r + 1]) / 2);
        }
    for (int k = 1; k < NS - 1; k++) {
        mb_tri(ring[0][0], ring[0][k], ring[0][k + 1], back, 0, 0, zs[0] + 5);
        mb_tri(ring[nr - 1][0], ring[nr - 1][k], ring[nr - 1][k + 1], back, 0, 0, zs[nr - 1] - 5);
    }
}
// A treble hook hanging below (x, y, z): a shank and three barbed points.
static void treble(float x, float y, float z, int steel) {
    const int s0 = mb_v(x - 0.6f, y, z, 0, 0), s1 = mb_v(x + 0.6f, y, z, 0, 0), s2 = mb_v(x, y - 9, z, 0, 0);
    mb_tri(s0, s1, s2, steel, x, y - 4, z + 3); mb_tri(s0, s1, s2, steel, x, y - 4, z - 3);
    for (int p = 0; p < 3; p++) {
        const float a = p * 2 * PI_F / 3;
        const float px = x + cosf_(a) * 5, pz = z + sinf_(a) * 5;
        const int b0 = mb_v(x, y - 9, z, 0, 0), b1 = mb_v(px, y - 12, pz, 0, 0), b2 = mb_v(px * 0.9f + x * 0.1f, y - 6, pz * 0.9f + z * 0.1f, 0, 0);
        mb_tri(b0, b1, b2, steel, x, y - 20, z); mb_tri(b0, b1, b2, steel, x, y + 20, z);
    }
}
static void eyes(float x, float y, float z, int iris, int pupil) {
    for (int s = -1; s <= 1; s += 2) {
        const float ex = s * x;
        const int e0 = mb_v(ex, y - 3, z - 3, 0, 0), e1 = mb_v(ex, y + 3, z - 3, 0, 0), e2 = mb_v(ex, y + 3, z + 3, 0, 0), e3 = mb_v(ex, y - 3, z + 3, 0, 0);
        mb_quad(e0, e1, e2, e3, iris, -s * 20.0f, y, z);
        const float px = ex + s * 0.3f;
        const int p0 = mb_v(px, y - 1.5f, z - 1, 0, 0), p1 = mb_v(px, y + 1.5f, z - 1, 0, 0), p2 = mb_v(px, y + 1.5f, z + 2, 0, 0), p3 = mb_v(px, y - 1.5f, z + 2, 0, 0);
        mb_quad(p0, p1, p2, p3, pupil, -s * 20.0f, y, z);
    }
}
// One piece of the worm (0 = head): 3 rings of 6, 13 units long from its pivot (z 0) back to z -13;
// ribbed (alternate rings a shade lighter), the last piece tapering to a curly tail.
#define WORM_LEN 13.0f
static void worm_piece(int piece, int m0, int m1, int m2) {
    enum { NR = 3, NS = 6 };
    int ring[NR][NS];
    for (int r = 0; r < NR; r++) {
        const int gi = piece * 2 + r;                  // ring index along the whole worm (0..10)
        const float z = -r * WORM_LEN / 2, rad = gi == 0 ? 2.4f : gi > 8 ? 3.3f - (gi - 8) * 1.0f : gi == 2 ? 3.9f : 3.4f;
        for (int k = 0; k < NS; k++) {
            const float an = k * 2 * PI_F / NS;
            ring[r][k] = mb_v(cosf_(an) * rad, sinf_(an) * rad, z, 0, 0);
        }
    }
    for (int r = 0; r < NR - 1; r++)
        for (int k = 0; k < NS; k++) {
            const int gi = piece * 2 + r, mat = gi == 1 ? m2 : (gi & 1) ? m1 : m0;       // a darker collar
            mb_quad(ring[r][k], ring[r][(k + 1) % NS], ring[r + 1][(k + 1) % NS], ring[r + 1][k], mat, 0, 0, -r * WORM_LEN / 2 - 3);
        }
    if (piece == 0)
        for (int k = 1; k < NS - 1; k++) mb_tri(ring[0][0], ring[0][k], ring[0][k + 1], m0, 0, 0, -5);
    if (piece == WORM_SEGS) {                          // the curly tail: a flat sickle
        const int t0 = mb_v(-1.5f, 0, -WORM_LEN, 0, 0), t1 = mb_v(1.5f, 0, -WORM_LEN, 0, 0);
        const int t2 = mb_v(7, 0, -WORM_LEN - 8, 0, 0), t3 = mb_v(2, 0, -WORM_LEN - 14, 0, 0);
        mb_quad(t0, t1, t2, t3, m1, 0, 5, -WORM_LEN - 6); mb_quad(t0, t1, t2, t3, m1, 0, -5, -WORM_LEN - 6);
    }
}
static void build_lures(void) {
    const int steel = vx_material(C565(200, 204, 214), VX_GOURAUD, 255, -1, 120);
    const int iris = vx_material(C565(255, 214, 40), VX_UNLIT, 255, -1, 0);
    const int pupil = vx_material(C565(10, 10, 12), VX_UNLIT, 255, -1, 0);
    for (int k = 0; k < NLURES; k++) {
        if (k == LURE_CRANK) {        // red-head crankbait: fat body, clear diving lip, two trebles
            const int back = vx_material(C565(170, 20, 24), VX_GOURAUD, 255, -1, 120);
            const int flank = vx_material(C565(236, 60, 44), VX_GOURAUD, 255, -1, 120);
            const int belly = vx_material(C565(250, 248, 240), VX_GOURAUD, 255, -1, 120);
            static const float zs[6] = { -18, -12, -4, 5, 12, 17 }, pr[6] = { 1.5f, 5.5f, 8, 8.5f, 7, 3.5f };
            lure_body(zs, pr, 6, 1.2f, back, flank, belly);
            eyes(7.2f, 3, 10, iris, pupil);
            const int lip = vx_material(C565(190, 210, 220), VX_GOURAUD, 200, -1, 160);
            const int l0 = mb_v(-5, -3, 16, 0, 0), l1 = mb_v(5, -3, 16, 0, 0), l2 = mb_v(6, -12, 27, 0, 0), l3 = mb_v(-6, -12, 27, 0, 0);
            mb_quad(l0, l1, l2, l3, lip, 0, 5, 18); mb_quad(l0, l1, l2, l3, lip, 0, -20, 30);
            treble(0, -8, 3, steel);
            treble(0, -2, -19, steel);
        } else if (k == LURE_POPPER) { // yellow popper: black back, cupped mouth, feathered tail
            const int back = vx_material(C565(24, 24, 28), VX_GOURAUD, 255, -1, 120);
            const int flank = vx_material(C565(252, 214, 30), VX_GOURAUD, 255, -1, 120);
            const int belly = vx_material(C565(255, 240, 150), VX_GOURAUD, 255, -1, 120);
            static const float zs[6] = { -17, -10, -2, 7, 14, 16 }, pr[6] = { 2.5f, 5.5f, 7, 7.5f, 8, 8 };
            lure_body(zs, pr, 6, 1.0f, back, flank, belly);
            const int mouth = vx_material(C565(70, 16, 20), VX_UNLIT, 255, -1, 0);
            for (int j = 1; j < 7; j++) {           // the cup: a dark disc set into the face
                const float a0 = j * 2 * PI_F / 8, a1 = (j + 1) * 2 * PI_F / 8;
                const int c = mb_v(0, 0, 15, 0, 0), p0 = mb_v(cosf_(0) * 6, sinf_(0) * 6, 16.5f, 0, 0);
                const int p1 = mb_v(cosf_(a0) * 6, sinf_(a0) * 6, 16.5f, 0, 0), p2 = mb_v(cosf_(a1) * 6, sinf_(a1) * 6, 16.5f, 0, 0);
                mb_tri(c, p1, p2, mouth, 0, 0, 0);
                (void)p0;
            }
            eyes(6.6f, 3, 8, iris, pupil);
            const int feather = vx_material(C565(240, 60, 60), VX_GOURAUD, 255, -1, 0);
            const int f0 = mb_v(0, 0, -17, 0, 0), f1 = mb_v(0, 6, -30, 0, 0), f2 = mb_v(0, -6, -30, 0, 0);
            mb_tri(f0, f1, f2, feather, 5, 0, -24); mb_tri(f0, f1, f2, feather, -5, 0, -24);
            treble(0, -6, 2, steel);
        } else if (k == LURE_JIG) {    // black-and-blue jig: lead head, eye, flared silicone skirt, hook up
            const int head = vx_material(C565(40, 44, 60), VX_GOURAUD, 255, -1, 160);
            const int head2 = vx_material(C565(60, 70, 110), VX_GOURAUD, 255, -1, 160);
            static const float zs[5] = { 4, 9, 14, 19, 22 }, pr[5] = { 3, 7, 8, 6.5f, 2.5f };
            lure_body(zs, pr, 5, 1.0f, head2, head, head);
            eyes(7.3f, 2, 15, iris, pupil);
            const int sk0 = vx_material(C565(30, 60, 190), VX_GOURAUD, 255, -1, 0);
            const int sk1 = vx_material(C565(20, 20, 30), VX_GOURAUD, 255, -1, 0);
            for (int j = 0; j < 12; j++) {                 // skirt strands flaring back
                const float a = j * 2 * PI_F / 12;
                const float x0 = cosf_(a) * 4, y0 = sinf_(a) * 4, x1 = cosf_(a) * 12, y1 = sinf_(a) * 11;
                const int s0 = mb_v(x0, y0, 4, 0, 0), s1 = mb_v(x1, y1, -26, 0, 0), s2 = mb_v(x1 * 0.8f + 1, y1 * 0.8f, -24, 0, 0);
                mb_tri(s0, s1, s2, (j & 1) ? sk0 : sk1, 0, 0, -10); mb_tri(s0, s1, s2, (j & 1) ? sk0 : sk1, x1 * 3, y1 * 3, -10);
            }
            const int h0 = mb_v(-0.7f, 4, 6, 0, 0), h1 = mb_v(0.7f, 4, 6, 0, 0), h2 = mb_v(0, 14, -18, 0, 0);
            mb_tri(h0, h1, h2, steel, 5, 8, -5); mb_tri(h0, h1, h2, steel, -5, 8, -5);
            const int g0 = mb_v(0, 14, -18, 0, 0), g1 = mb_v(0, 6, -12, 0, 0), g2 = mb_v(0.7f, 13, -15, 0, 0);
            mb_tri(g0, g1, g2, steel, 5, 10, -14); mb_tri(g0, g1, g2, steel, -5, 10, -14);
        } else {                       // purple soft worm: a head with the hook, the body in segments
            const int m0 = vx_material(C565(120, 50, 170), VX_GOURAUD, 255, -1, 160);
            const int m1 = vx_material(C565(160, 90, 210), VX_GOURAUD, 255, -1, 160);
            const int m2 = vx_material(C565(90, 30, 130), VX_GOURAUD, 255, -1, 160);
            worm_piece(0, m0, m1, m2);
            const int h0 = mb_v(-0.6f, 3, 0, 0, 0), h1 = mb_v(0.6f, 3, 0, 0, 0), h2 = mb_v(0, 10, -12, 0, 0);
            mb_tri(h0, h1, h2, steel, 0, 6, -4); mb_tri(h0, h1, h2, steel, 0, 6, 4);
            const int g0 = mb_v(0, 10, -12, 0, 0), g1 = mb_v(0, 4, -18, 0, 0), g2 = mb_v(0.6f, 10, -14, 0, 0);
            mb_tri(g0, g1, g2, steel, 5, 8, -15); mb_tri(g0, g1, g2, steel, -5, 8, -15);
            s_lure_obj[k] = mb_commit(steel, 0);
            vx_obj_scale(s_lure_obj[k], 100);
            vx_obj_show(s_lure_obj[k], 0);
            for (int j = 0; j < WORM_SEGS; j++) {
                worm_piece(j + 1, m0, m1, m2);
                s_worm_seg[j] = mb_commit(m0, 0);
                vx_obj_scale(s_worm_seg[j], 100);
                vx_obj_show(s_worm_seg[j], 0);
            }
            continue;
        }
        s_lure_obj[k] = mb_commit(steel, 0);
        vx_obj_scale(s_lure_obj[k], k == LURE_WORM ? 100 : 95);
        vx_obj_show(s_lure_obj[k], 0);
    }
}
static void lure_pose(float x, float y, float z, float yaw) {
    for (int k = 0; k < NLURES; k++) vx_obj_show(s_lure_obj[k], k == s_lure);
    for (int j = 0; j < WORM_SEGS; j++) vx_obj_show(s_worm_seg[j], s_lure == LURE_WORM);
    const float t = nv_millis() * 0.001f, w = s_lure_wave;
    float pitch = 0, roll = 0, sway = 0;
    if (s_lure == LURE_CRANK) { roll = sinf_(t * 26) * 28 * w; sway = sinf_(t * 13) * 0.12f * w; }   // the lip makes it wobble
    else if (s_lure == LURE_POPPER) { pitch = -8 + sinf_(t * 7) * 6 * w; roll = sinf_(t * 3) * 6; }
    else if (s_lure == LURE_JIG) { pitch = 25 - w * 20 + sinf_(t * 5) * 4; roll = sinf_(t * 4) * 5; }      // nose down, the skirt breathing
    vx_obj_pos(s_lure_obj[s_lure], iroundf(x), iroundf(y), iroundf(z));
    vx_obj_rot(s_lure_obj[s_lure], iroundf(pitch), iroundf(deg(yaw + sway)), iroundf(roll));
    if (s_lure == LURE_WORM) {                 // a wave running down the body, bigger toward the tail
        float px = x, py = y, pz = z, a = yaw + sinf_(t * 6) * 0.12f * w;
        vx_obj_rot(s_lure_obj[s_lure], 0, iroundf(deg(a)), 0);
        px -= sinf_(a) * WORM_LEN; pz -= cosf_(a) * WORM_LEN;
        for (int j = 0; j < WORM_SEGS; j++) {
            a = yaw + sinf_(t * 6 - (j + 1) * 1.1f) * (0.25f + 0.2f * j) * (0.4f + w);
            vx_obj_pos(s_worm_seg[j], iroundf(px), iroundf(py), iroundf(pz));
            vx_obj_rot(s_worm_seg[j], 0, iroundf(deg(a)), 0);
            px -= sinf_(a) * WORM_LEN; pz -= cosf_(a) * WORM_LEN;
            py += sinf_(t * 4 - j) * 0.6f * w;
        }
    }
}
static void lure_hide(void) {
    for (int k = 0; k < NLURES; k++) vx_obj_show(s_lure_obj[k], 0);
    for (int j = 0; j < WORM_SEGS; j++) vx_obj_show(s_worm_seg[j], 0);
}

// ---- records -----------------------------------------------------------------------------------------------------
static void records_load(void) {
    if (nv_load("records.bin", s_rec, sizeof s_rec) != (int)sizeof s_rec)
        for (int i = 0; i < NRECORDS; i++) { s_rec[i].kg100 = 0; s_rec[i].species = 0; s_rec[i].stage = 0; }
    if (nv_load("bestrun.bin", &s_best_run100, 4) != 4) s_best_run100 = 0;
}
// Insert a catch; returns its rank (0-based) on the wall, or -1.
static int records_add(int sp, float kg, int stage) {
    const int k = iroundf(kg * 100);
    int pos = NRECORDS;
    while (pos > 0 && s_rec[pos - 1].kg100 < k) pos--;
    if (pos >= NRECORDS) return -1;
    for (int i = NRECORDS - 1; i > pos; i--) s_rec[i] = s_rec[i - 1];
    s_rec[pos].kg100 = (uint16_t)k; s_rec[pos].species = (uint8_t)sp; s_rec[pos].stage = (uint8_t)stage;
    nv_save("records.bin", s_rec, sizeof s_rec);
    return pos;
}

// ---- game state ----------------------------------------------------------------------------------------------------
static float s_aim, s_power, s_power_t;
static int s_charging;
static int s_boil = -1, s_boil_at;          // a fish breaking the surface at a spot (where to cast)
static float s_boil_x, s_boil_z;
static float s_lx, s_ly, s_lz, s_cast_t, s_cast_len, s_tx, s_tz, s_twitch_t, s_orbit;
static int s_strike_fish, s_strike_until, s_twitches, s_nibble = -1;
static float s_fyaw, s_fpx, s_fpz, s_ccp[3], s_cct[3];     // fight: fish heading, last position, camera
static Fight s_fight;
static int s_catch_sp;
static float s_catch_kg;
static uint8_t s_list_sp[16];
static int s_bonus;
static float s_list_kg[16];
// The boat moves: up/down on the d-pad start the outboard and drive, left/right steer.
static float s_bx, s_bz, s_bspeed;
static int s_engine, s_engine_at, s_motor_at;              // engine 0 off, 1 cranking, 2 running
// Arcade juice: the big banner ("FISH ON!"), a white flash, a hit-stop freeze on the hook-set.
static const char *s_ban;
static int s_ban_at, s_ban_col, s_flash_until, s_hitstop_until;
static void banner(const char *t, int col, int now) { s_ban = t; s_ban_at = now; s_ban_col = col; }
static int s_combo, s_qualified, s_qual_now, s_perfect, s_junk = -1, s_tb;
static int s_run_catches, s_run_best_sp, s_stages_cleared;   // the whole tournament, for the final page
static float s_run_best_kg;
static int s_unlocked, s_sel;                              // lakes open on the select screen
// The livewell (Fisherman's Bait rules): a stage counts its three heaviest fish.
static float well_total(void) {
    float a = 0, b = 0, c = 0;
    const int n = s_catches < 16 ? s_catches : 16;
    for (int i = 0; i < n; i++) {
        const float k = s_list_kg[i];
        if (k > a) { c = b; b = a; a = k; } else if (k > b) { c = b; b = k; } else if (k > c) c = k;
    }
    return a + b + c;
}
static int well_keeps(float kg) {                          // is this catch among the three heaviest?
    int heavier = 0;
    const int n = s_catches < 16 ? s_catches : 16;
    for (int i = 0; i < n; i++) if (s_list_kg[i] > kg) heavier++;
    return heavier < 3;
}
static void well_add(int sp, float kg) {
    int slot = s_catches < 16 ? s_catches : -1;
    if (slot < 0) {                                        // full: replace the lightest
        slot = 0;
        for (int i = 1; i < 16; i++) if (s_list_kg[i] < s_list_kg[slot]) slot = i;
        if (s_list_kg[slot] >= kg) slot = -1;
    }
    if (slot >= 0) { s_list_sp[slot] = (uint8_t)sp; s_list_kg[slot] = kg; }
    s_catches++;
}

static float s_line;                      // line out, rod tip to lure (units)
#define ROPE_N 14
static float s_rope[ROPE_N + 1][3], s_rope_old[ROPE_N + 1][3];
static int s_rope_ok;
static void rope_anchor(float *a) {                     // the rod tip, in the underwater frame
    a[0] = s_bx + sinf_(s_aim) * 40 + cosf_(s_aim) * 30; a[1] = SURF + 150; a[2] = s_bz + cosf_(s_aim) * 40 - sinf_(s_aim) * 30;
}
static void rope_reset(void) {
    float a[3];
    rope_anchor(a);
    const float lure[3] = { s_lx, s_ly, s_lz };
    for (int i = 0; i <= ROPE_N; i++)
        for (int j = 0; j < 3; j++) s_rope[i][j] = s_rope_old[i][j] = a[j] + (lure[j] - a[j]) * i / ROPE_N;
    s_rope_ok = 1;
}
// Verlet: ends pinned (rod tip, lure), gravity heavy in air and light in water (nylon is nearly
// neutral), water drag, the segment length set by the line out, the lake bed as a floor.
static void rope_step(float dt) {
    if (!s_rope_ok) rope_reset();
    float a[3];
    rope_anchor(a);
    for (int j = 0; j < 3; j++) { s_rope[0][j] = a[j]; }
    s_rope[ROPE_N][0] = s_lx; s_rope[ROPE_N][1] = s_ly; s_rope[ROPE_N][2] = s_lz;
    const float h = dt > 0.033f ? 0.033f : dt;
    for (int i = 1; i < ROPE_N; i++) {
        float *p = s_rope[i], *o = s_rope_old[i];
        const int wet = p[1] < SURF;
        const float damp = wet ? 0.86f : 0.97f, g = wet ? 60.0f : 900.0f;
        for (int j = 0; j < 3; j++) { const float v = (p[j] - o[j]) * damp; o[j] = p[j]; p[j] += v; }
        p[1] -= g * h * h;
    }
    const float seg = s_line / ROPE_N;
    for (int it = 0; it < 12; it++) {
        for (int i = 0; i < ROPE_N; i++) {
            float *p = s_rope[i], *q = s_rope[i + 1];
            const float dx = q[0] - p[0], dy = q[1] - p[1], dz = q[2] - p[2];
            const float l = sqrtf_(dx * dx + dy * dy + dz * dz) + 1e-4f;
            const float k = (l - seg) / l * 0.5f;
            if (i > 0) { p[0] += dx * k; p[1] += dy * k; p[2] += dz * k; }
            if (i + 1 < ROPE_N) { q[0] -= dx * k; q[1] -= dy * k; q[2] -= dz * k; }
        }
        for (int i = 1; i < ROPE_N; i++) if (s_rope[i][1] < 3) s_rope[i][1] = 3;
    }
}
// Only the part under water shows (the surface hides the rest): a fine line, a faint pale glint
// near the lure fading into the water.
static void draw_rope(void) {
    int px = 0, py = 0, have = 0;
    for (int i = ROPE_N; i >= 0; i--) {
        const float *p = s_rope[i];
        int sx, sy;
        if (p[1] > SURF + 2 || !project(p[0], p[1], p[2], &sx, &sy)) { have = 0; continue; }
        if (have) nv_gfx_line(px, py, sx, sy, i > ROPE_N - 3 ? C565(158, 198, 206) : C565(104, 154, 166));
        px = sx; py = sy; have = 1;
    }
}

static void go(int st, int now) {
    static const char *const names[] = { "title", "records", "stage", "lure", "aim", "cast", "retrieve",
                                         "strike", "fight", "catch", "lost", "weigh", "over", "intro", "select" };
    char b[40] = "bass: ";
    cat(b, names[st]);
    nv_log(NV_LOG_INFO, b);
    s_state = st; s_state_ms = now;
}

static int s_go_at;                          // when this stage's READY/GO started (0 = not yet)
static int s_tb_at;                          // when the last time bonus was added (clock popup)
static int s_last_sec = -1;                  // last whole second beeped in the final countdown
static void start_stage(int now) {
    lake_build(s_stage, s_loop);
    fish_build();
    build_lures();
    lake_view(0);
    {   // each lake has its own theme on the intermission card (ACE-Step)
        char m[8] = "lake0";
        m[4] = (char)('0' + s_stage);
        sfx(m);
    }
    s_quota = g_stage[s_stage].quota_kg * (1.0f + 0.35f * s_loop);
    s_time_ms = g_stage[s_stage].time_s * 1000;
    s_total = 0; s_catches = 0; s_stage_best = 0;
    s_aim = 0; s_bx = s_bz = s_bspeed = 0; s_engine = 0;
    s_combo = 0; s_qualified = 0; s_qual_now = 0; s_go_at = 0; s_last_sec = -1;
    go(ST_STAGE, now);
}
static void to_aim(int now) {
    if (!s_go_at) s_go_at = now;             // first cast of the stage: READY? GO!
    fish_hide();
    lake_view(0);
    lure_hide();
    s_power = 0; s_power_t = 0; s_charging = 0;
    go(ST_AIM, now);
}

// How far along the aim the water goes: the cast distance clipped to 60 units short of the bank.
static float cast_reach(float d, int *clipped) {
    const float fx = sinf_(s_aim), fz = cosf_(s_aim);
    float lo = 0, hi = d;
    *clipped = 0;
    const float x = s_bx + fx * d, z = s_bz + fz * d;
    if (sqrtf_(x * x + z * z) < lake_shore(atan2f_(x, z)) - 60) return d;
    *clipped = 1;
    for (int i = 0; i < 12; i++) {                      // bisect to the waterline
        const float m = (lo + hi) / 2, mx = s_bx + fx * m, mz = s_bz + fz * m;
        if (sqrtf_(mx * mx + mz * mz) < lake_shore(atan2f_(mx, mz)) - 60) lo = m; else hi = m;
    }
    return lo;
}
static int s_bank;                          // the last cast was clipped at the bank

static void aim_camera(void) {
    const float fx = sinf_(s_aim), fz = cosf_(s_aim), sp = fabsf_(s_bspeed);
    const float back = 330 + sp * 0.3f, up = 160 + sp * 0.08f;          // pulls back at speed
    cam(s_bx - fx * back, up, s_bz - fz * back, s_bx + fx * 900, 0, s_bz + fz * 900, 62 + sp * 0.012f);
    const int t = nv_millis();
    const float bob = sinf_(t * 0.0022f) * 2.5f + (s_engine == 2 ? sinf_(t * 0.05f) * 0.8f : 0);
    // nose up under power, a little roll while turning; the trim mesh rides with the hull
    const int pitch = iroundf(-s_bspeed * 0.011f + sinf_(t * 0.0017f) * 1.2f), roll = iroundf((s_in.right - s_in.left) * sp * 0.012f);
    vx_obj_pos(g_boat, iroundf(s_bx), iroundf(bob), iroundf(s_bz));
    vx_obj_rot(g_boat, pitch, iroundf(deg(s_aim)), roll);
    vx_obj_pos(g_boat_trim, iroundf(s_bx), iroundf(bob), iroundf(s_bz));
    vx_obj_rot(g_boat_trim, pitch, iroundf(deg(s_aim)), roll);
}
static void rod_tip(float *x, float *y, float *z) { *x = s_bx + sinf_(s_aim) * 40 + cosf_(s_aim) * 30; *y = 150; *z = s_bz + cosf_(s_aim) * 40 - sinf_(s_aim) * 30; }

// ---- HUD pieces -------------------------------------------------------------------------------------------------------
static void hud_top(void) {
    char b[48], t[24];
    panel(4, 4, 172, 36);
    nv_gfx_image("i_scale", 8, 10, 22, 22);
    text_sh(34, 10, lake_name(s_stage), C_CYAN, 1);
    fmt_kg(t, s_total); b[0] = 0; cat(b, t); cat(b, " / "); fmt_kg(t, s_quota); cat(b, t);
    text_sh(34, 23, b, s_total >= s_quota ? C_GREEN : C_WHITE, 1);
    // quota bar under the panel
    nv_gfx_rect(8, 36, 164, 2, C565(30, 40, 60));
    nv_gfx_rect(8, 36, iroundf(clampf(s_total / (s_quota > 0 ? s_quota : 1), 0, 1) * 164), 2, s_total >= s_quota ? C_GREEN : C_YELLOW);
    {   // the livewell: the three heaviest fish so far
        int top[3] = { -1, -1, -1 };
        const int n = s_catches < 16 ? s_catches : 16;
        for (int i = 0; i < n; i++)
            for (int k = 0; k < 3; k++)
                if (top[k] < 0 || s_list_kg[i] > s_list_kg[top[k]]) {
                    for (int j = 2; j > k; j--) top[j] = top[j - 1];
                    top[k] = i;
                    break;
                }
        for (int k = 0; k < 3; k++) {
            const int x = 4 + k * 58;
            nv_gfx_rect(x, 42, 55, 20, C565(10, 22, 40));
            if (top[k] < 0) continue;
            fish_art_kg(s_list_sp[top[k]], s_list_kg[top[k]], x + 1, 43, 27, 18);
            char t2[16];
            fmt_kg(t2, s_list_kg[top[k]]);
            t2[4] = 0;                                     // "1.23"
            text_sh(x + 28, 48, t2, C_WHITE, 1);
        }
    }
    fmt_clock(t, s_time_ms);
    panel(W / 2 - 58, 4, 116, 32);
    nv_gfx_image("i_clock", W / 2 - 52, 9, 22, 22);
    text_sh(W / 2 - 22, 9, t, s_time_ms < 20000 && ((s_time_ms / 250) & 1) ? C_RED : C_YELLOW, 3);
    {
        const int now = nv_millis();
        if (s_tb_at && now - s_tb_at < 1500 && s_tb > 0) {             // "+12" floats up off the clock
            char b2[12], n2[8];
            b2[0] = '+'; b2[1] = 0; fmt_int(n2, s_tb); cat(b2, n2);
            text_sh(W / 2 + 64, 30 - (now - s_tb_at) / 60, b2, ((now / 100) & 1) ? C_GREEN : C_WHITE, 2);
        }
        if (s_combo >= 2) {
            char b2[16], n2[8];
            b2[0] = 0; cat(b2, "COMBO x"); fmt_int(n2, s_combo); cat(b2, n2);
            text_sh(W / 2 - nv_gfx_text_width(b2, 1) / 2, 38, b2, C565(255, 150, 40), 1);
        }
    }
    panel(W - 132, 4, 128, 36);
    nv_gfx_image("i_hook", W - 126, 10, 22, 22);
    text_sh(W - 100, 23, T(g_lure_it[s_lure], g_lure_en[s_lure]), C_WHITE, 1);
    nv_gfx_image("i_fishes", W - 100, 5, 18, 18);
    fmt_int(t, s_catches); b[0] = 0; cat(b, "x"); cat(b, t);
    text_sh(W - 78, 10, b, C_YELLOW, 1);
}
static void hud_msg(int now) {
    if (now < s_msg_until && s_msg[0]) {
        const int w = nv_gfx_text_width(s_msg, 3);
        panel(W / 2 - w / 2 - 12, 96, w + 24, 34);
        text_c(102, s_msg, C_YELLOW, 3);
    }
}
static void bar(int x, int y, int w, int h, float v, int col, int redline) {
    nv_gfx_rect(x - 2, y - 2, w + 4, h + 4, C_SHADOW);
    nv_gfx_rect(x, y, w, h, C565(30, 36, 50));
    if (redline) nv_gfx_rect(x + w * 85 / 100, y, w - w * 85 / 100, h, C565(110, 20, 20));
    const int f = iroundf(clampf(v, 0, 1) * w);
    nv_gfx_rect(x, y, f, h, col);
}
// ---- the rod and the line ------------------------------------------------------------------------------
// The rod is drawn over the frame, butt at the bottom right, as a bent curve: its tip is animated
// (wind-up over the shoulder while charging, a whip forward on release, pulled toward the fish and
// bent by the line tension in the fight). The line is a real 3D curve from the rod tip to the lure
// or the fish's mouth: sampled, sagging with its slack (never below the water while it lies on it),
// projected point by point.
static float s_rtx = W - 170, s_rty = 96, s_rbend;          // rod tip on screen, bend (px)
static float s_release_t = -1;                              // seconds since the cast release
static void rod_seek(float tx, float ty, float bend, float rate, float dt) {
    const float k = clampf(dt * rate, 0, 1);
    s_rtx += (tx - s_rtx) * k; s_rty += (ty - s_rty) * k; s_rbend += (bend - s_rbend) * k;
}
static void draw_rod(int reeling, int now) {
    const float bx = W - 36, by = H + 12;                   // butt
    const float mx = (bx + s_rtx) / 2, my = (by + s_rty) / 2;
    float nx = -(s_rty - by), ny = s_rtx - bx;              // perpendicular, toward the bend side
    const float nl = sqrtf_(nx * nx + ny * ny) + 1e-3f;
    nx /= nl; ny /= nl;
    const float cx = mx + nx * s_rbend, cy = my + ny * s_rbend;
    int px = (int)bx, py = (int)by;
    for (int i = 1; i <= 14; i++) {                          // quadratic Bezier, thick at the butt
        const float t = i / 14.0f, u = 1 - t;
        const int x = iroundf(u * u * bx + 2 * u * t * cx + t * t * s_rtx);
        const int y = iroundf(u * u * by + 2 * u * t * cy + t * t * s_rty);
        const int th = t < 0.25f ? 3 : t < 0.6f ? 2 : 1;
        for (int k = -th / 2; k <= th / 2 + (th > 1 ? 0 : 0); k++) nv_gfx_line(px + k, py, x + k, y, C565(34, 34, 40));
        if (i > 3) nv_gfx_line(px - 1, py, x - 1, y, C565(110, 110, 124));      // highlight
        if (i == 4 || i == 8 || i == 11 || i == 13) nv_gfx_circle(x, y, 2, C565(200, 200, 210));   // guides
        if (i <= 3) nv_gfx_line(px + 2, py, x + 2, y, C565(170, 120, 70));      // cork grip
        px = x; py = y;
    }
    // Reel on the butt, handle turning while you wind.
    const int rx = iroundf(bx - (bx - cx) * 0.18f) - 10, ry = iroundf(by - (by - cy) * 0.18f);
    nv_gfx_circle(rx, ry, 11, C565(40, 44, 54));
    nv_gfx_circle(rx, ry, 8, C565(190, 194, 206));
    const float a = reeling ? now * 0.02f : 0.8f;
    nv_gfx_line(rx, ry, rx + iroundf(cosf_(a) * 12), ry + iroundf(sinf_(a) * 12), C565(30, 30, 30));
    nv_gfx_circle(rx + iroundf(cosf_(a) * 12), ry + iroundf(sinf_(a) * 12), 3, C565(230, 230, 230));
}
// The line from the rod tip (screen) to a 3D point, through `from3` (the rod tip in the world),
// sagging by `sag` world units at its middle; `water` keeps it on or above the surface (y >= 0).
static void draw_line3d(const float from3[3], float x, float y, float z, float sag, int water) {
    int lx = iroundf(s_rtx), ly = iroundf(s_rty);
    for (int i = 1; i <= 12; i++) {
        const float t = i / 12.0f;
        float px = from3[0] + (x - from3[0]) * t, py = from3[1] + (y - from3[1]) * t, pz = from3[2] + (z - from3[2]) * t;
        py -= sag * 4 * t * (1 - t);
        if (water && py < 1) py = 1;
        int sx, sy;
        if (!project(px, py, pz, &sx, &sy)) continue;
        nv_gfx_line(lx, ly, sx, sy, C565(236, 236, 244));
        lx = sx; ly = sy;
    }
}


// Rings on the water: a circle of radius r (world units) on the surface at (x, z), projected point by
// point (true perspective ellipses), its colour fading from bright foam into the stage's water.
static uint16_t blend565(uint16_t a, uint16_t b, int t) {
    const int r = (a >> 11) + ((((b >> 11) - (a >> 11)) * t) >> 8);
    const int g = ((a >> 5) & 63) + (((((b >> 5) & 63) - ((a >> 5) & 63)) * t) >> 8);
    const int bl = (a & 31) + ((((b & 31) - (a & 31)) * t) >> 8);
    return (uint16_t)((r << 11) | (g << 5) | bl);
}
static void water_ring(float x, float z, float r, int fade) {
    if (fade >= 250 || r < 2) return;
    const uint16_t col = blend565(C565(236, 246, 255), g_stage[s_stage].water, fade);
    int px = 0, py = 0, have = 0;
    for (int i = 0; i <= 24; i++) {
        const float a = i * (2 * PI_F / 24);
        int sx, sy;
        if (!project(x + sinf_(a) * r, 1, z + cosf_(a) * r, &sx, &sy)) { have = 0; continue; }
        if (have) nv_gfx_line(px, py, sx, sy, col);
        px = sx; py = sy; have = 1;
    }
}
static void water_rings(float x, float z, float age, float speed, int n) {
    for (int k = 0; k < n; k++) {
        const float a = age - k * 0.16f;                  // the rings leave one after another
        if (a <= 0) continue;
        water_ring(x, z, 6 + a * speed, iroundf(clampf(a * 190, 0, 256)));
    }
}


// ---- screens ------------------------------------------------------------------------------------------------------------
static int s_title_btn = -1, s_screen_btn = -1, s_logo_at;
static void draw_title(int now) {
    art("title");
    const int lt = now - s_logo_at;                        // the logo slams in, big to small
    const int sb = lt < 360 ? 16 - lt * 9 / 360 : 7, sv = lt < 360 ? 11 - lt * 6 / 360 : 5;
    text_c(34, "VERTICE", C_SHADOW, sv);
    text_sh((W - nv_gfx_text_width("VERTICE", sv)) / 2 - 2, 32, "VERTICE", C_WHITE, sv);
    text_sh((W - nv_gfx_text_width("BASS", sb)) / 2, 70, "BASS", ((now / 700) % 4 == 0) ? C_WHITE : C_YELLOW, sb);
    if (lt >= 360 && lt < 440) nv_gfx_rect(0, 0, W, H, C_WHITE);
    text_c(124, T("TORNEO DI PESCA ARCADE", "ARCADE FISHING TOURNAMENT"), C_CYAN, 2);
    static const char *const it[2] = { "GIOCA", "RECORD" }, *const en[2] = { "PLAY", "RECORDS" };
    for (int i = 0; i < 2; i++) {
        const int y = 160 + i * 34, sel = s_menu == i;
        panel(W / 2 + 60, y, 180, 28);
        if (sel) nv_gfx_rect(W / 2 + 64, y + 4, 172, 20, C565(40, 90, 150));
        text_sh(W / 2 + 150 - nv_gfx_text_width(T(it[i], en[i]), 2) / 2, y + 7, T(it[i], en[i]), sel ? C_YELLOW : C_WHITE, 2);
    }
    if (s_best_run100 > 0) {
        char b[40], t[20];
        fmt_kg(t, s_best_run100 / 100.0f); b[0] = 0; cat(b, T("MIGLIOR TORNEO ", "BEST RUN ")); cat(b, t);
        text_sh(W / 2 + 150 - nv_gfx_text_width(b, 1) / 2, 234, b, C_WHITE, 1);
    }
    static const char *const lab_it[4] = { "SU", "GIU'", "OK", "ESCI" }, *const lab_en[4] = { "UP", "DOWN", "OK", "EXIT" };
    const char *labs[4];
    for (int i = 0; i < 4; i++) labs[i] = T(lab_it[i], lab_en[i]);
    s_title_btn = ui_row(labs, 4);
    if (pad_connected()) {
        static const int k[3] = { K_UD, K_A, K_SELECT };
        const char *l[3] = { T("SCEGLI", "CHOOSE"), "OK", T("ESCI", "EXIT") };
        pad_hints(k, l, 3);
    }
    (void)now;
}

static void draw_records(void) {
    art("dock");
    panel(40, 20, W - 80, H - 40);
    nv_gfx_image("a_trophy", 44, 16, 40, 40);
    nv_gfx_image("a_trophy", W - 84, 16, 40, 40);
    text_c(30, T("I 10 PESCI PIU' GROSSI", "TOP 10 BIGGEST FISH"), C_YELLOW, 2);
    char b[64], t[24];
    for (int i = 0; i < NRECORDS; i++) {
        const int y = 58 + i * 20;
        const Record *r = &s_rec[i];
        fmt_int(t, i + 1); b[0] = 0; cat(b, t); cat(b, ".");
        static const char *const medal[3] = { "a_gold", "a_silver", "a_bronze" };
        if (i < 3 && r->kg100) nv_gfx_image(medal[i], 50, y - 6, 20, 20);
        else text_sh(56, y, b, i == s_new_rank ? C_YELLOW : C_GREY, 1);
        if (!r->kg100) { text_sh(84, y, "-", C_GREY, 1); continue; }
        fish_art_kg(r->species, r->kg100 / 100.0f, 78, y - 5, 27, 18);
        text_sh(110, y, sp_name(r->species), i == s_new_rank ? C_YELLOW : C_WHITE, 1);
        text_sh(250, y, lake_name(r->stage), C_CYAN, 1);
        fmt_kg(t, r->kg100 / 100.0f);
        text_sh(W - 60 - nv_gfx_text_width(t, 1), y, t, i == 0 ? C_YELLOW : C_WHITE, 1);
    }
    const char *labs[1] = { T("INDIETRO", "BACK") };
    s_screen_btn = ui_row(labs, 1);
    if (pad_connected()) { static const int k[1] = { K_B }; const char *l[1] = { T("INDIETRO", "BACK") }; pad_hints(k, l, 1); }
}

static void draw_stage_card(int now) {
    char b[48], t[24];
    lake_art(s_stage);
    panel(40, 8, W - 80, 58);
    b[0] = 0; cat(b, T("TAPPA ", "STAGE ")); fmt_int(t, s_stage + 1 + s_loop * NSTAGES); cat(b, t);
    text_c(14, b, C_CYAN, 1);
    text_c(28, lake_name(s_stage), C_WHITE, 3);
    panel(40, 190, W - 80, 64);
    b[0] = 0; cat(b, T("QUOTA ", "QUOTA ")); fmt_kg(t, s_quota); cat(b, t);
    cat(b, "   "); cat(b, T("TEMPO ", "TIME ")); fmt_clock(t, s_time_ms); cat(b, t);
    text_c(200, b, C_YELLOW, 2);
    text_c(228, T("PESCA ABBASTANZA PESO PRIMA DEL GONG", "LAND ENOUGH WEIGHT BEFORE THE BELL"), C_WHITE, 1);
    const char *labs[2] = { T("VIA!", "GO!"), "MENU" };
    s_screen_btn = ui_row(labs, 2);
    if (pad_connected()) { static const int k[2] = { K_A, K_B }; const char *l[2] = { T("VIA!", "GO!"), "MENU" }; pad_hints(k, l, 2); }
    (void)now;
}

static void draw_lure_icon(int k, int cx, int cy, int sel) {
    char n[8] = "lure0";
    n[4] = (char)('0' + k);
    nv_gfx_image(n, cx - 40, cy - 40, 80, 80);
    if (sel) nv_gfx_rect(cx - 50, cy + 48, 100, 3, C_YELLOW);
}
static void draw_lure_select(void) {
    static const char *const d_it[NLURES] = { "MEZZ'ACQUA", "GALLA", "FONDO LENTO", "FONDO VELOCE" };
    static const char *const d_en[NLURES] = { "MID WATER", "SURFACE", "SLOW BOTTOM", "FAST BOTTOM" };
    static const char *const h_it[NLURES] = { "RECUPERA", "STRAPPI", "PAUSE", "SALTELLI" };
    static const char *const h_en[NLURES] = { "REEL", "TWITCH", "PAUSE", "HOP" };
    art("school");
    text_c(26, T("SCEGLI L'ESCA", "CHOOSE YOUR LURE"), C_YELLOW, 3);
    for (int k = 0; k < NLURES; k++) {
        const int x = 8 + k * 125, sel = s_lure == k;
        panel(x, 66, 118, 164);
        if (sel) nv_gfx_rect(x + 4, 70, 110, 156, C565(30, 64, 110));
        draw_lure_icon(k, x + 59, 106, sel);
        text_sh(x + 59 - nv_gfx_text_width(T(g_lure_it[k], g_lure_en[k]), 2) / 2, 164, T(g_lure_it[k], g_lure_en[k]), sel ? C_YELLOW : C_WHITE, 2);
        text_sh(x + 59 - nv_gfx_text_width(T(d_it[k], d_en[k]), 1) / 2, 190, T(d_it[k], d_en[k]), C_CYAN, 1);
        text_sh(x + 59 - nv_gfx_text_width(T(h_it[k], h_en[k]), 1) / 2, 204, T(h_it[k], h_en[k]), C_GREY, 1);
    }
    const char *labs[4] = { "<", ">", "OK", "MENU" };
    s_screen_btn = ui_row(labs, 4);
    if (pad_connected()) { static const int k[3] = { K_LR, K_A, K_B }; const char *l[3] = { T("ESCA", "LURE"), "OK", "MENU" }; pad_hints(k, l, 3); }
}

static void draw_weigh(int now) {
    char b[48], t[24];
    const float shown = s_total * clampf((now - s_state_ms) / 1600.0f, 0, 1);
    art(now - s_state_ms > 1700 ? (s_total >= s_quota ? "win" : "lose") : "weigh");
    panel(66, 30, W - 132, 240);
    text_c(42, T("PESATURA", "WEIGH-IN"), C_YELLOW, 3);
    text_c(74, lake_name(s_stage), C_CYAN, 2);
    b[0] = 0; cat(b, T("I 3 PIU' PESANTI  (PESCI ", "3 HEAVIEST  (FISH ")); fmt_int(t, s_catches); cat(b, t); cat(b, ")");
    text_c(100, b, C_WHITE, 2);
    fmt_kg(t, shown);
    text_c(122, t, C_WHITE, 4);
    b[0] = 0; cat(b, T("QUOTA ", "QUOTA ")); fmt_kg(t, s_quota); cat(b, t);
    text_c(156, b, C_GREY, 1);
    // The catches, biggest first (up to six, two columns).
    int order[16], n = s_catches < 16 ? s_catches : 16;
    for (int i = 0; i < n; i++) order[i] = i;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (s_list_kg[order[j]] > s_list_kg[order[i]]) { const int x = order[i]; order[i] = order[j]; order[j] = x; }
    for (int i = 0; i < n && i < 6; i++) {
        const int x = i < 3 ? 84 : W / 2 + 6, y = 170 + (i % 3) * 11;
        fmt_kg(t, s_list_kg[order[i]]);
        b[0] = 0; cat(b, sp_name(s_list_sp[order[i]])); cat(b, " "); cat(b, t);
        text_sh(x, y, b, i < 3 ? C_YELLOW : C_GREY, 1);
    }
    if (now - s_state_ms > 1700) {
        const int ok = s_total >= s_quota;
        text_c(206, ok ? T("QUALIFICATO!", "QUALIFIED!") : T("NON QUALIFICATO", "NOT QUALIFIED"), ok ? C_GREEN : C_RED, 3);
        {   // the rank: S at double the quota, A at 1.5x, B qualified, C short
            const float q = s_total / (s_quota > 0 ? s_quota : 1);
            const char *rank = q >= 2.0f ? "S" : q >= 1.5f ? "A" : q >= 1.0f ? "B" : "C";
            const uint16_t rc = q >= 2.0f ? C565(255, 214, 40) : q >= 1.5f ? C_GREEN : q >= 1.0f ? C_CYAN : C_GREY;
            const int e = now - s_state_ms - 1700, sc = e < 200 ? 14 - e / 40 : 9;
            nv_gfx_circle(W - 104, 100, 40, C_SHADOW);
            nv_gfx_circle(W - 106, 98, 38, C565(20, 30, 50));
            text_sh(W - 106 - nv_gfx_text_width(rank, sc) / 2, 98 - sc * 7 / 2, rank, rc, sc);
            text_sh(W - 126, 142, T("RANGO", "RANK"), C_GREY, 1);
        }
        const char *labs[1] = { T("AVANTI", "NEXT") };
        s_screen_btn = ui_row(labs, 1);
        if (pad_connected()) { static const int k[1] = { K_A }; const char *l[1] = { T("AVANTI", "NEXT") }; pad_hints(k, l, 1); }
    }
}

// The end of a tournament: the lake painting behind a plate with the run's story counting up —
// stages cleared, fish landed, total weight, the biggest fish in its portrait — and the verdict.
static void draw_over(int now) {
    char b[48], t[24];
    const int e = now - s_state_ms;
    art("lose");
    nv_gfx_rect(0, 0, W, 34, C565(8, 12, 22));
    text_c(7, T("FINE TORNEO", "TOURNAMENT OVER"), C_YELLOW, e < 160 ? 5 : 3);
    panel(40, 44, W - 80, 196);
    // left column: the numbers, one after the other
    const float k1 = clampf((e - 300) / 500.0f, 0, 1), k2 = clampf((e - 800) / 500.0f, 0, 1), k3 = clampf((e - 1300) / 900.0f, 0, 1);
    text_sh(56, 58, T("TAPPE SUPERATE", "STAGES CLEARED"), C_GREY, 1);
    fmt_int(t, iroundf(s_stages_cleared * k1)); text_sh(56, 70, t, C_WHITE, 3);
    text_sh(56, 102, T("PESCI PRESI", "FISH LANDED"), C_GREY, 1);
    fmt_int(t, iroundf(s_run_catches * k2)); text_sh(56, 114, t, C_WHITE, 3);
    text_sh(56, 146, T("PESO TOTALE", "TOTAL WEIGHT"), C_GREY, 1);
    fmt_kg(t, s_run_total * k3); text_sh(56, 158, t, C_YELLOW, 3);
    b[0] = 0; cat(b, T("RECORD TORNEO ", "BEST RUN ")); fmt_kg(t, s_best_run100 / 100.0f); cat(b, t);
    text_sh(56, 190, b, C_CYAN, 1);
    // right column: the biggest fish of the run
    nv_gfx_rect(W / 2 + 10, 56, 1, 172, C565(40, 70, 110));
    text_sh(W / 2 + 26, 58, T("IL PIU' GROSSO", "BIGGEST CATCH"), C_GREY, 1);
    if (s_run_best_kg > 0 && e > 1800) {
        const int z = e < 2100 ? (e - 1800) * 100 / 300 : 100;
        const int fw = 170 * z / 100, fh = 106 * z / 100;
        fish_art_kg(s_run_best_sp, s_run_best_kg, W / 2 + 104 - fw / 2, 118 - fh / 2, fw, fh);
        text_sh(W / 2 + 26, 172, sp_name(s_run_best_sp), C_WHITE, 1);
        fmt_kg(t, s_run_best_kg); text_sh(W / 2 + 26, 184, t, C_YELLOW, 2);
    } else if (s_run_best_kg <= 0) text_sh(W / 2 + 26, 120, T("NESSUN PESCE", "NO FISH"), C_GREY, 2);
    if (e > 2300 && iroundf(s_run_total * 100) >= s_best_run100 && s_run_total > 0) {
        text_c(216, T("NUOVO RECORD DI TORNEO!", "NEW TOURNAMENT RECORD!"), ((e / 150) & 1) ? C_GREEN : C_WHITE, 2);
        nv_gfx_image("a_trophy", 46, 204, 32, 32);
        nv_gfx_image("a_trophy", W - 78, 204, 32, 32);
    }
    const char *labs[2] = { T("RIPROVA", "RETRY"), "MENU" };
    s_screen_btn = ui_row(labs, 2);
    if (pad_connected()) { static const int k[2] = { K_A, K_B }; const char *l[2] = { T("RIPROVA", "RETRY"), "MENU" }; pad_hints(k, l, 2); }
}

// ---- attract-mode intro (1990s arcade style) ------------------------------------------------------------------
// Painted scenes with camera moves (pan, zoom, shake), letterbox bars, venetian-blind wipes, white
// flashes and typewriter captions, over an ACE-Step theme; then the title logo slams in.
typedef struct { const char *img; int t0, t1, mode; const char *it, *en; } Scene;
static const Scene kIntro[] = {
    { "intro0",  2200,  6000, 0, "ALL'ALBA, SUL LAGO...", "AT DAWN, ON THE LAKE..." },
    { "intro1",  6000,  9600, 1, "IL BASS PIU' GROSSO TI ASPETTA", "THE BIGGEST BASS IS WAITING" },
    { "intro2",  9600, 13200, 2, "FERRA AL MOMENTO GIUSTO!", "SET THE HOOK AT THE RIGHT MOMENT!" },
    { "intro3", 13200, 17600, 3, "DIVENTA IL RE DEL LAGO!", "BECOME THE KING OF THE LAKE!" },
};
#define INTRO_END 17800
static void draw_intro(int now) {
    const int t = now - s_state_ms;
    nv_gfx_rect(0, 0, W, H, C565(0, 0, 0));
    if (t < 2200) {                                        // "VERTICE" drops in letter by letter
        static const char word[] = "VERTICE";
        for (int i = 0; i < 7; i++) {
            const int li = t - i * 110;
            if (li < 0) continue;
            const int y = li < 300 ? -40 + li * 140 / 300 : 100;
            char c[2] = { word[i], 0 };
            text_sh(W / 2 - 7 * 21 + i * 42, y, c, li < 350 ? C_WHITE : C_CYAN, 6);
        }
        if (t > 1100) text_c(166, T("PRESENTA", "PRESENTS"), C_YELLOW, 2);
        if (t > 780 && t < 860) nv_gfx_rect(0, 0, W, H, C_WHITE);   // flash as the last letter lands
        return;
    }
    for (unsigned k = 0; k < sizeof kIntro / sizeof kIntro[0]; k++) {
        const Scene *s = &kIntro[k];
        if (t < s->t0 || t >= s->t1) continue;
        const float u = (t - s->t0) / (float)(s->t1 - s->t0);
        int w = W, h = H, x = 0, y = 0;
        if (s->mode == 0) { w = W * 5 / 4; h = H * 5 / 4; x = -iroundf(u * (w - W)); y = -(h - H) / 2; }
        else if (s->mode == 1) { const float z = 1.0f + 0.4f * u; w = iroundf(W * z); h = iroundf(H * z); x = (W - w) / 2; y = (H - h) / 2; }
        else if (s->mode == 2) { w = W * 6 / 5; h = H * 6 / 5; x = (W - w) / 2 + rnd(9) - 4; y = (H - h) / 2 + rnd(7) - 3; }
        else { const float z = 1.35f - 0.35f * u; w = iroundf(W * z); h = iroundf(H * z); x = (W - w) / 2; y = (H - h) / 2 - iroundf((1 - u) * 20); }
        nv_gfx_image(s->img, x, y, w, h);
        if (s->mode == 2)                                  // bubbles rising over the underwater shot
            for (int b = 0; b < 10; b++) {
                const int bx = (b * 97 + 40) % W, by = H - ((t / 4 + b * 53) % (H + 40));
                nv_gfx_circle(bx, by, 2 + b % 3, C565(200, 240, 255));
            }
        if (s->mode == 3)                                  // sparkles over the champion
            for (int b = 0; b < 14; b++) {
                if (((t / 90) + b) % 3) continue;
                const int bx = (b * 131 + t / 7) % W, by = 30 + (b * 71) % 200;
                nv_gfx_rect(bx - 3, by, 7, 1, C_YELLOW); nv_gfx_rect(bx, by - 3, 1, 7, C_YELLOW);
            }
        nv_gfx_rect(0, 0, W, 26, C565(0, 0, 0));           // letterbox
        nv_gfx_rect(0, H - 34, W, 34, C565(0, 0, 0));
        const char *cap = T(s->it, s->en);
        int n = 0;
        while (cap[n]) n++;
        const int shown = (t - s->t0) / 45 < n ? (t - s->t0) / 45 : n;   // typewriter
        char buf[64];
        for (int i = 0; i < shown && i < 63; i++) buf[i] = cap[i];
        buf[shown < 63 ? shown : 63] = 0;
        text_sh((W - nv_gfx_text_width(cap, 2)) / 2, H - 26, buf, C_YELLOW, 2);
        if (t - s->t0 < 90 && (s->mode == 1 || s->mode == 3)) nv_gfx_rect(0, 0, W, H, C_WHITE);   // impact flash
        const int left = s->t1 - t;
        if (left < 260) {                                  // venetian-blind wipe to the next scene
            const int k2 = (260 - left) * 20 / 260;
            for (int yy = 0; yy < H; yy += 20) nv_gfx_rect(0, yy, W, k2, C565(0, 0, 0));
        }
    }
    if ((now / 400) & 1) text_sh(W - 150, 8, T("TOCCA: SALTA", "TAP: SKIP"), C_GREY, 1);
}

// ---- sonar (as in the Konami games): the lake, fish schools as blips lit by the sweep, the boat and
// where the cast will land.
static void draw_sonar(int now) {
    const int mx = 40, my = 104, mr = 32;
    const float k = mr / 4100.0f;
    nv_gfx_circle(mx, my, mr + 1, C565(52, 120, 108));                  // hairline rim
    nv_gfx_circle(mx, my, mr, C565(6, 26, 30));
    nv_gfx_line(mx - mr + 4, my, mx + mr - 4, my, C565(16, 50, 50));    // faint cross-hair
    nv_gfx_line(mx, my - mr + 4, mx, my + mr - 4, C565(16, 50, 50));
    float sw = now * 0.0035f;
    sw -= (int)(sw / (2 * PI_F)) * 2 * PI_F;
    for (int j = 2; j >= 0; j--) {                                      // the sweep and its fading trail
        const float a = sw - j * 0.14f;
        static const uint16_t tr[3] = { C565(120, 240, 170), C565(40, 130, 90), C565(20, 70, 56) };
        nv_gfx_line(mx, my, mx + iroundf(sinf_(a) * (mr - 1)), my - iroundf(cosf_(a) * (mr - 1)), tr[j]);
    }
    for (int s = 0; s < NSPOTS; s++) {                                  // schools: lit as the sweep passes
        const int px = mx + iroundf(g_spot[s].x * k), py = my - iroundf(g_spot[s].z * k);
        const float lag = wrap_pi(sw - atan2f_(g_spot[s].x, g_spot[s].z));
        const int lit = lag > 0 && lag < 1.4f;
        nv_gfx_rect(px - 1, py - 1, 2 + lit, 2 + lit, lit ? C565(170, 255, 200) : C565(36, 110, 76));
    }
    if (s_boil >= 0 && now - s_boil_at < 1400 && ((now / 140) & 1))
        nv_gfx_rect(mx + iroundf(s_boil_x * k) - 1, my - iroundf(s_boil_z * k) - 1, 3, 3, C_YELLOW);
    const int bx = mx + iroundf(s_bx * k), by = my - iroundf(s_bz * k);
    const float fx = sinf_(s_aim), fz = cosf_(s_aim), reach = (400 + s_power * 2350) * k;
    for (int i = 2; i < iroundf(reach); i += 3)                         // the cast line, dotted
        nv_gfx_rect(bx + iroundf(fx * i), by - iroundf(fz * i), 1, 1, C565(230, 200, 60));
    nv_gfx_tri(bx + iroundf(fx * 5), by - iroundf(fz * 5), bx + iroundf(-fx * 3 + fz * 3), by - iroundf(-fz * 3 - fx * 3),
               bx + iroundf(-fx * 3 - fz * 3), by - iroundf(-fz * 3 + fx * 3), C_WHITE);
}

// ---- lake select: six painted cards, locked until the lake before is cleared -----------------------------
static void draw_select(int now) {
    char b[40], t[20];
    lake_art(s_sel);
    panel(8, 4, W - 16, 30);
    text_c(11, T("SCEGLI IL LAGO", "CHOOSE A LAKE"), C_YELLOW, 2);
    for (int k = 0; k < NSTAGES; k++) {
        const int x = 14 + (k % 3) * 164, y = 40 + (k / 3) * 108, w = 156, h = 100;
        const int sel = k == s_sel, open = k <= s_unlocked;
        if (sel) nv_gfx_rect(x - 4, y - 4, w + 8, h + 8, ((now / 150) & 1) ? C_YELLOW : C565(255, 150, 20));
        nv_gfx_rect(x, y, w, h, C_PANEL);
        if (open) {
            char n[8] = "lake0";
            n[4] = (char)('0' + k);
            nv_gfx_image(n, x + 2, y + 2, w - 4, 64);
        } else {                                           // a padlock
            nv_gfx_rect(x + 2, y + 2, w - 4, 64, C565(24, 28, 40));
            nv_gfx_circle(x + w / 2, y + 26, 10, C_GREY);
            nv_gfx_circle(x + w / 2, y + 26, 6, C565(24, 28, 40));
            nv_gfx_rect(x + w / 2 - 13, y + 28, 26, 22, C_GREY);
            nv_gfx_rect(x + w / 2 - 2, y + 34, 4, 9, C565(24, 28, 40));
        }
        fmt_int(t, k + 1); b[0] = 0; cat(b, t); cat(b, ". "); cat(b, lake_name(k));
        text_sh(x + 4, y + 70, b, open ? (sel ? C_YELLOW : C_WHITE) : C_GREY, 1);
        b[0] = 0; cat(b, "QUOTA "); fmt_kg(t, g_stage[k].quota_kg); cat(b, t);
        text_sh(x + 4, y + 84, open ? b : T("BLOCCATO", "LOCKED"), open ? C_CYAN : C_RED, 1);
    }
    const char *labs[4] = { "<", ">", "OK", "MENU" };
    s_screen_btn = ui_row(labs, 4);
    if (pad_connected()) { static const int k[3] = { K_DPAD, K_A, K_B }; const char *l[3] = { T("SCEGLI", "CHOOSE"), T("VIA!", "GO!"), "MENU" }; pad_hints(k, l, 3); }
}

// ---- the catch, shown on a painted pond: the fish bursts in at its size, the class slams down, the
// weight rolls up, the time bonus and combo, and the livewell verdict. Junk gets its own gag.
static void draw_catch(int now) {
    const int t = now - s_state_ms;
    char b[48], s[24];
    pond_art(s_stage);
    if (s_junk >= 0) {
        static const char *const jit[4] = { "LATTINA", "SCARPONE", "PNEUMATICO", "SCRIGNO!" };
        static const char *const jen[4] = { "TIN CAN", "OLD BOOT", "TYRE", "TREASURE!" };
        char n[8] = "junk0";
        n[4] = (char)('0' + s_junk);
        const int bounce = t < 300 ? (300 - t) / 3 : iroundf(sinf_(t * 0.006f) * 4);
        nv_gfx_image(n, W / 2 - 105, 56 - bounce, 210, 140);
        text_c(10, s_junk == 3 ? T("TESORO!", "TREASURE!") : "CLEAN UP!", ((t / 120) & 1) ? C_YELLOW : C_WHITE, t < 180 ? 7 : 5);
        panel(W / 2 - 150, 206, 300, 50);
        text_c(214, T(jit[s_junk], jen[s_junk]), C_WHITE, 2);
        b[0] = 0; cat(b, T("TEMPO +", "TIME +")); fmt_int(s, s_tb); cat(b, s);
        text_c(234, b, C_GREEN, 2);
        if (t < 70) nv_gfx_rect(0, 0, W, H, C_WHITE);
        return;
    }
    const int cls = size_class(s_catch_sp, s_catch_kg);
    if (cls >= 2) {                                        // a slow sunburst behind the big ones
        const float spin = t * 0.0007f;
        for (int k = 0; k < 8; k++) {
            const float a0 = spin + k * PI_F / 4, a1 = a0 + PI_F / 9;
            nv_gfx_tri(W / 2, 112, W / 2 + iroundf(cosf_(a0) * 420), 112 + iroundf(sinf_(a0) * 420),
                       W / 2 + iroundf(cosf_(a1) * 420), 112 + iroundf(sinf_(a1) * 420), cls == 3 ? C565(255, 214, 90) : C565(250, 236, 170));
        }
    }
    const int fw = 170 + cls * 45, fh = fw * 2 / 3;
    const float z = t < 240 ? t / 240.0f * 1.12f : t < 380 ? 1.12f - (t - 240) / 140.0f * 0.12f : 1.0f + sinf_(t * 0.005f) * 0.025f;
    const int w = iroundf(fw * z), h = iroundf(fh * z);
    fish_art_kg(s_catch_sp, s_catch_kg, W / 2 - w / 2, 112 - h / 2, w, h);
    static const char *const cit[4] = { "PICCOLINO", "PRESO!", "BEL PESCE!", "MOSTRO!" };
    static const char *const cen[4] = { "TIDDLER", "LANDED!", "NICE FISH!", "MONSTER!" };
    const int sc = cls == 0 ? 4 : (t < 200 ? 8 - t * 3 / 200 : 5);
    text_c(8, T(cit[cls], cen[cls]), cls >= 2 && ((t / 100) & 1) ? C_YELLOW : C_WHITE, sc);
    panel(W / 2 - 200, 206, 400, 66);
    text_sh(W / 2 - 190, 212, sp_name(s_catch_sp), s_catch_sp == SP_GOLD ? C_YELLOW : C_WHITE, 2);
    fmt_kg(s, s_catch_kg * clampf((t - 250) / 800.0f, 0, 1));
    text_sh(W / 2 - 190, 232, s, C_YELLOW, 4);
    b[0] = 0; cat(b, T("TEMPO +", "TIME +")); fmt_int(s, s_tb); cat(b, s);
    text_sh(W / 2 + 70, 212, b, C_GREEN, 2);
    if (s_combo >= 2) { b[0] = 0; cat(b, "COMBO x"); fmt_int(s, s_combo); cat(b, s); text_sh(W / 2 + 70, 230, b, C565(255, 150, 40), 2); }
    if (s_perfect) text_sh(W / 2 + 70, 248, T("FERRATA PERFETTA", "PERFECT HOOK-SET"), C_CYAN, 1);
    text_sh(W / 2 + 70, 260, well_keeps(s_catch_kg) ? T("NEL VIVAIO", "INTO THE LIVEWELL") : T("RILASCIATO", "RELEASED"),
            well_keeps(s_catch_kg) ? C_GREEN : C_GREY, 1);
    if (s_new_rank >= 3) {                                 // on the record wall: top right, under the title
        b[0] = 0; cat(b, T("RECORD N.", "RECORD #")); fmt_int(s, s_new_rank + 1); cat(b, s);
        panel(W - 128, 44, 120, 22);
        text_sh(W - 120, 51, b, (t / 200) & 1 ? C_GREEN : C_WHITE, 1);
    }
    // the stage total filling toward the quota
    nv_gfx_rect(W / 2 - 200, 280, 400, 12, C_SHADOW);
    nv_gfx_rect(W / 2 - 198, 282, iroundf(clampf(s_total / (s_quota > 0 ? s_quota : 1), 0, 1) * 396 * clampf((t - 400) / 600.0f, 0, 1)), 8,
                s_total >= s_quota ? C_GREEN : C_YELLOW);
    if (s_qual_now && t > 1100) text_c(184, T("QUOTA RAGGIUNTA!", "QUOTA REACHED!"), ((t / 120) & 1) ? C_GREEN : C_WHITE, 3);
    if (pad_connected() && t > 900) { key_badge(W - 100, 8, K_A); text_sh(W - 100 + key_w(K_A) + 5, 12, T("AVANTI", "NEXT"), C_WHITE, 1); }
    if (t < 70) nv_gfx_rect(0, 0, W, H, C_WHITE);
}

// ---- podium catch: a fish that makes the record wall's top three gets an arcade celebration --------------
// Rotating sunburst, the fish portrait bursting in with a bounce, the medal slamming down, bouncing
// "NEW RECORD" letters, a rank that counts in big, the weight rolling up, and confetti raining.
static void draw_podium(int now) {
    const int t = now - s_state_ms;
    static const uint16_t ray[3][2] = { { C565(255, 200, 30), C565(255, 120, 0) },     // gold
                                        { C565(210, 220, 240), C565(120, 140, 180) },  // silver
                                        { C565(230, 150, 90), C565(150, 80, 40) } };   // bronze
    const int rk = s_new_rank < 0 ? 0 : s_new_rank > 2 ? 2 : s_new_rank;
    nv_gfx_rect(0, 0, W, H, ray[rk][1]);
    const float spin = t * 0.0009f;
    const int cx = W / 2, cy = 124;
    for (int k = 0; k < 12; k++) {                        // sunburst: 12 wedges turning slowly
        const float a0 = spin + k * PI_F / 6, a1 = a0 + PI_F / 12;
        nv_gfx_tri(cx, cy, cx + iroundf(cosf_(a0) * 600), cy + iroundf(sinf_(a0) * 600),
                   cx + iroundf(cosf_(a1) * 600), cy + iroundf(sinf_(a1) * 600), ray[rk][0]);
    }
    // band 1 (y 6..44): bouncing "NEW RECORD!" letters
    const char *title = T("NUOVO RECORD!", "NEW RECORD!");
    int n = 0;
    while (title[n]) n++;
    const int tw = nv_gfx_text_width(title, 4);
    for (int i = 0; i < n; i++) {
        char c[2] = { title[i], 0 };
        const int y = 12 + iroundf(sinf_(t * 0.012f - i * 0.5f) * 5);
        text_sh((W - tw) / 2 + i * 24, y, c, ((t / 120 + i) % 3) ? C_WHITE : C_YELLOW, 4);
    }
    // band 2 (y 52..200): the fish bursting in, a medal each side; the rank under the left medal
    float z = t < 350 ? t / 350.0f * 1.15f : t < 550 ? 1.15f - (t - 350) / 200.0f * 0.15f : 1.0f + sinf_(t * 0.006f) * 0.03f;
    const int fw = iroundf(230 * z), fh = iroundf(146 * z);
    fish_art_kg(s_catch_sp, s_catch_kg, cx - fw / 2, cy - fh / 2, fw, fh);
    static const char *const medal[3] = { "a_gold", "a_silver", "a_bronze" };
    int my = 70;
    if (t < 700) my = -90;
    else if (t < 900) my = -90 + (t - 700) * 175 / 200;
    else if (t < 1000) my = 85 - (t - 900) * 15 / 100;
    nv_gfx_image(medal[rk], 18, my, 72, 72);
    nv_gfx_image(medal[rk], W - 90, my, 72, 72);
    if (t > 900) {
        char r[8];
        r[0] = (char)('1' + rk); r[1] = 0;
        cat(r, rk == 0 ? T("O", "ST") : rk == 1 ? T("O", "ND") : T("O", "RD"));
        text_sh(54 - nv_gfx_text_width(r, 4) / 2, 150, r, C_WHITE, 4);
        text_sh(W - 54 - nv_gfx_text_width(T("POSTO", "PLACE"), 2) / 2, 156, T("POSTO", "PLACE"), C_WHITE, 2);
    }
    // band 3 (y 214..292): the plate — name, weight rolling up, time bonus
    char s[20];
    panel(W / 2 - 170, 214, 340, 58);
    text_sh(W / 2 - 160, 222, sp_name(s_catch_sp), C_YELLOW, 2);
    fmt_kg(s, s_catch_kg * clampf((t - 600) / 900.0f, 0, 1));
    text_sh(W / 2 - 160, 242, s, C_WHITE, 3);
    if (s_tb) {
        char tb[20], m[8];
        tb[0] = 0; cat(tb, T("TEMPO +", "TIME +")); fmt_int(m, s_tb); cat(tb, m);
        text_sh(W / 2 + 160 - nv_gfx_text_width(tb, 2), 246, tb, C_GREEN, 2);
    }
    if (pad_connected() && t > 2200) { key_badge(W / 2 - 40, 278, K_A); text_sh(W / 2 - 40 + key_w(K_A) + 5, 282, T("AVANTI", "NEXT"), C_WHITE, 1); }
    // confetti
    for (int k = 0; k < 40; k++) {
        const int x = (k * 53 + (t / (6 + k % 5))) % W, y = (k * 37 + t / (3 + k % 4)) % H;
        static const uint16_t col[5] = { C565(255, 60, 60), C565(60, 200, 255), C565(255, 230, 60), C565(90, 230, 90), C565(230, 90, 230) };
        nv_gfx_rect(x, y, 4 + (k & 1) * 2, 3 + ((t / 100 + k) & 1) * 3, col[k % 5]);
    }
    if (t < 90 || (t > 880 && t < 950)) nv_gfx_rect(0, 0, W, H, C_WHITE);   // flashes: the burst, the slam
}

// ---- the game loop -----------------------------------------------------------------------------------------------------------
NV_EXPORT("run")
void run(void) {
    char lang[8] = "";
    nv_lang(lang, sizeof lang);
    s_it = lang[0] == 'i' && lang[1] == 't';
    records_load();
    if (nv_load("unlock.bin", &s_unlocked, 4) != 4 || s_unlocked < 0 || s_unlocked >= NSTAGES) s_unlocked = 0;
#ifdef BASS_TEST_UNLOCK
    s_unlocked = NSTAGES - 1;
#endif
    s_stage = 0; s_loop = 0;
    lake_build(0, 0); fish_build(); build_lures(); lake_view(0);
    int last = nv_millis();
    s_state_ms = last;
    sfx("intro");
    s_state = ST_INTRO;
#ifdef BASS_TEST_CATCH   // simulator only: open straight on a catch (1 fish, 2 podium, 3 junk)
    s_catch_sp = BASS_TEST_SP; s_catch_kg = BASS_TEST_KG; s_tb = 12; s_combo = 3; s_perfect = 1; s_total = 3.9f; s_quota = 2.4f;
    s_qual_now = 1; s_list_sp[0] = (uint8_t)s_catch_sp; s_list_kg[0] = s_catch_kg; s_catches = 1;
    s_new_rank = BASS_TEST_CATCH == 2 ? 1 : 5; s_junk = BASS_TEST_CATCH == 3 ? 1 : -1;
    s_state = ST_CATCH;
    if (BASS_TEST_CATCH == 4) {           // the tournament-over page
        s_run_catches = 7; s_stages_cleared = 2; s_run_total = 9.4f; s_run_best_kg = s_catch_kg; s_run_best_sp = s_catch_sp;
        s_state = ST_OVER;
    }
#endif
    int music_at = last;
    while (nv_gfx_present()) {
        const int now = nv_millis();
        float dt = (now - last) / 1000.0f;
        last = now;
        if (dt > 0.05f) dt = 0.05f;
        s_shake *= 1.0f - clampf(dt * 7, 0, 1);
        read_input();
        const int in_play = s_state == ST_AIM || s_state == ST_CAST || s_state == ST_RETRIEVE || s_state == ST_STRIKE ||
                            s_state == ST_FIGHT || s_state == ST_LOST;
        // Pause: the "II" key in play, START/SELECT on a pad. Resume, back to the menu, or quit.
        if (in_play && !s_paused && ((s_in.tap && in_rect(&kPause, s_in.tx, s_in.ty)) || pressed(NV_PAD_START | NV_PAD_SELECT))) {
            s_paused = 1; s_pause_sel = 0; s_in.tap = 0;
        }
        if (s_paused) {
            if (pressed(NV_PAD_UP)) s_pause_sel = (s_pause_sel + 2) % 3;
            if (pressed(NV_PAD_DOWN)) s_pause_sel = (s_pause_sel + 1) % 3;
            vx_render();
            panel(W / 2 - 120, 60, 240, 170);
            text_c(72, T("PAUSA", "PAUSED"), C_YELLOW, 3);
            static const char *const it[3] = { "RIPRENDI", "MENU", "ESCI" }, *const en[3] = { "RESUME", "MENU", "QUIT" };
            int hit = -1;
            for (int i = 0; i < 3; i++) {
                if (s_pause_sel == i) nv_gfx_rect(W / 2 - 104, 104 + i * 40, 208, 36, C_YELLOW);
                if (ui_btn(W / 2 - 100, 106 + i * 40, 200, 32, T(it[i], en[i]), C_CYAN)) hit = i;
            }
            if (pressed(NV_PAD_A)) hit = s_pause_sel;
            if (pressed(NV_PAD_B)) hit = 0;
            if (hit == 0) { s_paused = 0; last = nv_millis(); }
            if (hit == 1) { s_paused = 0; lake_build(0, 0); fish_build(); build_lures(); lake_view(0); s_menu = 0; s_logo_at = now; go(ST_TITLE, now); }
            if (hit == 2) return;
            continue;
        }
        if (nv_gfx_back()) {
            if (s_state == ST_TITLE) return;
            if (s_state == ST_RECORDS) { go(ST_TITLE, now); }
            else { lake_build(0, 0); fish_build(); build_lures(); lake_view(0); s_menu = 0; go(ST_TITLE, now); }
        }
        // Music on the menus: the theme comes round again every 30 s while nothing else plays.
        if ((s_state == ST_TITLE || s_state == ST_RECORDS) && now - music_at > 30500) { music_at = now; sfx("menu"); }
        const int ticking = s_state == ST_AIM || s_state == ST_CAST || s_state == ST_RETRIEVE || s_state == ST_STRIKE || s_state == ST_FIGHT;
        const int go_hold = s_go_at && now - s_go_at < 1500;           // READY/GO: the clock waits
        if (ticking && !go_hold) s_time_ms -= (int)(dt * 1000);
        if (ticking && s_time_ms > 0 && s_time_ms < 10000) {          // the final ten: a beep each second
            const int sec = s_time_ms / 1000;
            if (sec != s_last_sec) { s_last_sec = sec; nv_gfx_tone(sec < 3 ? 1760 : 1320, 70); }
        }
        const int time_up = s_time_ms <= 0 && s_state != ST_FIGHT && ticking;
        if (time_up) {
            s_time_ms = 0; fish_hide(); lure_hide(); lake_view(0);
            sfx("bell");
            go(ST_WEIGH, now);
        }

        switch (s_state) {
        case ST_INTRO:
            if (now - s_state_ms > INTRO_END || s_in.tap || pressed(NV_PAD_A | NV_PAD_START | NV_PAD_B)) {
                s_logo_at = now; s_menu = 0; music_at = now; sfx("menu");
                go(ST_TITLE, now);
            }
            break;
        case ST_TITLE: {
            if (now - s_state_ms > 45000) { sfx("intro"); go(ST_INTRO, now); break; }   // attract mode
            s_orbit += dt * 0.12f;
            cam(sinf_(s_orbit) * 900, 260, cosf_(s_orbit) * 900 + 600, 0, 40, 900, 60);
            if (s_in.up || pressed(NV_PAD_UP)) s_menu = 0;
            if (s_in.down || pressed(NV_PAD_DOWN)) s_menu = 1;
            int pick = -1;
            if (s_in.tap) for (int i = 0; i < 2; i++) if (s_in.tx > W / 2 && s_in.ty >= 160 + i * 34 && s_in.ty < 188 + i * 34) pick = i;
            if (pressed(NV_PAD_A | NV_PAD_START)) pick = s_menu;
            if (s_title_btn == 0) s_menu = 0;
            if (s_title_btn == 1) s_menu = 1;
            if (s_title_btn == 2) pick = s_menu;
            if (s_title_btn == 3 || pressed(NV_PAD_SELECT)) return;
            s_title_btn = -1;
            if (s_in.tap || s_pad) s_state_ms = s_state_ms > now - 45000 ? s_state_ms : now;   // (activity delays attract)
            if (pick == 0) { s_sel = s_unlocked; s_new_rank = -1; snd_click(); go(ST_SELECT, now); }
            if (pick == 1) { s_new_rank = -1; go(ST_RECORDS, now); }
            break;
        }
        case ST_SELECT: {
            int mv = 0, start = s_screen_btn == 2 || pressed(NV_PAD_A | NV_PAD_START);
            if (pressed(NV_PAD_LEFT) || s_screen_btn == 0) mv = -1;
            if (pressed(NV_PAD_RIGHT) || s_screen_btn == 1) mv = 1;
            if (pressed(NV_PAD_UP)) mv = -3;
            if (pressed(NV_PAD_DOWN)) mv = 3;
            if (mv) { s_sel = (s_sel + mv + NSTAGES) % NSTAGES; snd_click(); }
            if (s_in.tap && s_in.ty > 36 && s_in.ty < 252) {   // tap a card to pick it, again to go
                const int k = (int)clampf((s_in.tx - 10) / 164.0f, 0, 2) + (s_in.ty >= 144 ? 3 : 0);
                if (k == s_sel) start = 1; else { s_sel = k; snd_click(); }
            }
            if (s_screen_btn == 3 || pressed(NV_PAD_B)) { s_screen_btn = -1; s_logo_at = now; go(ST_TITLE, now); break; }
            s_screen_btn = -1;
            if (start && now - s_state_ms > 250) {
                if (s_sel <= s_unlocked) {
                    snd_click(); s_stage = s_sel; s_loop = 0; s_run_total = 0;
                    s_run_catches = 0; s_run_best_kg = 0; s_stages_cleared = 0;
                    start_stage(now);
                }
                else { sfx("fail"); msg(T("LAGO BLOCCATO", "LAKE LOCKED"), now, 900); }
            }
            break;
        }
        case ST_RECORDS:
            s_orbit += dt * 0.12f;
            cam(sinf_(s_orbit) * 900, 260, cosf_(s_orbit) * 900 + 600, 0, 40, 900, 60);
            if (s_screen_btn == 0 || pressed(NV_PAD_A | NV_PAD_B | NV_PAD_START)) { s_screen_btn = -1; go(ST_TITLE, now); }
            break;
        case ST_STAGE:
            s_orbit += dt * 0.15f;
            cam(sinf_(s_orbit) * 700, 220, cosf_(s_orbit) * 700 + 700, 0, 30, 900, 60);
            if (now - s_state_ms > 400 && (s_screen_btn == 0 || pressed(NV_PAD_A | NV_PAD_START))) { snd_click(); go(ST_LURE, now); }
            if (s_screen_btn == 1 || pressed(NV_PAD_B)) { lake_build(0, 0); fish_build(); build_lures(); lake_view(0); s_logo_at = now; go(ST_TITLE, now); }
            s_screen_btn = -1;
            break;
        case ST_LURE:
            aim_camera();
            if (pressed(NV_PAD_LEFT)) { s_lure = (s_lure + NLURES - 1) % NLURES; snd_click(); }
            if (pressed(NV_PAD_RIGHT)) { s_lure = (s_lure + 1) % NLURES; snd_click(); }
            if (s_screen_btn == 0) { s_lure = (s_lure + NLURES - 1) % NLURES; snd_click(); }
            if (s_screen_btn == 1) { s_lure = (s_lure + 1) % NLURES; snd_click(); }
            if (s_screen_btn == 3 || pressed(NV_PAD_B)) { lake_build(0, 0); fish_build(); build_lures(); lake_view(0); s_logo_at = now; s_screen_btn = -1; go(ST_TITLE, now); break; }
            if (s_screen_btn == 2 && now - s_state_ms > 250) { s_screen_btn = -1; snd_click(); to_aim(now); break; }
            s_screen_btn = -1;
            if (s_in.tap && s_in.ty > 66 && s_in.ty < 230) {
                s_lure = (int)clampf((s_in.tx - 8) / 125.0f, 0, NLURES - 1); snd_click(); to_aim(now);
            } else if (pressed(NV_PAD_A | NV_PAD_START) && now - s_state_ms > 250) { snd_click(); to_aim(now); }
            break;
        case ST_AIM: {
            {   // Steer, and drive: up/down crank the outboard, then run the boat to a new spot.
                const float turn = 0.9f + fabsf_(s_bspeed) / 460.0f * 0.5f;
                if (s_in.left) s_aim -= dt * turn;
                if (s_in.right) s_aim += dt * turn;
                s_aim = wrap_pi(s_aim);
                const int drive = s_charging ? 0 : (s_in.up ? 1 : s_in.down ? -1 : 0);
                if (drive && s_engine == 0) { s_engine = 1; s_engine_at = now; sfx("motor_start"); s_shake = 5; }
                if (s_engine == 1 && now - s_engine_at > 900) { s_engine = 2; s_motor_at = 0; }
                const float want = s_engine == 2 ? drive * (drive > 0 ? 460.0f : 170.0f) : 0.0f, acc = drive ? 360.0f : 240.0f;
                s_bspeed += clampf(want - s_bspeed, -dt * acc, dt * acc);
                if (s_engine == 2 && (drive || fabsf_(s_bspeed) > 20) && now - s_motor_at > 950) { s_motor_at = now; sfx("motor"); }
                if (s_engine == 2 && !drive && fabsf_(s_bspeed) < 5 && now - s_motor_at > 2600) s_engine = 0;   // idles, then cuts out
                s_bx += sinf_(s_aim) * s_bspeed * dt; s_bz += cosf_(s_aim) * s_bspeed * dt;
                const float r = sqrtf_(s_bx * s_bx + s_bz * s_bz);
                const float lim = lake_shore(atan2f_(s_bx, s_bz)) - 520;     // keep off the bank
                if (r > lim) { s_bx *= lim / r; s_bz *= lim / r; s_bspeed *= 0.4f; }
                if (fabsf_(s_bspeed) > 60 && (now / 50) % 2 == 0) {    // wake off the stern, spray off the bow
                    const float fx = sinf_(s_aim), fz = cosf_(s_aim), sd = s_bspeed > 0 ? 1.0f : -1.0f, j = (float)(rnd(80) - 40);
                    vx_emit(g_fx_splash, iroundf(s_bx - fx * 200 * sd + fz * j), 4, iroundf(s_bz - fz * 200 * sd - fx * j), 0, 70, 0, 60, 2);
                    if (s_bspeed > 250) {
                        vx_emit(g_fx_splash, iroundf(s_bx + fx * 150 + fz * 50), 16, iroundf(s_bz + fz * 150 - fx * 50), 0, 110, 0, 60, 1);
                        vx_emit(g_fx_splash, iroundf(s_bx + fx * 150 - fz * 50), 16, iroundf(s_bz + fz * 150 + fx * 50), 0, 110, 0, 60, 1);
                    }
                }
            }
            if (s_in.b_hit) { s_lure = (s_lure + 1) % NLURES; snd_click(); }
            aim_camera();
            lake_birds(now);
            if (s_charging && s_in.a) rod_seek(W - 40 - 20 * s_power, 26 + 10 * s_power, 26, 14, dt);   // wound back
            else rod_seek(W - 170, 96, 0, 8, dt);
            if (now - s_boil_at > 2600 + rnd(2200)) {           // fish break the surface now and then
                s_boil = rnd(NSPOTS); s_boil_at = now;
                s_boil_x = g_spot[s_boil].x + rnd(160) - 80; s_boil_z = g_spot[s_boil].z + rnd(160) - 80;
                vx_emit(g_fx_splash, iroundf(s_boil_x), 4, iroundf(s_boil_z), 0, 220, 0, 110, 10);
            }
            if ((now / 70) % 2 == 0) {                         // sun glints dancing on the water
                const float ga = s_aim + (rnd(1000) / 1000.0f - 0.5f) * 1.2f, gd = 500 + rnd(2200);
                vx_emit(g_fx_glint, iroundf(s_bx + sinf_(ga) * gd), 3, iroundf(s_bz + cosf_(ga) * gd), 0, 0, 0, 0, 1);
            }
            if (s_in.a_hit) s_charging = 1;                   // a fresh press starts the charge
            if (s_in.a && s_charging) {                       // charge: the power swings up and down
                s_power_t += dt;
                const float ph = s_power_t / 1.3f;
                s_power = 1.0f - fabsf_((ph - (int)ph) * 2 - 1);
            } else if (s_power_t > 0 && s_charging) {         // release: cast
                s_charging = 0;
                const float d = cast_reach(400 + s_power * 2350, &s_bank);   // long casts: up to ~27 m
                s_tx = s_bx + sinf_(s_aim) * d; s_tz = s_bz + cosf_(s_aim) * d;
                s_bspeed = 0;
                s_cast_len = 0.35f + d / 2400; s_cast_t = 0;
                s_power_t = 0; s_release_t = 0;
                sfx("cast");
                go(ST_CAST, now);
            }
            break;
        }
        case ST_CAST: {
            aim_camera();
            lake_birds(now);
            s_cast_t += dt;
            s_release_t += dt;
            if (s_release_t < 0.16f) rod_seek(W / 2 + 70, 150, -30, 30, dt);    // the whip
            else rod_seek(W - 190, 104, 8, 6, dt);                               // follow-through
            float rx, ry, rz;
            rod_tip(&rx, &ry, &rz);
            const float t = clampf(s_cast_t / s_cast_len, 0, 1);
            s_lx = rx + (s_tx - rx) * t; s_lz = rz + (s_tz - rz) * t;
            s_ly = ry + (6 - ry) * t + sinf_(t * PI_F) * 320;
            lure_pose(s_lx, s_ly, s_lz, s_aim);
            if (t >= 1.0f && now - s_state_ms < 100000) {
                if (s_cast_t - s_cast_len < dt) {
                    vx_emit(g_fx_splash, iroundf(s_tx), 4, iroundf(s_tz), 0, 260, 0, 160, 18);
                    snd_splash();
                    const int spot = lake_spot_near(s_tx, s_tz);
                    msg(s_bank ? T("SOTTO RIVA!", "UNDER THE BANK!") : spot >= 0 ? T("BUON POSTO!", "NICE SPOT!") : T("ACQUA APERTA", "OPEN WATER"), now, 900);
                }
                if (s_cast_t > s_cast_len + 0.5f) {           // dive under
                    lake_view(1);
                    fish_spawn(s_tx, s_tz, s_stage);
                    s_lx = s_tx; s_lz = s_tz; s_ly = SURF - 8; s_twitch_t = 0; s_twitches = 0;
                    {
                        float an[3];
                        rope_anchor(an);
                        const float ex = s_lx - an[0], ey = s_ly - an[1], ez = s_lz - an[2];
                        s_line = sqrtf_(ex * ex + ey * ey + ez * ez) + 60;   // a little slack from the cast
                        s_rope_ok = 0;
                    }
                    vx_emit(g_fx_bubble, iroundf(s_lx), iroundf(s_ly), iroundf(s_lz), 0, 120, 0, 60, 16);
                    {   // this frame already renders under water: put the camera there too
                        const float dd = sqrtf_((s_lx - s_bx) * (s_lx - s_bx) + (s_lz - s_bz) * (s_lz - s_bz)) + 1e-3f, ux = (s_lx - s_bx) / dd, uz = (s_lz - s_bz) / dd;
                        const float back = dd > 330 ? 260.0f : dd - 70.0f;
                        cam(s_lx - ux * back, SURF - 10, s_lz - uz * back, s_lx + ux * 60, s_ly - 30, s_lz + uz * 60, 66);
                    }
                    go(ST_RETRIEVE, now);
                }
            }
            break;
        }
        case ST_RETRIEVE:
        case ST_STRIKE: {
            // Lure physics: reel (A held), pause, twitch (DOWN: the rod pulled back), give line (B held:
            // the lure sinks freely and drifts away from the boat).
            const int pay = s_in.b && s_state == ST_RETRIEVE;
            const int reel = s_in.a && !pay && s_state == ST_RETRIEVE;
            const float ramt = reel ? s_reel_amt : 0.0f;
            if (s_in.d_hit && s_state == ST_RETRIEVE) {
                s_twitch_t = 0.35f; s_twitches++;
                vx_emit(g_fx_bubble, iroundf(s_lx), iroundf(s_ly), iroundf(s_lz), 0, 80, 0, 40, s_lure == LURE_POPPER ? 10 : 4);
                sfx(s_lure == LURE_POPPER ? "plop" : "click");
            }
            s_twitch_t -= dt;
            float speed = reel ? (s_lure == LURE_WORM ? 150.0f : s_lure == LURE_POPPER ? 210.0f : s_lure == LURE_JIG ? 190.0f : 260.0f) * ramt : 0;
            if (s_twitch_t > 0.2f) speed += 420;
            float depth_target;
            if (s_lure == LURE_POPPER) depth_target = SURF - 8;
            else if (s_lure == LURE_CRANK) depth_target = reel ? SURF - 240 : SURF - 8;
            else if (s_lure == LURE_JIG) depth_target = reel ? 60 : 14;        // heavy head: dives to the bed
            else depth_target = reel ? 90 : 22;
            float dv = s_lure == LURE_JIG ? (reel ? 90.0f : 190.0f) : (s_lure == LURE_WORM && !reel) ? 90.0f : 110.0f;
            if (pay) {                                     // slack line: nothing pulls — sinkers sink, floaters rise
                if (s_lure == LURE_WORM || s_lure == LURE_JIG) { depth_target = 12; dv = s_lure == LURE_JIG ? 190.0f : 80.0f; }
                else { depth_target = SURF - 8; dv = 60; }
            }
            if (s_lure == LURE_JIG && s_in.d_hit && s_state == ST_RETRIEVE) {  // twitch = a hop off the bottom
                s_ly = clampf(s_ly + 80, 0, SURF - 20);
                vx_emit(g_fx_dust, iroundf(s_lx), 6, iroundf(s_lz), 0, 60, 0, 40, 6);   // a puff of silt
            }
            s_ly += clampf(depth_target - s_ly, -dv * dt, dv * dt);
            const float d = sqrtf_((s_lx - s_bx) * (s_lx - s_bx) + (s_lz - s_bz) * (s_lz - s_bz)) + 1e-3f;
            const float ux = (s_lx - s_bx) / d, uz = (s_lz - s_bz) / d;
            if (s_state == ST_RETRIEVE) {
                // The reel takes in line; the line, not the reel, moves the lure: while there is
                // slack the lure doesn't budge, once it is taut the lure comes toward the rod.
                s_line -= speed * dt;
                if (pay) s_line += 160 * dt;                       // line off the spool
                if (s_line > 4200) s_line = 4200;
                float an[3];
                rope_anchor(an);
                {
                    const float ex = s_lx - an[0], ey = s_ly - an[1], ez = s_lz - an[2];
                    const float L = sqrtf_(ex * ex + ey * ey + ez * ez) + 1e-3f;
                    if (L > s_line) { const float k = s_line / L; s_lx = an[0] + ex * k; s_lz = an[2] + ez * k; s_ly = an[1] + ey * k; }
                    if (s_line < L - 400) s_line = L - 400;       // never more than a little slack wound in
                }
                // Rod left/right swings the lure sideways across the line (steer it past cover).
                if (lake_collide(&s_lx, &s_ly, &s_lz, 10)) {     // the lure bumps over rocks and logs
                    static int bump_at;
                    if (now - bump_at > 350) {
                        bump_at = now; sfx("click");
                        vx_emit(g_fx_dust, iroundf(s_lx), iroundf(s_ly), iroundf(s_lz), 0, 40, 0, 40, 4);
                    }
                }
                const int steer = s_in.right - s_in.left;
                if (steer) { const float sv = (reel ? 130.0f : 75.0f) * steer * dt; s_lx += uz * sv; s_lz -= ux * sv; }
                if (s_ly < 30 && (speed > 0 || s_twitch_t > 0)) {    // skimming the bed: a trail of silt
                    static int silt_at;
                    if (now - silt_at > 300) { silt_at = now; vx_emit(g_fx_dust, iroundf(s_lx), 8, iroundf(s_lz), 0, 30, 0, 30, 2); }
                }
            }
            rope_step(dt);
            const float yaw = atan2f_(-ux, -uz);
            {   // how hard the lure works: a twitch kicks it, reeling keeps it busy, a pause lets it settle
                const float want = s_twitch_t > 0 ? 1.4f : reel ? 1.0f : 0.25f;
                s_lure_wave += (want - s_lure_wave) * clampf(dt * 6, 0, 1);
            }
            lure_pose(s_lx, s_ly, s_lz, yaw);
            if (reel) {
                sfx_reel(now);
                static int trail_at;
                if (now - trail_at > 140) {                        // a thin trail of bubbles off the lure
                    trail_at = now;
                    vx_emit(g_fx_bubble, iroundf(s_lx - ux * 20), iroundf(s_ly + 6), iroundf(s_lz - uz * 20), 0, 40, 0, 12, 1);
                }
            }
            // Camera: behind the lure, facing the boat.
            // Camera on the boat's side, looking out at the lure: reeling brings it (and the fish
            // chasing it) toward you. Weeds right in front of the lens are hidden.
            {
                const float back = d > 400 ? 330.0f : d - 70.0f;    // never behind the boat
                cam(s_lx - ux * back, clampf(s_ly + 95, 70, SURF - 10), s_lz - uz * back, s_lx + ux * 160, s_ly + 25, s_lz + uz * 160, 66);
                lake_clear_near(s_lx - ux * back * 0.6f, s_lz - uz * back * 0.6f, 190);
            }
            if ((now / 120) % 4 == 0)                             // drifting specks in the water
                vx_emit(g_fx_dust, iroundf(s_lx + rnd(500) - 250), iroundf(s_ly + rnd(200) - 100), iroundf(s_lz + rnd(500) - 250), 0, 10, 0, 20, 1);
            LureState ls = { s_twitch_t > 0 ? 2 : (reel ? 0 : 1), s_lx, s_ly, s_lz, s_lure };
            const int nib = s_state == ST_RETRIEVE ? fish_nibbling() : -1;
            if (nib >= 0) {                                        // a fish mouthing the lure
                static int tick_at;
                if (now - tick_at > 260) { tick_at = now; sfx("click"); }
                lure_pose(s_lx, s_ly + sinf_(now * 0.06f) * 4, s_lz, yaw + sinf_(now * 0.05f) * 0.2f);
                if (s_in.d_hit || s_in.a_hit) {                    // struck at a nibble: too early
                    fish_spook(nib);
                    msg(T("TROPPO PRESTO!", "TOO EARLY!"), now, 900);
                    sfx("splash");
                }
            }
            s_nibble = nib;
            const int st = fish_update(&ls, dt, now);
            if (s_state == ST_RETRIEVE) {
                if (st >= 0) {
                    s_strike_fish = st; s_strike_until = now + 850;
                    s_junk = rnd(100) < 7 ? (rnd(100) < 15 ? 3 : rnd(3)) : -1;   // sometimes the "bite" is junk
                    snd_strike();
                    vx_emit(g_fx_bubble, iroundf(s_lx), iroundf(s_ly), iroundf(s_lz), 0, 160, 0, 80, 20);
                    go(ST_STRIKE, now);
                } else if (d < 130) {
                    msg(T("RECUPERATA", "REELED IN"), now, 700);
                    to_aim(now);
                }
            } else {                                               // strike window: set the hook!
                fish_pose(s_strike_fish, s_lx + ux * 34, s_ly, s_lz + uz * 34, yaw, sinf_(now * 0.05f) * 0.25f, 0);
                if ((s_in.d_hit || s_in.a_hit) && s_junk >= 0) {       // junk on the hook: CLEAN UP!
                    static const int bonus[4] = { 8, 12, 10, 25 };
                    fish_release_others(-1);
                    s_tb = bonus[s_junk]; s_time_ms += s_tb * 1000; s_tb_at = nv_millis();
                    s_new_rank = -1; s_perfect = 0;
                    sfx("junk"); lake_view(0); lure_hide();
                    go(ST_CATCH, now);
                } else if (s_in.d_hit || s_in.a_hit) {       // set the hook: rod back or crank
                    s_perfect = now - (s_strike_until - 850) < 300;     // a snap hook-set tires the fish
                    fish_release_others(s_strike_fish);
                    fight_start(&s_fight, s_strike_fish, s_lx - s_bx, s_ly, s_lz - s_bz);
                    if (s_perfect) { s_fight.stamina = 0.8f; msg(T("FERRATA PERFETTA!", "PERFECT HOOK-SET!"), now, 1100); }
                    else if (fish_kg(s_strike_fish) >= 4.0f) msg(T("PESCE GROSSO!", "BIG ONE!"), now, 1100);
                    banner("FISH ON!", C_YELLOW, now);
                    rumble(30000, 60000, 300);
                    s_shake = 16; s_flash_until = now + 60; s_hitstop_until = now + 140;
                    s_fyaw = atan2f_(ux, uz); s_fpx = s_lx; s_fpz = s_lz;
                    s_ccp[0] = s_lx - ux * 300; s_ccp[1] = s_ly + 80; s_ccp[2] = s_lz - uz * 300;
                    s_cct[0] = s_lx; s_cct[1] = s_ly; s_cct[2] = s_lz;
                    s_rtx = W / 2 + 40; s_rty = 150;
                    sfx("fish_on");
                    vx_emit(g_fx_spark, iroundf(s_lx), iroundf(s_ly), iroundf(s_lz), 0, 60, 0, 140, 24);
                    go(ST_FIGHT, now);
                } else if (now > s_strike_until) {
                    fish_release_others(-1);
                    msg(T("TROPPO TARDI!", "TOO LATE!"), now, 900);
                    sfx("splash");
                    go(ST_RETRIEVE, now);
                }
            }
            break;
        }
        case ST_FIGHT: {
            const int rod = s_in.left ? -1 : s_in.right ? 1 : 0;
            g_rod_lift = (s_in.down || s_in.b) ? -1 : s_in.up ? 1 : 0;
            const int r = now < s_hitstop_until ? 0 : fight_update(&s_fight, rod, s_in.b ? 0.0f : s_reel_amt, s_in.b_hit || s_in.d_hit, dt);
            {   // the pad feels it: a jolt on each hard run, a buzz while the line is in the red
                static int buzz_at;
                if (s_fight.surge > 0.38f) rumble(26000, 42000, 160);
                else if (s_fight.tension > 0.88f && now - buzz_at > 220) { buzz_at = now; rumble(9000, 16000, 120); }
            }
            if (s_fight.surge > 0.35f && s_shake < 9) s_shake = 9;      // a hard run jolts the view
            if (s_fight.tension > 0.9f && s_shake < 3) s_shake = 3;
            // Where the fish is: along the line from the boat, pulled sideways by its runs.
            const float d = sqrtf_((s_lx - s_bx) * (s_lx - s_bx) + (s_lz - s_bz) * (s_lz - s_bz)) + 1e-3f, ux = (s_lx - s_bx) / d, uz = (s_lz - s_bz) / d;
            const float fx = s_bx + ux * s_fight.dist + uz * s_fight.fx, fz = s_bz + uz * s_fight.dist - ux * s_fight.fx;
            // Heading: a running fish points where it swims; one being dragged in (tired, or reeled
            // while it isn't running) comes mouth-first toward the boat. Turned at a fish's pace.
            {
                const float vx = (fx - s_fpx) / (dt > 1e-3f ? dt : 1e-3f), vz = (fz - s_fpz) / (dt > 1e-3f ? dt : 1e-3f);
                s_fpx = fx; s_fpz = fz;
                const float sp = sqrtf_(vx * vx + vz * vz);
                float want;
                if (s_fight.stamina < 0.15f || (s_in.a && s_fight.run < 0.45f)) want = atan2f_(s_bx - fx, s_bz - fz);
                else if (sp > 30) want = atan2f_(vx, vz);
                else want = atan2f_(ux, uz) + s_fight.run_dir * 0.9f;
                s_fyaw = wrap_pi(s_fyaw + clampf(wrap_pi(want - s_fyaw), -dt * 2.6f, dt * 2.6f));
            }
            const float effort = clampf(s_fight.run, 0, 1) * (0.3f + 0.7f * s_fight.stamina);
            const float wig = sinf_(now * (0.010f + 0.012f * effort)) * (0.05f + 0.22f * effort);
            const float pitch = s_fight.jumping ? -0.6f : (s_fight.stamina < 0.2f ? -0.25f : 0.0f);
            fish_pose(s_fight.fish, fx, s_fight.fy, fz, s_fyaw, wig, pitch);
            lure_hide();
            {   // camera eases after the fish instead of being bolted to it
                const float want_p[3] = { fx - ux * 300, clampf(s_fight.fy + 85, 50, SURF - 20), fz - uz * 300 };
                const float want_t[3] = { fx, s_fight.fy - 10, fz };
                const float k = clampf(dt * 3.5f, 0, 1);
                for (int j = 0; j < 3; j++) { s_ccp[j] += (want_p[j] - s_ccp[j]) * k; s_cct[j] += (want_t[j] - s_cct[j]) * k; }
                cam(s_ccp[0], s_ccp[1], s_ccp[2], s_cct[0], s_cct[1], s_cct[2], 64);
                lake_clear_near((s_ccp[0] + s_cct[0]) / 2, (s_ccp[2] + s_cct[2]) / 2, 190);
            }
            {   // the rod bends toward the fish, more under tension; dips on a jump
                int sx = W / 2, sy = H / 2;
                project(fx, s_fight.fy, fz, &sx, &sy);
                const float tx = W - 170 + (sx - W / 2) * 0.35f,
                            ty = 70 + s_fight.tension * 60 + (s_fight.jumping ? 50 : 0) - g_rod_lift * 34;
                rod_seek(tx, ty, 18 + s_fight.tension * 70, 6, dt);
            }
            if ((now / 160) % 3 == 0) vx_emit(g_fx_bubble, iroundf(fx), iroundf(s_fight.fy + 20), iroundf(fz), 0, 100, 0, 30, 2);
            if (s_fight.jumping && (now / 100) % 2 == 0) vx_emit(g_fx_bubble, iroundf(fx), iroundf(s_fight.fy), iroundf(fz), 0, 260, 0, 90, 6);
            if (s_in.a || s_fight.drag) sfx_reel(now);             // cranking, or the drag clicking
            {   // a jump starting: the splash
                static int was_jumping;
                if (s_fight.jumping && !was_jumping) sfx("jump");
                was_jumping = s_fight.jumping;
            }
            if (s_fight.tension > 0.85f && (now / 240) % 2 == 0 && now - s_reel_at > 150) nv_gfx_tone(2400, 25);
            s_lx = s_bx + ux * (s_fight.dist + 1); s_lz = s_bz + uz * (s_fight.dist + 1);   // keep the line direction
            if (r == 1) {
                s_catch_sp = fish_species(s_fight.fish); s_catch_kg = fish_kg(s_fight.fish);
                s_junk = -1;
                well_add(s_catch_sp, s_catch_kg);
                s_run_catches++;
                if (s_catch_kg > s_run_best_kg) { s_run_best_kg = s_catch_kg; s_run_best_sp = s_catch_sp; }
                s_total = well_total();                            // the three heaviest count
                if (s_catch_kg > s_stage_best) s_stage_best = s_catch_kg;
                s_new_rank = records_add(s_catch_sp, s_catch_kg, s_stage);
                if (s_new_rank == 0) sfx("record");                // a new top record: its own fanfare
                // Fisherman's Bait: every catch buys time, more for a heavier fish; a streak adds more.
                s_combo++;
                s_tb = 2 + iroundf(s_catch_kg * 2.5f);
                if (s_tb > 25) s_tb = 25;
                if (s_combo >= 2) s_tb += s_combo > 5 ? 5 : s_combo;
                s_time_ms += s_tb * 1000; s_tb_at = nv_millis(); s_bonus = s_tb;
                s_qual_now = !s_qualified && s_total >= s_quota;
                if (s_qual_now) s_qualified = 1;
                if (s_new_rank != 0) snd_fanfare();
                lake_view(0);
                fish_release_others(s_fight.fish);
                go(ST_CATCH, now);
            } else if (r < 0) {
                banner(r == -1 ? "LINE BREAK!" : T("SLAMATO!", "GOT AWAY!"), C_RED, now);
                rumble(r == -1 ? 60000 : 20000, r == -1 ? 20000 : 10000, r == -1 ? 350 : 150);
                s_combo = 0; s_shake = r == -1 ? 14 : 6;
                sfx(r == -1 ? "snap" : "splash");
                fish_release_others(-1);
                go(ST_LOST, now);
            }
            break;
        }
        case ST_CATCH: {
            // Trophy shot: the fish held up by the boat, turning in the light.
            const float t = (now - s_state_ms) / 1000.0f;
            (void)t;
            if (s_qual_now == 1 && now - s_state_ms > 1700) { s_qual_now = 2; sfx("qualify"); }
            if (now - s_state_ms > ((s_new_rank >= 0 && s_new_rank < 3) ? 2200 : 900) && confirm()) {
                if (s_time_ms <= 0) { fish_hide(); go(ST_WEIGH, now); } else to_aim(now);
            }
            break;
        }
        case ST_LOST:
            if (now - s_state_ms > 1500) {
                if (s_time_ms <= 0) { fish_hide(); lake_view(0); go(ST_WEIGH, now); } else to_aim(now);
            }
            break;
        case ST_WEIGH: {
            static int verdict_for = -1, drum_for = -1;
            if (drum_for != s_state_ms) { drum_for = s_state_ms; sfx("drum"); }
            if (now - s_state_ms > 1650 && verdict_for != s_state_ms) {
                verdict_for = s_state_ms;
                sfx(s_total >= s_quota ? "victory" : "fail");
            }
            s_orbit += dt * 0.15f;
            cam(sinf_(s_orbit) * 700, 220, cosf_(s_orbit) * 700 + 700, 0, 30, 900, 60);
            if (now - s_state_ms > 1800 && (s_screen_btn == 0 || pressed(NV_PAD_A | NV_PAD_START))) {
                s_screen_btn = -1;
                s_run_total += s_total;
                if (s_total >= s_quota && s_stage + 1 < NSTAGES && s_stage + 1 > s_unlocked) {
                    s_unlocked = s_stage + 1;
                    nv_save("unlock.bin", &s_unlocked, 4);
                }
                if (s_total >= s_quota) {
                    s_stages_cleared++;
                    if (++s_stage >= NSTAGES) { s_stage = 0; s_loop++; }
                    start_stage(now);
                } else {
                    if (iroundf(s_run_total * 100) > s_best_run100) {
                        s_best_run100 = iroundf(s_run_total * 100);
                        nv_save("bestrun.bin", &s_best_run100, 4);
                    }
                    go(ST_OVER, now);
                }
            }
            break;
        }
        case ST_OVER:
            s_orbit += dt * 0.12f;
            cam(sinf_(s_orbit) * 900, 260, cosf_(s_orbit) * 900 + 600, 0, 40, 900, 60);
            if (now - s_state_ms > 1200 && (s_screen_btn == 0 || pressed(NV_PAD_A | NV_PAD_START))) {   // retry this lake
                s_screen_btn = -1;
                s_loop = 0; s_run_total = 0; s_run_catches = 0; s_run_best_kg = 0; s_stages_cleared = 0;
                start_stage(now);
            } else if (now - s_state_ms > 1200 && (s_screen_btn == 1 || pressed(NV_PAD_B))) {
                s_screen_btn = -1; s_logo_at = now;
                s_stage = 0; s_loop = 0;
                lake_build(0, 0); fish_build(); build_lures(); lake_view(0);
                s_menu = 0; go(ST_TITLE, now);
            }
            break;
        }

        // Full-screen paintings hide the 3D frame: don't render it under them.
        if (!(s_state == ST_TITLE || s_state == ST_RECORDS || s_state == ST_STAGE || s_state == ST_WEIGH || s_state == ST_OVER || s_state == ST_LURE || s_state == ST_INTRO ||
              s_state == ST_SELECT || s_state == ST_CATCH))
            vx_render();

        // ---- 2D over the frame ----
        switch (s_state) {
        case ST_INTRO: draw_intro(now); break;
        case ST_TITLE: draw_title(now); break;
        case ST_RECORDS: draw_records(); break;
        case ST_SELECT: draw_select(now); break;
        case ST_STAGE: draw_stage_card(now); break;
        case ST_LURE: draw_lure_select(); break;
        case ST_AIM: {
            hud_top();
            // The landing ring on the water at the current power, and the power gauge.
            int clip;
            const float d = cast_reach(400 + s_power * 2350, &clip);
            int sx, sy;
            if (s_in.a && project(s_bx + sinf_(s_aim) * d, 0, s_bz + cosf_(s_aim) * d, &sx, &sy)) {
                const uint16_t rc = clip ? C565(255, 140, 30) : C_YELLOW;          // orange: it will hit the bank
                nv_gfx_circle(sx, sy, 10, C_SHADOW); nv_gfx_circle(sx, sy, 8, rc); nv_gfx_circle(sx, sy, 5, C_SHADOW);
                if (clip) text_sh(sx - 18, sy - 22, T("RIVA", "BANK"), rc, 1);
            }
            if (s_boil >= 0 && now - s_boil_at < 1400)             // rings where the fish rose
                water_rings(s_boil_x, s_boil_z, (now - s_boil_at) / 1000.0f, 150, 3);
            draw_rod(0, now);
            {   // the lure hanging from the tip on a short line
                const int lx = iroundf(s_rtx) + (s_charging && s_in.a ? 6 : 0), ly = iroundf(s_rty) + 22;
                nv_gfx_line(iroundf(s_rtx), iroundf(s_rty), lx, ly, C565(236, 236, 244));
                nv_gfx_circle(lx, ly + 3, 4, s_lure == LURE_CRANK ? C_RED : s_lure == LURE_POPPER ? C_YELLOW : s_lure == LURE_JIG ? C565(40, 60, 190) : C565(130, 60, 170));
            }
            panel(W / 2 - 104, 246, 208, 24);
            bar(W / 2 - 96, 254, 192, 8, s_power, s_power > 0.85f ? C_RED : C_YELLOW, 0);
            if (s_in.a || !pad_connected()) text_c(228, s_in.a ? T("RILASCIA PER LANCIARE", "RELEASE TO CAST") : T("A LANCIO  < > GIRA  SU/GIU' MOTORE", "A CAST  < > TURN  UP/DOWN MOTOR"), C_WHITE, 1);
            draw_sonar(now);
            if (s_engine == 1) text_c(150, T("AVVIO MOTORE...", "STARTING MOTOR..."), C_YELLOW, 2);
            s_icon_a = "b_cast"; s_icon_b = "b_lure";
            s_arrows_label = T("GIRA / MOTORE", "TURN / MOTOR");
            draw_controls(T("LANCIO", "CAST"), T("ESCA", "LURE"), 2);
            break;
        }
        case ST_CAST: {
            hud_top();
            float r3[3];
            rod_tip(&r3[0], &r3[1], &r3[2]);
            const int landed = s_cast_t >= s_cast_len;
            // In flight the line is taut behind the lure; once it lands it goes slack on the water.
            const float dist = sqrtf_((s_lx - r3[0]) * (s_lx - r3[0]) + (s_lz - r3[2]) * (s_lz - r3[2]));
            draw_line3d(r3, s_lx, s_ly, s_lz, landed ? 30 + dist * 0.03f : 14 + dist * 0.02f, landed);
            if (landed) water_rings(s_tx, s_tz, s_cast_t - s_cast_len, 130, 3);   // rings from the splash
            draw_rod(0, now);
            break;
        }
        case ST_RETRIEVE:
        case ST_STRIKE: {
            draw_rope();
            hud_top();
            // Depth gauge: surface at the top, lake bed at the bottom.
            panel(8, 70, 24, 130);
            nv_gfx_rect(12, 74, 16, 122, C565(20, 60, 90));
            const int ly = 74 + iroundf((1.0f - s_ly / SURF) * 118);
            nv_gfx_rect(10, ly, 20, 4, C_YELLOW);
            if (s_in.a && s_reel_amt < 0.99f) {                  // analog reel: how fast it turns
                nv_gfx_rect(10, 204, 20, 4, C565(30, 40, 60));
                nv_gfx_rect(10, 204, iroundf(20 * s_reel_amt), 4, C_GREEN);
            }
            const char *act = s_twitch_t > 0 ? T("STRAPPO", "TWITCH") : s_in.b ? T("MOLLO FILO", "GIVING LINE") : s_in.a ? T("RECUPERO", "REELING") : T("PAUSA", "PAUSE");
            text_sh(40, 74, act, C_CYAN, 1);
            char b[24], t[12];
            fmt_int(t, iroundf(sqrtf_((s_lx - s_bx) * (s_lx - s_bx) + (s_lz - s_bz) * (s_lz - s_bz)) / 100)); b[0] = 0; cat(b, t); cat(b, " M");
            text_sh(40, 88, b, C_WHITE, 1);
            for (int i = 0; i < fish_slots(); i++) {               // what the fish think of the lure
                float fx, fy, fz;
                const int mk = fish_mark(i, &fx, &fy, &fz);
                int sx, sy;
                if (!mk || !project(fx, fy, fz, &sx, &sy) || sy < 40 || sy > H - 20) continue;
                const char *t = mk == 2 ? "!" : "?";
                nv_gfx_circle(sx + 2, sy + 2, 9, C_SHADOW);
                nv_gfx_circle(sx, sy, 9, mk == 2 ? C_RED : C_YELLOW);
                nv_gfx_text(sx - 5, sy - 7, t, C_WHITE, 2);
            }
            if (s_nibble >= 0 && s_state == ST_RETRIEVE) {
                panel(W / 2 - 70, 116, 140, 26);
                text_c(122, T("TOCCA...", "NIBBLE..."), C_CYAN, 2);
            }
            if (s_state == ST_STRIKE) {
                const int big = ((now / 80) & 1);
                panel(W / 2 - 110, 110, 220, 44);
                text_c(118, T("FERRA ORA!", "SET THE HOOK!"), big ? C_YELLOW : C_RED, 3);
            }
            s_icon_a = "b_reel"; s_icon_b = 0;
            s_arrows_label = T("GUIDA / GIU' STRAPPO", "STEER / DOWN TWITCH");
            draw_controls(T("MULINELLO", "REEL"), T("MOLLA", "RELEASE"), 3);
            break;
        }
        case ST_FIGHT: {
            hud_top();
            int sx, sy;
            // The line from the rod (bottom of the screen) to the fish's mouth.
            const float d = sqrtf_((s_lx - s_bx) * (s_lx - s_bx) + (s_lz - s_bz) * (s_lz - s_bz)) + 1e-3f, ux = (s_lx - s_bx) / d, uz = (s_lz - s_bz) / d;
            const float fx = s_bx + ux * s_fight.dist + uz * s_fight.fx, fz = s_bz + uz * s_fight.dist - ux * s_fight.fx;
            if (project(fx, s_fight.fy, fz, &sx, &sy)) {       // taut line: straighter the harder it pulls
                const float mx = (s_rtx + sx) / 2, my = (s_rty + sy) / 2 + (1.0f - s_fight.tension) * 40;
                int lx = iroundf(s_rtx), ly = iroundf(s_rty);
                const int lcol = s_fight.tension > 0.85f && ((now / 60) & 1) ? C565(236, 90, 80) : C565(196, 210, 220);
                for (int i = 1; i <= 10; i++) {
                    const float t = i / 10.0f, u = 1 - t;
                    const int x = iroundf(u * u * s_rtx + 2 * u * t * mx + t * t * sx);
                    const int y = iroundf(u * u * s_rty + 2 * u * t * my + t * t * sy);
                    nv_gfx_line(lx, ly, x, y, lcol);
                    lx = x; ly = y;
                }
            }
            draw_rod(s_in.a, now);
            {   // a tall line-tension meter on the right, red at the top
                const int x = W - 34, y0 = 48, hh = 128;
                panel(x - 6, y0 - 6, 34, hh + 26);
                nv_gfx_rect(x, y0, 22, hh, C565(30, 36, 50));
                nv_gfx_rect(x, y0, 22, hh * 15 / 100, C565(110, 20, 20));
                const int f = iroundf(clampf(s_fight.tension, 0, 1) * hh);
                const int col = s_fight.tension > 0.85f ? (((now / 70) & 1) ? C_RED : C_WHITE) : s_fight.tension > 0.6f ? C_YELLOW : C_GREEN;
                nv_gfx_rect(x, y0 + hh - f, 22, f, col);
                if (s_fight.strain > 0.02f) {                     // strain: fills up beside the meter
                    const int sh = iroundf(s_fight.strain * hh);
                    nv_gfx_rect(x - 5, y0, 3, hh, C565(40, 20, 20));
                    nv_gfx_rect(x - 5, y0 + hh - sh, 3, sh, ((now / 90) & 1) ? C_RED : C_WHITE);
                }
                text_sh(x - 4, y0 + hh + 6, T("LENZA", "LINE"), C_GREY, 1);
            }
            panel(W / 2 - 80, 44, 160, 22);
            text_sh(W / 2 - 74, 51, T("PESCE", "FISH"), C_GREY, 1);
            bar(W / 2 - 30, 51, 104, 8, s_fight.stamina, C_CYAN, 0);
            if (s_fight.strain > 0.45f)
                text_c(96, T("STA PER ROMPERSI! MOLLA!", "ABOUT TO SNAP! LET GO!"), ((now / 90) & 1) ? C_RED : C_WHITE, 2);
            else if (s_fight.tension > 0.85f && s_in.a)
                text_c(96, T("ROSSO! SMETTI DI RECUPERARE", "RED! STOP REELING"), ((now / 120) & 1) ? C_RED : C_WHITE, 2);
            else if (s_fight.slack_t > 1.0f)
                text_c(96, T("LENZA MOLLE! RECUPERA", "SLACK LINE! REEL IN"), ((now / 120) & 1) ? C_YELLOW : C_WHITE, 2);
            else if (s_fight.drag) text_c(96, T("FRIZIONE: IL PESCE PRENDE FILO", "DRAG: THE FISH TAKES LINE"), C_YELLOW, 1);
            else if (s_fight.stamina < 0.15f) text_c(96, T("E' STANCO! RECUPERA!", "IT'S TIRED! REEL!"), C_GREEN, 2);
            else if (now - s_state_ms < 3500)
                text_c(96, T("A RECUPERA - B MOLLA QUANDO E' ROSSA", "A REEL - B RELEASE WHEN IT GOES RED"), C_WHITE, 1);
            char b[24], t[12];
            fmt_int(t, iroundf(s_fight.dist / 100)); b[0] = 0; cat(b, t); cat(b, " M");
            text_sh(8, 74, b, C_WHITE, 2);
            // Which way the fish is running: steer the rod the other way.
            if (s_fight.run_dir != 0 && s_fight.run > 0.35f) {
                const int ax = s_fight.run_dir > 0 ? W - 90 : 60, s = s_fight.run_dir > 0 ? 1 : -1;
                nv_gfx_tri(ax - s * 16, 130, ax - s * 16, 162, ax + s * 16, 146, C_RED);
                text_sh(ax - 30, 168, T("TIRA", "PULL"), C_RED, 1);
            }
            if (s_fight.jumping) {
                panel(W / 2 - 120, 120, 240, 44);
                text_c(128, T("SALTO! CANNA GIU'", "JUMP! ROD DOWN"), ((now / 90) & 1) ? C_YELLOW : C_WHITE, 3);
            }
            s_icon_a = "b_reel"; s_icon_b = 0;
            s_arrows_label = T("CANNA", "ROD");
            draw_controls(T("MULINELLO", "REEL"), T("MOLLA", "RELEASE"), 2);
            break;
        }
        case ST_CATCH:
            if (s_junk < 0 && s_new_rank >= 0 && s_new_rank < 3) draw_podium(now);
            else draw_catch(now);
            break;
        case ST_LOST: hud_top(); break;
        case ST_WEIGH: draw_weigh(now); break;
        case ST_OVER: draw_over(now); break;
        }
        if (in_play && s_go_at && now - s_go_at < 1600) {                 // READY? ... GO!
            const int g = now - s_go_at;
            static int go_beeped;
            if (g < 900) { text_c(112, T("PRONTI?", "READY?"), ((g / 150) & 1) ? C_YELLOW : C_WHITE, 5); go_beeped = 0; }
            else {
                if (!go_beeped) { go_beeped = 1; nv_gfx_tone(1320, 220); }
                const int sc = g < 1050 ? 10 - (g - 900) / 40 : 7;
                text_c(122 - sc * 3, "GO!", C_GREEN, sc);
            }
            if (g < 60) nv_gfx_tone(880, 120);
        }
        if (in_play && s_time_ms > 0 && s_time_ms < 10000) {            // the final ten, huge
            char n2[4];
            fmt_int(n2, s_time_ms / 1000 + 1);
            const int f = s_time_ms % 1000, sc = f > 850 ? 12 : 9;
            text_c(150, n2, (s_time_ms / 1000) < 3 ? C_RED : C_YELLOW, sc);
        }
        if (s_ban && now - s_ban_at < 1100 && s_state != ST_CATCH) {   // the arcade banner
            const int t = now - s_ban_at, sc = t < 150 ? 8 - t * 3 / 150 : 5;
            text_c(128 - sc * 4, s_ban, ((t / 90) & 1) ? s_ban_col : C_WHITE, sc);
        }
        if (now < s_flash_until) nv_gfx_rect(0, 0, W, H, C_WHITE);
        hud_msg(now);
        if (in_play && !pad_connected()) {
            nv_gfx_rect(kPause.x + 2, kPause.y + 2, kPause.w, kPause.h, C_SHADOW);
            nv_gfx_rect(kPause.x, kPause.y, kPause.w, kPause.h, C565(22, 44, 78));
            nv_gfx_rect(kPause.x + 9, kPause.y + 7, 4, 14, C_WHITE); nv_gfx_rect(kPause.x + 17, kPause.y + 7, 4, 14, C_WHITE);
        }
    }
}
