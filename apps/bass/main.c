// Vertice Bass — arcade lake fishing for NucleoOS on the Vertice 3D engine.
// A tournament of six lakes: each stage has a clock and a weight quota. Pick a lure, aim and cast
// from the boat, work the lure under water (reel / pause / twitch) until a fish strikes, set the
// hook, then fight it — rod against its runs, reel without snapping the line, catch the jumps.
// Weigh-in at the bell: make the quota to move on. The ten biggest fish ever go on the record
// wall. Touch, USB keyboard or gamepad; Italian or English from the system language.
#include "bass.h"

enum { ST_TITLE, ST_RECORDS, ST_STAGE, ST_LURE, ST_AIM, ST_CAST, ST_RETRIEVE, ST_STRIKE, ST_FIGHT,
       ST_CATCH, ST_LOST, ST_WEIGH, ST_OVER };

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
static void fish_art(int sp, int x, int y, int w, int h) {
    char n[8] = "fish0";
    n[4] = (char)('0' + sp);
    nv_gfx_image(n, x, y, w, h);
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
typedef struct { int left, right, up, down, a, b, a_hit, b_hit, tap, tx, ty; } Pad;
static Pad s_in;
typedef struct { int x, y, w, h; } Rect;
static const Rect kLeft = { 6, 214, 62, 80 }, kRight = { 74, 214, 62, 80 };
static const Rect kB = { 362, 222, 64, 72 }, kA = { 432, 204, 74, 90 };
static int in_rect(const Rect *r, int x, int y) { return x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h; }
static int pad_connected(void) { return (s_pad & (NV_PAD_KEYBOARD | NV_PAD_GAMEPAD)) != 0; }

static void read_input(void) {
    static int prev_a, prev_b;
    Pad p = { 0 };
    const int n = nv_touch_count();
    for (int i = 0; i < n; i++) {
        int x, y;
        if (!nv_touch_at(i, &x, &y)) continue;
        if (y > 190 && x < 145) { if (x < 71) p.left = 1; else p.right = 1; }
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
    p.a_hit = p.a && !prev_a; p.b_hit = p.b && !prev_b;
    prev_a = p.a; prev_b = p.b;
    if (!p.tx && !p.ty) { p.tx = s_in.tx; p.ty = s_in.ty; }
    s_in = p;
}
static int pressed(int bit) { return (s_pad & bit) && !(s_prev_pad & bit); }
// "Confirm": A on a pad, or a tap anywhere not on the on-screen buttons.
static int confirm(void) {
    return pressed(NV_PAD_A | NV_PAD_START) || (s_in.tap && !in_rect(&kB, s_in.tx, s_in.ty));
}

static void button(const Rect *r, int on, const char *label, int col, const char *icon) {
    const int cx = r->x + r->w / 2, cy = r->y + r->h / 2, rad = (r->w < r->h ? r->w : r->h) / 2 - 2;
    nv_gfx_circle(cx + 2, cy + 2, rad, C_SHADOW);
    nv_gfx_circle(cx, cy, rad, on ? col : C565(40, 50, 70));
    nv_gfx_circle(cx, cy, rad - 3, on ? C565(90, 110, 150) : C565(20, 28, 44));
    if (icon) {
        const int s = on ? 40 : 36;
        nv_gfx_image(icon, cx - s / 2, cy - s / 2 - 6, s, s);
        text_sh(cx - nv_gfx_text_width(label, 1) / 2, cy + rad - 14, label, on ? C_YELLOW : C_GREY, 1);
    } else {
        text_sh(cx - nv_gfx_text_width(label, 1) / 2, cy - 3, label, on ? C_WHITE : C_GREY, 1);
    }
}
static const char *s_icon_a, *s_icon_b;   // painted icons for the next draw_controls
static void draw_controls(const char *a_label, const char *b_label, int arrows) {
    if (pad_connected()) return;
    if (arrows) {
        const Rect *rs[2] = { &kLeft, &kRight };
        for (int i = 0; i < 2; i++) {
            const Rect *r = rs[i];
            const int on = i ? s_in.right : s_in.left, cx = r->x + r->w / 2, cy = r->y + r->h / 2;
            nv_gfx_rect(r->x, r->y, r->w, r->h, on ? C565(60, 90, 130) : C565(18, 26, 42));
            if (i == 0) nv_gfx_tri(cx + 12, cy - 16, cx + 12, cy + 16, cx - 14, cy, on ? C_YELLOW : C_GREY);
            else nv_gfx_tri(cx - 12, cy - 16, cx - 12, cy + 16, cx + 14, cy, on ? C_YELLOW : C_GREY);
        }
    }
    if (a_label) button(&kA, s_in.a, a_label, C_GREEN, s_icon_a);
    if (b_label) button(&kB, s_in.b, b_label, C_CYAN, s_icon_b);
    s_icon_a = s_icon_b = 0;
}

// ---- camera + projection (to draw the fishing line and markers over the 3D frame) --------------------------
static float s_cp[3], s_ct[3], s_fov = 62;
static void cam(float px, float py, float pz, float tx, float ty, float tz, float fov) {
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
        } else {                       // purple soft worm: a wavy segmented tube, hook through the head
            const int m0 = vx_material(C565(120, 50, 170), VX_GOURAUD, 255, -1, 160);
            const int m1 = vx_material(C565(160, 90, 210), VX_GOURAUD, 255, -1, 160);
            enum { NR = 11, NS = 6 };
            int ring[NR][NS];
            for (int r = 0; r < NR; r++) {
                const float z = 30 - r * 6.5f, ox = sinf_(r * 0.9f) * 5, rad = r == 0 ? 2.5f : r > 8 ? 3.2f - (r - 8) * 0.9f : 3.4f;
                for (int k = 0; k < NS; k++) {
                    const float a = k * 2 * PI_F / NS;
                    ring[r][k] = mb_v(ox + cosf_(a) * rad, sinf_(a) * rad, z, 0, 0);
                }
            }
            for (int r = 0; r < NR - 1; r++)
                for (int k = 0; k < NS; k++)
                    mb_quad(ring[r][k], ring[r][(k + 1) % NS], ring[r + 1][(k + 1) % NS], ring[r + 1][k], (r & 1) ? m1 : m0,
                            sinf_(r * 0.9f) * 5, 0, 30 - r * 6.5f - 3);
            const int h0 = mb_v(-0.6f, 3, 30, 0, 0), h1 = mb_v(0.6f, 3, 30, 0, 0), h2 = mb_v(0, 10, 18, 0, 0);
            mb_tri(h0, h1, h2, steel, 0, 6, 26); mb_tri(h0, h1, h2, steel, 0, 6, 34);
            const int g0 = mb_v(0, 10, 18, 0, 0), g1 = mb_v(0, 4, 12, 0, 0), g2 = mb_v(0.6f, 10, 16, 0, 0);
            mb_tri(g0, g1, g2, steel, 5, 8, 15); mb_tri(g0, g1, g2, steel, -5, 8, 15);
        }
        s_lure_obj[k] = mb_commit(steel, 0);
        vx_obj_scale(s_lure_obj[k], 150);
        vx_obj_show(s_lure_obj[k], 0);
    }
}
static void lure_pose(float x, float y, float z, float yaw) {
    for (int k = 0; k < NLURES; k++) vx_obj_show(s_lure_obj[k], k == s_lure);
    vx_obj_pos(s_lure_obj[s_lure], iroundf(x), iroundf(y), iroundf(z));
    vx_obj_rot(s_lure_obj[s_lure], 0, iroundf(deg(yaw)), 0);
}
static void lure_hide(void) { for (int k = 0; k < NLURES; k++) vx_obj_show(s_lure_obj[k], 0); }

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

static void go(int st, int now) {
    static const char *const names[] = { "title", "records", "stage", "lure", "aim", "cast", "retrieve",
                                         "strike", "fight", "catch", "lost", "weigh", "over" };
    char b[40] = "bass: ";
    cat(b, names[st]);
    nv_log(NV_LOG_INFO, b);
    s_state = st; s_state_ms = now;
}

static void start_stage(int now) {
    lake_build(s_stage, s_loop);
    fish_build();
    build_lures();
    lake_view(0);
    s_quota = g_stage[s_stage].quota_kg * (1.0f + 0.35f * s_loop);
    s_time_ms = g_stage[s_stage].time_s * 1000;
    s_total = 0; s_catches = 0; s_stage_best = 0;
    s_aim = 0;
    go(ST_STAGE, now);
}
static void to_aim(int now) {
    fish_hide();
    lake_view(0);
    lure_hide();
    s_power = 0; s_power_t = 0; s_charging = 0;
    go(ST_AIM, now);
}

static void aim_camera(void) {
    const float fx = sinf_(s_aim), fz = cosf_(s_aim);
    cam(-fx * 230, 128, -fz * 230, fx * 900, 0, fz * 900, 62);
    vx_obj_rot(g_boat, 0, iroundf(deg(s_aim)), 0);
}
static void rod_tip(float *x, float *y, float *z) { *x = sinf_(s_aim) * 40 + cosf_(s_aim) * 30; *y = 150; *z = cosf_(s_aim) * 40 - sinf_(s_aim) * 30; }

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
    fmt_clock(t, s_time_ms);
    panel(W / 2 - 58, 4, 116, 32);
    nv_gfx_image("i_clock", W / 2 - 52, 9, 22, 22);
    text_sh(W / 2 - 22, 9, t, s_time_ms < 20000 && ((s_time_ms / 250) & 1) ? C_RED : C_YELLOW, 3);
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


// ---- screens ------------------------------------------------------------------------------------------------------------
static void draw_title(int now) {
    art("title");
    text_c(34, "VERTICE", C_SHADOW, 5);
    text_sh((W - nv_gfx_text_width("VERTICE", 5)) / 2 - 2, 32, "VERTICE", C_WHITE, 5);
    text_sh((W - nv_gfx_text_width("BASS", 7)) / 2, 70, "BASS", C_YELLOW, 7);
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
    if ((now / 500) & 1) text_c(280, pad_connected() ? T("A = SCEGLI", "A = SELECT") : T("TOCCA UNA VOCE", "TAP AN ITEM"), C_WHITE, 1);
}

static void draw_records(void) {
    art("weigh");
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
        fish_art(r->species, 78, y - 5, 27, 18);
        text_sh(110, y, sp_name(r->species), i == s_new_rank ? C_YELLOW : C_WHITE, 1);
        text_sh(250, y, lake_name(r->stage), C_CYAN, 1);
        fmt_kg(t, r->kg100 / 100.0f);
        text_sh(W - 60 - nv_gfx_text_width(t, 1), y, t, i == 0 ? C_YELLOW : C_WHITE, 1);
    }
    text_c(H - 34, T("TOCCA PER TORNARE", "TAP TO GO BACK"), C_GREY, 1);
}

static void draw_stage_card(int now) {
    char b[48], t[24];
    lake_art(s_stage);
    panel(40, 8, W - 80, 58);
    b[0] = 0; cat(b, T("TAPPA ", "STAGE ")); fmt_int(t, s_stage + 1 + s_loop * NSTAGES); cat(b, t);
    text_c(14, b, C_CYAN, 1);
    text_c(28, lake_name(s_stage), C_WHITE, 3);
    panel(40, 208, W - 80, 84);
    b[0] = 0; cat(b, T("QUOTA ", "QUOTA ")); fmt_kg(t, s_quota); cat(b, t);
    cat(b, "   "); cat(b, T("TEMPO ", "TIME ")); fmt_clock(t, s_time_ms); cat(b, t);
    text_c(218, b, C_YELLOW, 2);
    text_c(244, T("PESCA ABBASTANZA PESO PRIMA DEL GONG", "LAND ENOUGH WEIGHT BEFORE THE BELL"), C_WHITE, 1);
    if ((now / 500) & 1) text_c(266, T("TOCCA PER SCEGLIERE L'ESCA", "TAP TO CHOOSE A LURE"), C_CYAN, 1);
}

static void draw_lure_icon(int k, int cx, int cy, int sel) {
    char n[8] = "lure0";
    n[4] = (char)('0' + k);
    nv_gfx_image(n, cx - 44, cy - 40, 88, 88);
    if (sel) nv_gfx_rect(cx - 50, cy + 48, 100, 3, C_YELLOW);
}
static void draw_lure_select(void) {
    static const char *const d_it[NLURES] = { "MEZZ'ACQUA  RECUPERO", "GALLA  STRAPPI", "FONDO  PAUSE" };
    static const char *const d_en[NLURES] = { "MID WATER  STEADY", "SURFACE  TWITCH", "BOTTOM  STOP-GO" };
    text_c(26, T("SCEGLI L'ESCA", "CHOOSE YOUR LURE"), C_YELLOW, 3);
    for (int k = 0; k < NLURES; k++) {
        const int x = 22 + k * 160, sel = s_lure == k;
        panel(x, 70, 148, 150);
        if (sel) nv_gfx_rect(x + 4, 74, 140, 142, C565(30, 64, 110));
        draw_lure_icon(k, x + 74, 110, sel);
        text_sh(x + 74 - nv_gfx_text_width(T(g_lure_it[k], g_lure_en[k]), 2) / 2, 168, T(g_lure_it[k], g_lure_en[k]), sel ? C_YELLOW : C_WHITE, 2);
        text_sh(x + 74 - nv_gfx_text_width(T(d_it[k], d_en[k]), 1) / 2, 194, T(d_it[k], d_en[k]), C_CYAN, 1);
    }
    text_c(240, pad_connected() ? T("< > SCEGLI   A CONFERMA", "< > CHOOSE   A CONFIRM") : T("TOCCA UN'ESCA", "TAP A LURE"), C_WHITE, 1);
}

static void draw_weigh(int now) {
    char b[48], t[24];
    const float shown = s_total * clampf((now - s_state_ms) / 1600.0f, 0, 1);
    art("weigh");
    panel(66, 30, W - 132, 240);
    text_c(42, T("PESATURA", "WEIGH-IN"), C_YELLOW, 3);
    text_c(74, lake_name(s_stage), C_CYAN, 2);
    b[0] = 0; cat(b, T("PESCI ", "FISH ")); fmt_int(t, s_catches); cat(b, t);
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
        text_sh(x, y, b, i == 0 ? C_YELLOW : C_WHITE, 1);
    }
    if (now - s_state_ms > 1700) {
        const int ok = s_total >= s_quota;
        text_c(206, ok ? T("QUALIFICATO!", "QUALIFIED!") : T("NON QUALIFICATO", "NOT QUALIFIED"), ok ? C_GREEN : C_RED, 3);
        if ((now / 500) & 1) text_c(230, T("TOCCA PER CONTINUARE", "TAP TO CONTINUE"), C_WHITE, 1);
    }
}

static void draw_over(int now) {
    char b[48], t[24];
    art("title");
    panel(66, 40, W - 132, 190);
    text_c(56, T("FINE TORNEO", "TOURNAMENT OVER"), C_YELLOW, 3);
    b[0] = 0; cat(b, T("TAPPE SUPERATE ", "STAGES CLEARED ")); fmt_int(t, s_stage + s_loop * NSTAGES); cat(b, t);
    text_c(100, b, C_WHITE, 2);
    fmt_kg(t, s_run_total);
    text_c(130, t, C_WHITE, 4);
    if (iroundf(s_run_total * 100) >= s_best_run100 && s_run_total > 0) {
        text_c(172, T("NUOVO RECORD DI TORNEO!", "NEW TOURNAMENT RECORD!"), C_GREEN, 2);
        nv_gfx_image("a_trophy", 76, 150, 56, 56);
        nv_gfx_image("a_trophy", W - 132, 150, 56, 56);
    }
    if ((now / 500) & 1) text_c(206, T("TOCCA PER IL MENU", "TAP FOR THE MENU"), C_WHITE, 1);
}

// ---- the game loop -----------------------------------------------------------------------------------------------------------
NV_EXPORT("run")
void run(void) {
    char lang[8] = "";
    nv_lang(lang, sizeof lang);
    s_it = lang[0] == 'i' && lang[1] == 't';
    records_load();
    s_stage = 0; s_loop = 0;
    lake_build(0, 0); fish_build(); build_lures(); lake_view(0);
    int last = nv_millis();
    s_state_ms = last;
    sfx("title");
    while (nv_gfx_present()) {
        const int now = nv_millis();
        float dt = (now - last) / 1000.0f;
        last = now;
        if (dt > 0.05f) dt = 0.05f;
        read_input();
        if (nv_gfx_back() || pressed(NV_PAD_SELECT)) {
            if (s_state == ST_TITLE) return;
            if (s_state == ST_RECORDS) { go(ST_TITLE, now); }
            else { lake_build(0, 0); fish_build(); build_lures(); lake_view(0); s_menu = 0; go(ST_TITLE, now); }
        }
        const int ticking = s_state == ST_AIM || s_state == ST_CAST || s_state == ST_RETRIEVE || s_state == ST_STRIKE || s_state == ST_FIGHT;
        if (ticking) s_time_ms -= (int)(dt * 1000);
        const int time_up = s_time_ms <= 0 && s_state != ST_FIGHT && ticking;
        if (time_up) {
            s_time_ms = 0; fish_hide(); lure_hide(); lake_view(0);
            sfx("bell");
            go(ST_WEIGH, now);
        }

        switch (s_state) {
        case ST_TITLE: {
            s_orbit += dt * 0.12f;
            cam(sinf_(s_orbit) * 900, 260, cosf_(s_orbit) * 900 + 600, 0, 40, 900, 60);
            if (s_in.up || pressed(NV_PAD_UP)) s_menu = 0;
            if (s_in.down || pressed(NV_PAD_DOWN)) s_menu = 1;
            int pick = -1;
            if (s_in.tap) for (int i = 0; i < 2; i++) if (s_in.tx > W / 2 && s_in.ty >= 160 + i * 34 && s_in.ty < 188 + i * 34) pick = i;
            if (pressed(NV_PAD_A | NV_PAD_START)) pick = s_menu;
            if (pick == 0) { s_stage = 0; s_loop = 0; s_run_total = 0; s_new_rank = -1; snd_click(); start_stage(now); }
            if (pick == 1) { s_new_rank = -1; snd_click(); go(ST_RECORDS, now); }
            break;
        }
        case ST_RECORDS:
            s_orbit += dt * 0.12f;
            cam(sinf_(s_orbit) * 900, 260, cosf_(s_orbit) * 900 + 600, 0, 40, 900, 60);
            if (confirm() || s_in.b_hit) go(ST_TITLE, now);
            break;
        case ST_STAGE:
            s_orbit += dt * 0.15f;
            cam(sinf_(s_orbit) * 700, 220, cosf_(s_orbit) * 700 + 700, 0, 30, 900, 60);
            if (now - s_state_ms > 400 && confirm()) { snd_click(); go(ST_LURE, now); }
            break;
        case ST_LURE:
            aim_camera();
            if (pressed(NV_PAD_LEFT)) { s_lure = (s_lure + NLURES - 1) % NLURES; snd_click(); }
            if (pressed(NV_PAD_RIGHT)) { s_lure = (s_lure + 1) % NLURES; snd_click(); }
            if (s_in.tap && s_in.ty > 70 && s_in.ty < 220) {
                s_lure = (int)clampf((s_in.tx - 22) / 160.0f, 0, NLURES - 1); snd_click(); to_aim(now);
            } else if (pressed(NV_PAD_A | NV_PAD_START) && now - s_state_ms > 250) { snd_click(); to_aim(now); }
            break;
        case ST_AIM: {
            if (s_in.left) s_aim -= dt * 0.9f;
            if (s_in.right) s_aim += dt * 0.9f;
            s_aim = clampf(s_aim, -1.1f, 1.1f);
            if (s_in.b_hit) { s_lure = (s_lure + 1) % NLURES; snd_click(); }
            aim_camera();
            if (s_charging && s_in.a) rod_seek(W - 40 - 20 * s_power, 26 + 10 * s_power, 26, 14, dt);   // wound back
            else rod_seek(W - 170, 96, 0, 8, dt);
            if (now - s_boil_at > 2600 + rnd(2200)) {           // fish break the surface now and then
                s_boil = rnd(NSPOTS); s_boil_at = now;
                s_boil_x = g_spot[s_boil].x + rnd(160) - 80; s_boil_z = g_spot[s_boil].z + rnd(160) - 80;
                vx_emit(g_fx_splash, iroundf(s_boil_x), 4, iroundf(s_boil_z), 0, 220, 0, 110, 10);
            }
            if (s_in.a_hit) s_charging = 1;                   // a fresh press starts the charge
            if (s_in.a && s_charging) {                       // charge: the power swings up and down
                s_power_t += dt;
                const float ph = s_power_t / 1.3f;
                s_power = 1.0f - fabsf_((ph - (int)ph) * 2 - 1);
            } else if (s_power_t > 0 && s_charging) {         // release: cast
                s_charging = 0;
                const float d = 350 + s_power * 1450;
                s_tx = sinf_(s_aim) * d; s_tz = cosf_(s_aim) * d;
                s_cast_len = 0.35f + d / 2000; s_cast_t = 0;
                s_power_t = 0; s_release_t = 0;
                sfx("cast");
                go(ST_CAST, now);
            }
            break;
        }
        case ST_CAST: {
            aim_camera();
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
                    msg(spot >= 0 ? T("BUON POSTO!", "NICE SPOT!") : T("ACQUA APERTA", "OPEN WATER"), now, 900);
                }
                if (s_cast_t > s_cast_len + 0.5f) {           // dive under
                    lake_view(1);
                    fish_spawn(s_tx, s_tz, s_stage);
                    s_lx = s_tx; s_lz = s_tz; s_ly = SURF - 8; s_twitch_t = 0; s_twitches = 0;
                    vx_emit(g_fx_bubble, iroundf(s_lx), iroundf(s_ly), iroundf(s_lz), 0, 120, 0, 60, 16);
                    {   // this frame already renders under water: put the camera there too
                        const float dd = sqrtf_(s_lx * s_lx + s_lz * s_lz) + 1e-3f, ux = s_lx / dd, uz = s_lz / dd;
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
            // Lure physics: reel (A held), pause, twitch (B).
            const int reel = s_in.a && s_state == ST_RETRIEVE;
            if (s_in.b_hit && s_state == ST_RETRIEVE) {
                s_twitch_t = 0.35f; s_twitches++;
                vx_emit(g_fx_bubble, iroundf(s_lx), iroundf(s_ly), iroundf(s_lz), 0, 80, 0, 40, s_lure == LURE_POPPER ? 10 : 4);
                sfx(s_lure == LURE_POPPER ? "plop" : "click");
            }
            s_twitch_t -= dt;
            float speed = reel ? (s_lure == LURE_WORM ? 115.0f : s_lure == LURE_POPPER ? 160.0f : 200.0f) : 0;
            if (s_twitch_t > 0.2f) speed += 420;
            float depth_target;
            if (s_lure == LURE_POPPER) depth_target = SURF - 8;
            else if (s_lure == LURE_CRANK) depth_target = reel ? SURF - 240 : SURF - 8;
            else depth_target = reel ? 90 : 22;
            const float dv = (s_lure == LURE_WORM && !reel) ? 90.0f : 110.0f;
            s_ly += clampf(depth_target - s_ly, -dv * dt, dv * dt);
            const float d = sqrtf_(s_lx * s_lx + s_lz * s_lz) + 1e-3f;
            const float ux = s_lx / d, uz = s_lz / d;
            if (s_state == ST_RETRIEVE) { s_lx -= ux * speed * dt; s_lz -= uz * speed * dt; }
            const float yaw = atan2f_(-ux, -uz);
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
            // chasing it) toward you.
            {
                const float back = d > 330 ? 260.0f : d - 70.0f;    // never behind the boat
                cam(s_lx - ux * back, clampf(s_ly + 110, 80, SURF - 10), s_lz - uz * back, s_lx + ux * 60, s_ly - 30, s_lz + uz * 60, 66);
            }
            if ((now / 120) % 4 == 0)                             // drifting specks in the water
                vx_emit(g_fx_dust, iroundf(s_lx + rnd(500) - 250), iroundf(s_ly + rnd(200) - 100), iroundf(s_lz + rnd(500) - 250), 0, 10, 0, 20, 1);
            LureState ls = { s_twitch_t > 0 ? 2 : (reel ? 0 : 1), s_lx, s_ly, s_lz, s_lure };
            const int nib = s_state == ST_RETRIEVE ? fish_nibbling() : -1;
            if (nib >= 0) {                                        // a fish mouthing the lure
                static int tick_at;
                if (now - tick_at > 260) { tick_at = now; sfx("click"); }
                lure_pose(s_lx, s_ly + sinf_(now * 0.06f) * 4, s_lz, yaw + sinf_(now * 0.05f) * 0.2f);
                if (s_in.b_hit || s_in.a_hit) {                    // struck at a nibble: too early
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
                    snd_strike();
                    vx_emit(g_fx_bubble, iroundf(s_lx), iroundf(s_ly), iroundf(s_lz), 0, 160, 0, 80, 20);
                    go(ST_STRIKE, now);
                } else if (d < 130) {
                    msg(T("RECUPERATA", "REELED IN"), now, 700);
                    to_aim(now);
                }
            } else {                                               // strike window: set the hook!
                fish_pose(s_strike_fish, s_lx + ux * 34, s_ly, s_lz + uz * 34, yaw, sinf_(now * 0.05f) * 0.25f, 0);
                if (s_in.b_hit || s_in.a_hit) {
                    fish_release_others(s_strike_fish);
                    fight_start(&s_fight, s_strike_fish, s_lx, s_ly, s_lz);
                    msg(fish_kg(s_strike_fish) >= 4.0f ? T("PESCE GROSSO!", "BIG ONE!") : T("FERRATO!", "HOOKED!"), now, 1100);
                    s_fyaw = atan2f_(ux, uz); s_fpx = s_lx; s_fpz = s_lz;
                    s_ccp[0] = s_lx - ux * 300; s_ccp[1] = s_ly + 80; s_ccp[2] = s_lz - uz * 300;
                    s_cct[0] = s_lx; s_cct[1] = s_ly; s_cct[2] = s_lz;
                    s_rtx = W / 2 + 40; s_rty = 150;
                    sfx("hook");
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
            const int r = fight_update(&s_fight, rod, s_in.a, s_in.b_hit, dt);
            // Where the fish is: along the line from the boat, pulled sideways by its runs.
            const float d = sqrtf_(s_lx * s_lx + s_lz * s_lz) + 1e-3f, ux = s_lx / d, uz = s_lz / d;
            const float fx = ux * s_fight.dist + uz * s_fight.fx, fz = uz * s_fight.dist - ux * s_fight.fx;
            // Heading: a running fish points where it swims; one being dragged in (tired, or reeled
            // while it isn't running) comes mouth-first toward the boat. Turned at a fish's pace.
            {
                const float vx = (fx - s_fpx) / (dt > 1e-3f ? dt : 1e-3f), vz = (fz - s_fpz) / (dt > 1e-3f ? dt : 1e-3f);
                s_fpx = fx; s_fpz = fz;
                const float sp = sqrtf_(vx * vx + vz * vz);
                float want;
                if (s_fight.stamina < 0.15f || (s_in.a && s_fight.run < 0.45f)) want = atan2f_(-fx, -fz);
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
            }
            {   // the rod bends toward the fish, more under tension; dips on a jump
                int sx = W / 2, sy = H / 2;
                project(fx, s_fight.fy, fz, &sx, &sy);
                const float tx = W - 170 + (sx - W / 2) * 0.35f, ty = 70 + s_fight.tension * 60 + (s_fight.jumping ? 50 : 0);
                rod_seek(tx, ty, 18 + s_fight.tension * 70, 6, dt);
            }
            if ((now / 160) % 3 == 0) vx_emit(g_fx_bubble, iroundf(fx), iroundf(s_fight.fy + 20), iroundf(fz), 0, 100, 0, 30, 2);
            if (s_fight.jumping && (now / 100) % 2 == 0) vx_emit(g_fx_bubble, iroundf(fx), iroundf(s_fight.fy), iroundf(fz), 0, 260, 0, 90, 6);
            if (s_in.a) sfx_reel(now);
            {   // a jump starting: the splash
                static int was_jumping;
                if (s_fight.jumping && !was_jumping) sfx("jump");
                was_jumping = s_fight.jumping;
            }
            if (s_fight.tension > 0.85f && (now / 240) % 2 == 0 && now - s_reel_at > 150) nv_gfx_tone(2400, 25);
            s_lx = ux * (s_fight.dist + 1); s_lz = uz * (s_fight.dist + 1);   // keep the line direction
            if (r == 1) {
                s_catch_sp = fish_species(s_fight.fish); s_catch_kg = fish_kg(s_fight.fish);
                s_total += s_catch_kg; s_run_total += s_catch_kg;
                if (s_catches < 16) { s_list_sp[s_catches] = (uint8_t)s_catch_sp; s_list_kg[s_catches] = s_catch_kg; }
                s_catches++;
                if (s_catch_kg > s_stage_best) s_stage_best = s_catch_kg;
                s_new_rank = records_add(s_catch_sp, s_catch_kg, s_stage);
                if (s_catch_kg >= 2.5f) { s_time_ms += 10000; s_bonus = 1; } else s_bonus = 0;   // arcade: big fish, more time
                snd_fanfare();
                lake_view(0);
                fish_release_others(s_fight.fish);
                go(ST_CATCH, now);
            } else if (r < 0) {
                msg(r == -1 ? T("LENZA SPEZZATA!", "LINE SNAPPED!") : T("SI E' SLAMATO!", "IT GOT AWAY!"), now, 1500);
                sfx(r == -1 ? "snap" : "splash");
                fish_release_others(-1);
                go(ST_LOST, now);
            }
            break;
        }
        case ST_CATCH: {
            // Trophy shot: the fish held up by the boat, turning in the light.
            const float t = (now - s_state_ms) / 1000.0f;
            fish_pose(s_fight.fish, 0, 160, 120, t * 1.3f, sinf_(t * 9) * 0.12f, sinf_(t * 2) * 0.15f);
            cam(0, 175, -110, 0, 150, 120, 50);
            if (now - s_state_ms > 900 && confirm()) {
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
                sfx(s_total >= s_quota ? "qualify" : "fail");
            }
            s_orbit += dt * 0.15f;
            cam(sinf_(s_orbit) * 700, 220, cosf_(s_orbit) * 700 + 700, 0, 30, 900, 60);
            if (now - s_state_ms > 1800 && confirm()) {
                if (s_total >= s_quota) {
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
            if (now - s_state_ms > 1200 && confirm()) {
                s_stage = 0; s_loop = 0;
                lake_build(0, 0); fish_build(); build_lures(); lake_view(0);
                s_menu = 0; go(ST_TITLE, now);
            }
            break;
        }

        // Full-screen paintings hide the 3D frame: don't render it under them.
        if (!(s_state == ST_TITLE || s_state == ST_RECORDS || s_state == ST_STAGE || s_state == ST_WEIGH || s_state == ST_OVER))
            vx_render();

        // ---- 2D over the frame ----
        switch (s_state) {
        case ST_TITLE: draw_title(now); break;
        case ST_RECORDS: draw_records(); break;
        case ST_STAGE: draw_stage_card(now); break;
        case ST_LURE: draw_lure_select(); break;
        case ST_AIM: {
            hud_top();
            // The landing ring on the water at the current power, and the power gauge.
            const float d = 350 + s_power * 1450;
            int sx, sy;
            if (s_in.a && project(sinf_(s_aim) * d, 0, cosf_(s_aim) * d, &sx, &sy)) {
                nv_gfx_circle(sx, sy, 10, C_SHADOW); nv_gfx_circle(sx, sy, 8, C_YELLOW); nv_gfx_circle(sx, sy, 5, C_SHADOW);
            }
            if (s_boil >= 0 && now - s_boil_at < 1400) {           // rings where the fish rose
                int sx, sy;
                if (project(s_boil_x, 0, s_boil_z, &sx, &sy)) {
                    const float age = (now - s_boil_at) / 1000.0f;
                    const float dist = sqrtf_(s_boil_x * s_boil_x + s_boil_z * s_boil_z);
                    for (int k = 0; k < 2; k++) {
                        const int r = 3 + iroundf((age * 50 + k * 12) * 260.0f / (200 + dist * 0.25f));
                        nv_gfx_line(sx - r, sy, sx, sy - r / 4, C565(230, 244, 255)); nv_gfx_line(sx, sy - r / 4, sx + r, sy, C565(230, 244, 255));
                        nv_gfx_line(sx - r, sy, sx, sy + r / 4, C565(190, 214, 240)); nv_gfx_line(sx, sy + r / 4, sx + r, sy, C565(190, 214, 240));
                    }
                }
            }
            draw_rod(0, now);
            {   // the lure hanging from the tip on a short line
                const int lx = iroundf(s_rtx) + (s_charging && s_in.a ? 6 : 0), ly = iroundf(s_rty) + 22;
                nv_gfx_line(iroundf(s_rtx), iroundf(s_rty), lx, ly, C565(236, 236, 244));
                nv_gfx_circle(lx, ly + 3, 4, s_lure == LURE_CRANK ? C_RED : s_lure == LURE_POPPER ? C_YELLOW : C565(130, 60, 170));
            }
            panel(W / 2 - 104, 246, 208, 24);
            bar(W / 2 - 96, 254, 192, 8, s_power, s_power > 0.85f ? C_RED : C_YELLOW, 0);
            text_c(228, s_in.a ? T("RILASCIA PER LANCIARE", "RELEASE TO CAST") : T("TIENI A PER CARICARE  < > MIRA  B ESCA", "HOLD A TO CHARGE  < > AIM  B LURE"), C_WHITE, 1);
            s_icon_a = "b_cast"; s_icon_b = "b_lure";
            draw_controls(T("LANCIO", "CAST"), T("ESCA", "LURE"), 1);
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
            if (landed) {                                          // rings spreading from the splash
                int sx, sy;
                const float age = s_cast_t - s_cast_len;
                if (project(s_tx, 0, s_tz, &sx, &sy))
                    for (int k = 0; k < 2; k++) {
                        const int r = 4 + iroundf((age * 60 + k * 10) * 200.0f / (100 + dist * 0.2f));
                        nv_gfx_line(sx - r, sy, sx - r / 2, sy - r / 4, C565(220, 236, 250));
                        nv_gfx_line(sx - r / 2, sy - r / 4, sx + r / 2, sy - r / 4, C565(220, 236, 250));
                        nv_gfx_line(sx + r / 2, sy - r / 4, sx + r, sy, C565(220, 236, 250));
                        nv_gfx_line(sx - r, sy, sx - r / 2, sy + r / 4, C565(180, 210, 236));
                        nv_gfx_line(sx - r / 2, sy + r / 4, sx + r / 2, sy + r / 4, C565(180, 210, 236));
                        nv_gfx_line(sx + r / 2, sy + r / 4, sx + r, sy, C565(180, 210, 236));
                    }
            }
            draw_rod(0, now);
            break;
        }
        case ST_RETRIEVE:
        case ST_STRIKE: {
            hud_top();
            // Depth gauge: surface at the top, lake bed at the bottom.
            panel(8, 70, 24, 130);
            nv_gfx_rect(12, 74, 16, 122, C565(20, 60, 90));
            const int ly = 74 + iroundf((1.0f - s_ly / SURF) * 118);
            nv_gfx_rect(10, ly, 20, 4, C_YELLOW);
            const char *act = s_twitch_t > 0 ? T("STRAPPO", "TWITCH") : s_in.a ? T("RECUPERO", "REELING") : T("PAUSA", "PAUSE");
            text_sh(40, 74, act, C_CYAN, 1);
            char b[24], t[12];
            fmt_int(t, iroundf(sqrtf_(s_lx * s_lx + s_lz * s_lz) / 100)); b[0] = 0; cat(b, t); cat(b, " M");
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
            s_icon_a = "b_reel"; s_icon_b = "b_twitch";
            draw_controls(T("MULINELLO", "REEL"), T("STRAPPO", "TWITCH"), 0);
            break;
        }
        case ST_FIGHT: {
            hud_top();
            int sx, sy;
            // The line from the rod (bottom of the screen) to the fish's mouth.
            const float d = sqrtf_(s_lx * s_lx + s_lz * s_lz) + 1e-3f, ux = s_lx / d, uz = s_lz / d;
            const float fx = ux * s_fight.dist + uz * s_fight.fx, fz = uz * s_fight.dist - ux * s_fight.fx;
            if (project(fx, s_fight.fy, fz, &sx, &sy)) {       // taut line: straighter the harder it pulls
                const float mx = (s_rtx + sx) / 2, my = (s_rty + sy) / 2 + (1.0f - s_fight.tension) * 40;
                int lx = iroundf(s_rtx), ly = iroundf(s_rty);
                for (int i = 1; i <= 10; i++) {
                    const float t = i / 10.0f, u = 1 - t;
                    const int x = iroundf(u * u * s_rtx + 2 * u * t * mx + t * t * sx);
                    const int y = iroundf(u * u * s_rty + 2 * u * t * my + t * t * sy);
                    nv_gfx_line(lx, ly, x, y, C565(236, 236, 244));
                    lx = x; ly = y;
                }
            }
            draw_rod(s_in.a, now);
            panel(W / 2 - 124, 44, 248, 44);
            text_sh(W / 2 - 116, 50, T("TENSIONE", "TENSION"), C_GREY, 1);
            bar(W / 2 - 116, 62, 232, 10, s_fight.tension, s_fight.tension > 0.85f ? C_RED : C_GREEN, 1);
            text_sh(W / 2 - 116, 76, T("FORZA PESCE", "FISH POWER"), C_GREY, 1);
            bar(W / 2 + 4, 77, 112, 5, s_fight.stamina, C_CYAN, 0);
            char b[24], t[12];
            fmt_int(t, iroundf(s_fight.dist / 100)); b[0] = 0; cat(b, t); cat(b, " M");
            text_sh(8, 74, b, C_WHITE, 2);
            // Which way the fish is running: steer the rod the other way.
            if (s_fight.run_dir != 0 && s_fight.run > 0.35f) {
                const int ax = s_fight.run_dir > 0 ? W - 60 : 60, s = s_fight.run_dir > 0 ? 1 : -1;
                nv_gfx_tri(ax - s * 16, 130, ax - s * 16, 162, ax + s * 16, 146, C_RED);
                text_sh(ax - 30, 168, T("TIRA", "PULL"), C_RED, 1);
            }
            if (s_fight.jumping) {
                panel(W / 2 - 120, 120, 240, 44);
                text_c(128, T("SALTO! PREMI B", "JUMP! PRESS B"), ((now / 90) & 1) ? C_YELLOW : C_WHITE, 3);
            }
            s_icon_a = "b_reel"; s_icon_b = "b_twitch";
            draw_controls(T("MULINELLO", "REEL"), T("GIU'", "DOWN"), 1);
            break;
        }
        case ST_CATCH: {
            char b[48], t[24];
            panel(W / 2 - 200, 196, 400, 96);
            fish_art(s_catch_sp, W / 2 - 194, 200, 132, 88);
            text_sh(W / 2 - 54, 206, sp_name(s_catch_sp), s_catch_sp == SP_GOLD ? C_YELLOW : C_WHITE, 2);
            fmt_kg(t, s_catch_kg);
            text_sh(W / 2 - 54, 230, t, C_YELLOW, 3);
            if (s_new_rank >= 0 && s_new_rank < 3) {
                static const char *const medal[3] = { "a_gold", "a_silver", "a_bronze" };
                nv_gfx_image(medal[s_new_rank], W / 2 + 150, 204, 36, 36);
            }
            if (s_new_rank >= 0) {
                b[0] = 0; cat(b, T("RECORD N.", "RECORD #")); fmt_int(t, s_new_rank + 1); cat(b, t); cat(b, "!");
                text_sh(W / 2 - 54, 264, b, (now / 200) & 1 ? C_GREEN : C_WHITE, 2);
            }
            text_c(18, T("PRESO!", "LANDED!"), C_GREEN, 4);
            if (s_bonus) text_c(52, T("PESCE GROSSO  TEMPO +10!", "BIG FISH  TIME +10!"), (now / 150) & 1 ? C_YELLOW : C_WHITE, 2);
            break;
        }
        case ST_LOST: hud_top(); break;
        case ST_WEIGH: draw_weigh(now); break;
        case ST_OVER: draw_over(now); break;
        }
        hud_msg(now);
    }
}
