// Vertice Bass — arcade lake fishing for NucleoOS on the Vertice 3D engine.
// A tournament of six lakes: each stage has a clock and a weight quota. Pick a lure, aim and cast
// from the boat, work the lure under water (reel / pause / twitch) until a fish strikes, set the
// hook, then fight it — rod against its runs, reel without snapping the line, catch the jumps.
// Weigh-in at the bell: make the quota to move on. The ten biggest fish ever go on the record
// wall. Touch, USB keyboard or gamepad; Italian or English from the system language.
#include "bass.h"
#ifdef NV_SIM
#include <stdio.h>
#endif

enum { ST_TITLE, ST_RECORDS, ST_STAGE, ST_LURE, ST_AIM, ST_CAST, ST_RETRIEVE, ST_STRIKE, ST_FIGHT,
       ST_CATCH, ST_LOST, ST_WEIGH, ST_OVER, ST_INTRO, ST_SELECT, ST_NAME };

#define C_WHITE  C565(255, 255, 255)
#define C_YELLOW C565(255, 214, 40)
#define C_SHADOW C565(8, 12, 22)
#define C_GREY   C565(170, 175, 190)
#define C_RED    C565(235, 50, 40)
#define C_GREEN  C565(60, 220, 90)
#define C_CYAN   C565(90, 200, 255)
#define C_PANEL  C565(14, 30, 54)
#define C_EDGE   C565(70, 150, 220)

static int s_lang = 0, s_state = ST_TITLE, s_state_ms, s_menu, s_pad, s_prev_pad, s_prev_down;
static int s_stage, s_loop, s_lure, s_time_ms, s_catches, s_msg_until;
static float s_total, s_run_total, s_stage_best, s_quota;
static const char *s_msg = "";
static int s_best_run100;
static Record s_rec[NRECORDS];
static int s_new_rank = -1;

// ---- text ------------------------------------------------------------------------------------------------
// Languages: 0 Italian, 1 English (both in the sources), 2 Spanish, 3 French, 4 German (lang.h, written
// by art/lang.py from the English: rerun it after changing any text).
enum { L_IT, L_EN, L_ES, L_FR, L_DE };
#include "lang.h"
static int strcmp_(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }
static const char *T(const char *it, const char *en) {
    if (s_lang == L_IT) return it;
    if (s_lang == L_EN) return en;
    int lo = 0, hi = K_TR_N - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2, c = strcmp_(en, k_tr[mid][0]);
        if (!c) return k_tr[mid][s_lang - 1];
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return en;                                     // not translated: English
}
// After a place number: 1ST / 1º / 1ER / 1.
static const char *ord_sfx(int rk) {
    switch (s_lang) {
    case L_EN: return rk == 0 ? "ST" : rk == 1 ? "ND" : rk == 2 ? "RD" : "TH";
    case L_ES: return "º";
    case L_FR: return rk == 0 ? "ER" : "E";
    case L_DE: return ".";
    default:   return "°";
    }
}
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
// ---- lettering: Montserrat Bold atlases (art/font.py) drawn glyph by glyph with nv_gfx_sprite,
// tinted to any colour; outline and drop shadow are baked into the glyphs.
#include "fontx.h"
enum { F_S, F_M, F_L };
typedef struct { const char *img; const Glyph *g; int line; } Font;
static const Font k_font[3] = { { "fs", k_fs, FS_LINE }, { "fm", k_fm, FM_LINE }, { "fl", k_fl, FL_LINE } };
// The glyph of the next character of *s (UTF-8), advancing *s; the game speaks in capitals.
static int glyph_at(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    int c = *p++;
    if (c >= 0xC0 && c < 0xE0 && (*p & 0xC0) == 0x80) c = ((c & 31) << 6) | (*p++ & 63);
    else if (c >= 0x80) { while ((*p & 0xC0) == 0x80) p++; c = '?'; }
    *s = (const char *)p;
    if (c >= 'a' && c <= 'z') return c - 32 - 32;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) c -= 0x20;     // à -> À ... þ -> Þ
    if (c >= 0xA0 && c <= 0xFF && k_lat1[c - 0xA0]) return k_lat1[c - 0xA0];
    if (c < 32 || c > 126) c = '?';
    return c - 32;
}
// v * pct / 100 rounded to nearest, also for negative v
static int scale_px(int v, int pct) { const int n = v * pct + 50; return n >= 0 ? n / 100 : -((99 - n) / 100); }
// Draw s with font f at pct percent of its size; (x, y) is the top-left of the line. Returns the width.
static int ftext(int x, int y, const char *s, int col, int f, int pct) {
    const Font *F = &k_font[f];
    int pen = 0;
    while (*s) {
        const int gi = glyph_at(&s);
        const Glyph *g = &F->g[gi];
        if (gi != 0) {
            // Every edge from the same scaled grid, rounded to nearest: scaled text keeps one baseline
            // (rounding each glyph's offset and size apart nudged letters up or down a pixel).
            // (floor, not C's truncation: cells start left of and above the pen - accents, the first
            // letter's outline - and truncating those toward zero moved them a pixel and squeezed them)
            const int gx = (pen + 2) / 4 + g->ox;
            const int x0 = scale_px(gx, pct), x1 = scale_px(gx + g->w, pct);
            const int y0 = scale_px(g->oy, pct), y1 = scale_px(g->oy + g->h, pct);
            nv_gfx_sprite(F->img, g->x, g->y, g->w, g->h, x + x0, y + y0, x1 - x0, y1 - y0, col);
        }
        pen += g->adv;                                     // quarter pixels
    }
    return pen * pct / 400;
}
static int ftext_w(const char *s, int f, int pct) {
    int pen = 0;
    while (*s) pen += k_font[f].g[glyph_at(&s)].adv;
    return pen * pct / 400;
}
// The old 5x7 sizes (scale 1, 2, 3...) mapped onto the three atlases: same cap height.
// The biggest size (<= pct) at which s fits in maxw: long names shrink instead of spilling out.
static int fit_pct(const char *s, int f, int pct, int maxw) {
    while (pct > 60 && ftext_w(s, f, pct) > maxw) pct -= 4;
    return pct;
}
static int sc_font(int sc) { return sc <= 1 ? F_S : sc == 2 ? F_M : F_L; }
static int sc_pct(int sc) { return sc <= 2 ? 100 : sc * 31; }
static int tw(const char *s, int sc) { return ftext_w(s, sc_font(sc), sc_pct(sc)); }
static void text_sh(int x, int y, const char *s, int col, int sc) {
    ftext(x, y - (sc <= 1 ? 2 : sc == 2 ? 3 : sc), s, col, sc_font(sc), sc_pct(sc));
}
static void text_c(int y, const char *s, int col, int sc) { text_sh((W - tw(s, sc)) / 2, y, s, col, sc); }
// ---- the UI kit (ABI 15 nv_gfx_panel: rounded corners, gradients, translucency) ------------------
#define C_GLASS_T C565(24, 48, 84)
#define C_GLASS_B C565(6, 16, 34)
// A HUD plate: dark translucent glass with rounded corners and a thin lit accent along its top.
static void panel(int x, int y, int w, int h) {
    nv_gfx_panel(x, y, w, h, h < 24 ? 5 : 8, C_GLASS_T, C_GLASS_B, 208);
    nv_gfx_panel(x + 6, y + 1, w - 12, 2, 1, C_EDGE, C_EDGE, 210);
}
// An opaque card (menus): a coloured rim, a navy face with a soft gloss on its upper half.
static void card(int x, int y, int w, int h, int rim, int rim2) {
    const int r = h < 40 ? 7 : 11;
    nv_gfx_panel(x + 2, y + 5, w, h, r, C_SHADOW, C_SHADOW, 150);
    nv_gfx_panel(x, y, w, h, r, rim, rim2, 255);
    nv_gfx_panel(x + 2, y + 2, w - 4, h - 4, r - 2, C565(30, 58, 100), C565(10, 22, 46), 255);
    nv_gfx_panel(x + 4, y + 3, w - 8, (h - 6) / 2, r - 3, C565(255, 255, 255), C565(30, 58, 100), 34);
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
// Fisherman's Bait's six weigh-in classes, by where the weight sits in the species' range.
enum { CL_POOR, CL_SMALL, CL_AVERAGE, CL_LARGE, CL_BIG, CL_HUGE };
static int weight_class(int sp, float kg) {
    const Species *S = &g_species[sp];
    const float q = (kg - S->kg_min) / (S->kg_max - S->kg_min + 1e-3f);
    return q < 0.06f ? CL_POOR : q < 0.16f ? CL_SMALL : q < 0.32f ? CL_AVERAGE : q < 0.52f ? CL_LARGE : q < 0.75f ? CL_BIG : CL_HUGE;
}
static const char *class_name(int c) {
    static const char *const it[6] = { "SCARSO", "PICCOLO", "MEDIO", "GRANDE", "GROSSO!", "ENORME!!" };
    static const char *const en[6] = { "POOR", "SMALL", "AVERAGE", "LARGE", "BIG!", "HUGE!!" };
    return T(it[c], en[c]);
}
static int size_class(int sp, float kg) {                  // 0 small fry, 1 keeper, 2 big, 3 monster (the art)
    const int c = weight_class(sp, kg);
    return c <= CL_SMALL ? 0 : c <= CL_AVERAGE ? 1 : c <= CL_BIG ? 2 : 3;
}
// Seconds a catch buys: by weight class, the bass family (the game fish) worth the most; a streak adds.
static int time_bonus(int sp, float kg, int combo) {
    static const uint8_t base[6] = { 3, 6, 10, 16, 23, 32 };
    int tb = base[weight_class(sp, kg)];
    if (sp != SP_BASS && sp != SP_GOLD) tb = (tb * 6 + 5) / 10;
    if (combo >= 2) tb += combo > 5 ? 5 : combo;
    return tb;
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
// The OS mixer (ABI v15) plays everything at once, in its own task apart from the game loop: one-shot
// effects (tools/gen_bass_sfx.py), live loops whose volume and pitch follow the game every frame (the
// reel, the drag, the straining line, the outboard, the lake around you) and the music, streamed
// from the SD (ACE-Step themes).
static void sfx(const char *name) { nv_snd_play(name, 256, 256, 0); }
static void sfxv(const char *name, int vol, int pitch) { nv_snd_play(name, vol, pitch, 0); }
static int s_vamb = -1, s_vreel = -1, s_vdrag = -1, s_vcreak = -1, s_vmotor = -1, s_vmus = -1;
static const char *s_amb_name, *s_mus_name;
static void loop_set(int *v, const char *name, int vol, int pitch) {
    if (vol <= 0) { if (*v >= 0) { nv_snd_stop(*v, 120); *v = -1; } return; }
    if (*v < 0) *v = nv_snd_play(name, vol, pitch, NV_SND_LOOP);
    else nv_snd_set(*v, vol, pitch);
}
// The soundtrack lives in its own package, "bass-music" (manifest "requires"; the store installs it
// with the game): the game's own package would pass the device's 24 MB per-package limit with it.
static int music_play(const char *name, int vol) {
    char full[40] = "bass-music:";
    int k = 11;
    for (int i = 0; name[i] && k < 39; i++) full[k++] = name[i];
    full[k] = 0;
    return nv_snd_play(full, vol, 256, NV_SND_LOOP | NV_SND_STREAM);
}
static void music2(const char *name, const char *fallback, int vol) {   // NULL: fade the music out
    if (name && s_mus_name && !strcmp_(name, s_mus_name)) { if (s_vmus >= 0) nv_snd_set(s_vmus, vol, -1); return; }
    if (s_vmus >= 0) nv_snd_stop(s_vmus, 700);
    s_vmus = name ? music_play(name, vol) : -1;
    if (s_vmus < 0 && fallback) s_vmus = music_play(fallback, vol);
    s_mus_name = name;
}
static void music(const char *name, int vol) { music2(name, 0, vol); }
static void ambience(const char *name, int vol) {
    if (name != s_amb_name) { if (s_vamb >= 0) nv_snd_stop(s_vamb, 400); s_vamb = -1; s_amb_name = name; }
    if (name) loop_set(&s_vamb, name, vol, 256);
}
static int s_reel_at;
static void sfx_reel(int now) { s_reel_at = now; }      // the reel loop runs while this is fresh
static void snd_splash(void) { sfx("splash"); }
static void snd_click(void);
static void snd_strike(void) { sfx("strike"); }
static void snd_fanfare(void) { sfx("catch"); }

// ---- input: touch zones or pad -------------------------------------------------------------------------------
typedef struct { int left, right, up, down, a, b, a_hit, b_hit, d_hit, tap, tx, ty, crank, crank_hit; } Pad;
// a: reel held (a button, the touch key, or the right-stick handle turning); a_hit: a fresh press of
// a button only (a handle turning unevenly must not read as a jerk); crank_hit: a burst on the handle.
static Pad s_in;
typedef struct { int x, y, w, h; } Rect;
static const Rect kB = { 362, 222, 64, 72 }, kA = { 432, 204, 74, 90 };
static int in_rect(const Rect *r, int x, int y) { return x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h; }
static int pad_connected(void) { return (s_pad & (NV_PAD_KEYBOARD | NV_PAD_GAMEPAD)) != 0; }

// Analog reel (ABI v11 controllers): how far the right trigger (R2/RT) is pressed, 0..1. The
// harder you squeeze, the faster the reel turns. Buttons, keys and touch reel at full speed.
static float s_reel_amt;
static float s_crank;                       // the right-stick reel handle, 0..1 (turn speed)
static int s_have_pad;                    // a v11 controller is connected (rumble, triggers)
// Rumble in two layers. Events (the hook-set, a run's jolt, a snap) own the motors for their length;
// under them haptics_frame() keeps a continuous feel refreshed every 50 ms: the fish's weight on the
// line, its head shakes, the drag's ratchet, the reel, the outboard.
static int s_rumble_hold;
static void rumble(int low, int high, int ms) {
    if (!s_have_pad) return;
    nv_pad_rumble(0, low, high, ms);
    s_rumble_hold = nv_millis() + ms;
}
static void rumble_bg(float low, float high, int force) {
    static int at, idle;
    const int now = nv_millis();
    if (!s_have_pad || now < s_rumble_hold || (now - at < 50 && !force)) return;
    const int lo = (int)clampf(low, 0, 65535), hi = (int)clampf(high, 0, 65535);
    if (!lo && !hi) { if (idle) return; idle = 1; } else idle = 0;   // silence is sent once
    at = now;
    nv_pad_rumble(0, lo, hi, 90);
}
static void snd_click(void) { sfx("click"); rumble(0, 6000, 22); }   // a light tap under the thumb too
static Fight s_fight;                       // (defined with the game state below)
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
#ifdef BASS_AUTOPLAY   // perf runs on the board: a robot player walks the menus, casts, reels and fights
    {
        const int now = nv_millis(), in = now - s_state_ms, pulse = ((now / 300) & 1) ? NV_PAD_A : 0;
        int b = 0;
#ifdef BASS_SHOWCASE   // video: a player who takes the tour (menus at a reading pace) and fights well
        (void)in;
        {
#define AT(t0, bit) (in >= (t0) && in < (t0) + 120 ? (bit) : 0)
        static int title_seen, aims, entered;
        static int last_state = -1;
        if (s_state != last_state) {
            if (s_state == ST_AIM) aims++;
            if (last_state == ST_TITLE) title_seen++;
            last_state = s_state; entered = now;
        }
        const int in = now - entered;                                 // own clock: the title resets its own
        switch (s_state) {
        case ST_INTRO: break;                                         // the whole intro
        case ST_TITLE:
            if (!title_seen) b = AT(3000, NV_PAD_DOWN) | AT(4200, NV_PAD_A);          // to the records
            else b = AT(1500, NV_PAD_UP) | AT(2700, NV_PAD_A);                        // then play
            break;
        case ST_RECORDS: b = AT(4500, NV_PAD_B); break;
        case ST_SELECT:
            b = AT(1800, NV_PAD_RIGHT) | AT(2800, NV_PAD_RIGHT) | AT(3800, NV_PAD_DOWN) | AT(4800, NV_PAD_UP) |
                AT(5600, NV_PAD_LEFT) | AT(6400, NV_PAD_LEFT) | AT(7600, NV_PAD_A);
            break;
        case ST_STAGE: b = AT(4500, NV_PAD_A); break;
        case ST_LURE:
            b = AT(1800, NV_PAD_RIGHT) | AT(3000, NV_PAD_RIGHT) | AT(4200, NV_PAD_RIGHT) | AT(5400, NV_PAD_LEFT) |
                AT(6200, NV_PAD_LEFT) | AT(7000, NV_PAD_LEFT) | AT(8200, NV_PAD_A);
            break;
        case ST_AIM:
            if (aims == 1) {                                          // a short drive to a spot first
                if (in > 1200 && in < 3600) b |= NV_PAD_UP;
                if (in > 2600 && in < 3300) b |= NV_PAD_LEFT;
                if (in > 5200 && in < 6000) b |= NV_PAD_A;
            } else b = (in > 2200 && in < 2800) ? NV_PAD_A : 0;
            break;
        case ST_CAST: break;
        case ST_RETRIEVE: b = ((in / 2500) & 1) ? NV_PAD_A : 0; break;
        case ST_STRIKE: b = in > 120 ? NV_PAD_DOWN : 0; break;
        case ST_FIGHT:
            if (s_fight.jumping) b = NV_PAD_DOWN;                     // rod down while it is in the air
            else {
                if (s_fight.strain > 0.35f || s_fight.tension > 0.9f) b = NV_PAD_B;   // give line in the red
                else if (s_fight.tension < 0.78f) b = NV_PAD_A;
                if (s_fight.run_dir > 0 && s_fight.run > 0.35f) b |= NV_PAD_LEFT;     // rod against the run
                if (s_fight.run_dir < 0 && s_fight.run > 0.35f) b |= NV_PAD_RIGHT;
            }
            break;
        case ST_NAME: b = AT(1500, NV_PAD_UP) | AT(2100, NV_PAD_UP) | AT(2900, NV_PAD_A) | AT(3500, NV_PAD_UP) |
                          AT(4300, NV_PAD_A) | AT(5200, NV_PAD_A); break;
        default: b = in > 3000 ? pulse : 0; break;
        }
#undef AT
        }
#else
        switch (s_state) {
        case ST_AIM: b = (in > 1800 && in < 2400) ? NV_PAD_A : 0; break;
        case ST_CAST: break;
        case ST_RETRIEVE: b = ((in / 2500) & 1) ? NV_PAD_A : 0; break;
        case ST_STRIKE: b = pulse; break;
        case ST_FIGHT: b = s_fight.tension < 0.8f ? NV_PAD_A : 0; break;
        default: b = in > 900 ? pulse : 0; break;
        }
#endif
        s_pad = b | NV_PAD_KEYBOARD;
    }
#endif
    if (s_pad & NV_PAD_LEFT) p.left = 1;
    if (s_pad & NV_PAD_RIGHT) p.right = 1;
    if (s_pad & NV_PAD_UP) p.up = 1;
    if (s_pad & NV_PAD_DOWN) p.down = 1;
    // A rod in the hands: the right stick is the reel handle (below), A reels too; the shoulder
    // buttons and triggers behind (L1 L2 R1 R2) let line go, as the fingers on the spool would.
    // Behind buttons give line only on the water: in the menus and at the cast R does nothing.
    const int on_water = s_state == ST_RETRIEVE || s_state == ST_STRIKE || s_state == ST_FIGHT;
    if (s_pad & NV_PAD_A) p.a = 1;
    if (s_pad & (NV_PAD_B | NV_PAD_L)) p.b = 1;
    if (on_water && (s_pad & NV_PAD_R)) p.b = 1;
    s_reel_amt = p.a ? 1.0f : 0.0f;
    s_have_pad = 0;
    if (nv_pad_count() > 0) {                 // player 1: triggers and the sticks
        nv_pad_state_t st;
        if (nv_pad_state(0, &st, sizeof st) > 0) {
            s_have_pad = st.rumble;
            if (st.lt > 16000) p.b = 1;                          // L2: give line
            if (on_water && st.rt > 16000) p.b = 1;              // R2: give line
            if (st.lx < -14000) p.left = 1;
            if (st.lx > 14000) p.right = 1;
            if (st.ly < -16000) p.up = 1;
            if (st.ly > 16000) p.down = 1;
            {   // The right stick as a reel handle: turn it in circles, the faster the faster you reel
                // (either way round). Only on the water, so it never confirms anything in the menus.
                static float prev_a, omega;
                static int prev_ok, prev_ms;
                const int now = nv_millis(), ms = now - prev_ms;
                prev_ms = now;
                const int fishing = on_water;
                const float rx = st.rx, ry = st.ry;
                const int out = rx * rx + ry * ry > 18000.0f * 18000.0f;    // pushed to the rim
                if (fishing && out && ms > 0 && ms < 200) {
                    const float a = atan2f_(ry, rx);
                    if (prev_ok) omega += (fabsf_(wrap_pi(a - prev_a)) * 1000.0f / ms - omega) * 0.35f;
                    prev_a = a; prev_ok = 1;
                } else {
                    prev_ok = out && fishing;
                    if (prev_ok) prev_a = atan2f_(ry, rx);
                    omega *= 0.7f;                                    // let go: the handle stops
                }
                const float crank = clampf(omega / (2 * PI_F * 2.0f), 0, 1);   // two turns a second = flat out
                s_crank = crank;
                if (fishing && crank > 0.06f && crank > s_reel_amt) { s_reel_amt = crank; p.crank = 1; }
                static float prev_crank;
                p.crank_hit = fishing && crank > 0.4f && prev_crank <= 0.4f;   // a sudden hard turn: sets the hook
                prev_crank = crank;
            }
        }
    }
    static int prev_d;
    p.a_hit = p.a && !prev_a; p.b_hit = p.b && !prev_b; p.d_hit = p.down && !prev_d;
    prev_a = p.a; prev_b = p.b; prev_d = p.down;
    if (p.crank) p.a = 1;                                    // the handle reels, but never presses
    if (!p.tx && !p.ty) { p.tx = s_in.tx; p.ty = s_in.ty; }
    s_in = p;
}
static int pressed(int bit) { return (s_pad & bit) && !(s_prev_pad & bit); }
// "Confirm": A on a pad, or a tap anywhere not on the on-screen buttons.
static int confirm(void) {
    return pressed(NV_PAD_A | NV_PAD_START) || (s_in.tap && !in_rect(&kB, s_in.tx, s_in.ty));
}

static void button(const Rect *r, int on, const char *label, int col, const char *icon) {
    // A glossy round key: drop shadow, coloured ring, dark face lit from above, the icon and its name.
    const int d = (r->w < r->h ? r->w : r->h) - 6, x = r->x + (r->w - d) / 2, y0 = r->y + (r->h - d) / 2 - 2, y = y0 + (on ? 3 : 0);
    nv_gfx_panel(x + 2, y0 + 6, d, d, d / 2, C_SHADOW, C_SHADOW, 150);
    nv_gfx_panel(x, y, d, d, d / 2, col, C565(20, 30, 46), 255);
    nv_gfx_panel(x + 4, y + 4, d - 8, d - 8, d / 2 - 4, on ? C565(80, 110, 160) : C565(40, 56, 86), on ? C565(30, 46, 80) : C565(12, 20, 38), 255);
    nv_gfx_panel(x + 8, y + 6, d - 16, (d - 12) / 2, (d - 16) / 2, C565(255, 255, 255), C565(40, 56, 86), on ? 60 : 40);
    const int cx = x + d / 2, cy = y + d / 2;
    if (icon) {
        const int s = on ? 38 : 34;
        nv_gfx_image(icon, cx - s / 2, cy - s / 2 - 7, s, s);
        ftext(cx - ftext_w(label, F_S, 100) / 2, cy + d / 2 - 20, label, on ? C_YELLOW : C_WHITE, F_S, 100);
    } else {
        ftext(cx - ftext_w(label, F_S, 100) / 2, cy - 7, label, on ? C_YELLOW : C_WHITE, F_S, 100);
    }
}
// The d-pad: a bevelled cross (horizontal bar x 4..146, vertical bar y 196..292, centre 75,244).
// Pressed arms light up; with mode 1 (left/right only) up and down are drawn dimmed.
static void draw_dpad(int mode) {
    // Rounded arms with a lit top, the pressed arm glowing; mode 1 (left/right only) dims up/down.
    nv_gfx_panel(6, 228, 142, 46, 10, C_SHADOW, C_SHADOW, 140); nv_gfx_panel(54, 200, 46, 98, 10, C_SHADOW, C_SHADOW, 140);
    nv_gfx_panel(4, 222, 142, 44, 10, C565(70, 82, 110), C565(26, 32, 48), 235);
    nv_gfx_panel(52, 196, 46, 96, 10, C565(70, 82, 110), C565(26, 32, 48), 235);
    const uint16_t lt = C565(90, 150, 230), lb = C565(30, 70, 140);
    if (s_in.left) nv_gfx_panel(6, 224, 44, 40, 8, lt, lb, 255);
    if (s_in.right) nv_gfx_panel(100, 224, 44, 40, 8, lt, lb, 255);
    if (s_in.up && mode == 2) nv_gfx_panel(54, 198, 42, 26, 8, lt, lb, 255);
    if (s_in.down && mode >= 2) nv_gfx_panel(54, 264, 42, 26, 8, lt, lb, 255);
    nv_gfx_panel(64, 233, 22, 22, 11, C565(24, 30, 44), C565(50, 60, 84), 255);           // hub
    const uint16_t on = C_YELLOW, idle = C565(226, 232, 244), off = C565(80, 88, 108);
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
static int key_w(int k) { const char *t = key_name(k); return t ? ftext_w(t, F_S, 100) + 12 : 17; }
static void key_badge(int x, int y, int k) {
    const char *t = key_name(k);
    if (t) {
        const int w = key_w(k);
        const uint16_t col = k == K_A ? C565(50, 190, 90) : k == K_B ? C565(220, 70, 60) : C565(90, 100, 128);
        const uint16_t col2 = k == K_A ? C565(20, 110, 50) : k == K_B ? C565(130, 30, 30) : C565(44, 50, 70);
        nv_gfx_panel(x + 1, y + 2, w, 15, 4, C_SHADOW, C_SHADOW, 160);
        nv_gfx_panel(x, y, w, 15, 4, col, col2, 255);
        ftext(x + 6, y + 1, t, C_WHITE, F_S, 100);
        return;
    }
    const int cx = x + 8, cy = y + 7;                      // a small cross, the used arms yellow
    nv_gfx_rect(cx - 7, cy - 2, 15, 5, C565(70, 80, 104)); nv_gfx_rect(cx - 2, cy - 7, 5, 15, C565(70, 80, 104));
    if (k != K_UD) { nv_gfx_rect(cx - 7, cy - 2, 4, 5, C_YELLOW); nv_gfx_rect(cx + 4, cy - 2, 4, 5, C_YELLOW); }
    if (k != K_LR) { nv_gfx_rect(cx - 2, cy - 7, 5, 4, C_YELLOW); nv_gfx_rect(cx - 2, cy + 4, 5, 4, C_YELLOW); }
}
static void pad_hints(const int *keys, const char *const *labels, int n) {
    int total = 0;
    for (int i = 0; i < n; i++) total += key_w(keys[i]) + 5 + ftext_w(labels[i], F_S, 100) + (i + 1 < n ? 14 : 0);
    int x = (W - total) / 2;
    panel(x - 8, H - 24, total + 16, 21);
    for (int i = 0; i < n; i++) {
        key_badge(x, H - 20, keys[i]);
        x += key_w(keys[i]) + 5;
        ftext(x, H - 20, labels[i], C_WHITE, F_S, 100);
        x += ftext_w(labels[i], F_S, 100) + 14;
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
    // A rounded key: shadow, accent rim, navy face with gloss; it sinks while the finger is on it.
    const Rect r = { x, y, w, h };
    const int held = s_prev_down && in_rect(&r, s_in.tx, s_in.ty), dy = held ? 3 : 0, rr = h / 3;
    nv_gfx_panel(x + 2, y + 5, w, h, rr, C_SHADOW, C_SHADOW, 160);
    nv_gfx_panel(x, y + dy, w, h, rr, accent, C565(16, 28, 50), 255);
    nv_gfx_panel(x + 2, y + dy + 2, w - 4, h - 4, rr - 2, held ? C565(70, 110, 170) : C565(36, 66, 112),
                 held ? C565(30, 56, 100) : C565(12, 26, 54), 255);
    nv_gfx_panel(x + 5, y + dy + 3, w - 10, (h - 6) / 2, rr - 3, C565(255, 255, 255), C565(36, 66, 112), held ? 50 : 36);
    const int cx = x + w / 2, cy = y + dy + h / 2;
    if (label[0] == '<' && !label[1]) nv_gfx_tri(cx + 8, cy - 9, cx + 8, cy + 9, cx - 9, cy, C_WHITE);
    else if (label[0] == '>' && !label[1]) nv_gfx_tri(cx - 8, cy - 9, cx - 8, cy + 9, cx + 9, cy, C_WHITE);
    else ftext(cx - ftext_w(label, F_M, 100) / 2, cy - 11, label, C_WHITE, F_M, 100);
    return s_in.tap && in_rect(&r, s_in.tx, s_in.ty);
}
// A row of up to four buttons along the bottom; returns the index tapped or -1.
static int ui_row(const char *const *labels, int n) {
    if (pad_connected()) return -1;                        // a keyboard or pad: hints instead (pad_hints)
    const int w = n <= 2 ? 150 : n == 3 ? 130 : 112, gap = 8, total = n * w + (n - 1) * gap;
    int hit = -1;
    for (int i = 0; i < n; i++)
        if (ui_btn((W - total) / 2 + i * (w + gap), H - 40, w, 32, labels[i],
                   i == n - 1 ? C565(70, 220, 110) : (i == 0 && n > 1) ? C565(230, 90, 80) : C_CYAN)) hit = i;   // back left, go right
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
    vx_lens(iroundf(fov), 16, g_cam_far);
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
    enum { NS = 12 };
    int ring[10][NS];
    for (int r = 0; r < nr; r++)
        for (int k = 0; k < NS; k++) {
            const float a = k * 2 * PI_F / NS + PI_F / NS;
            ring[r][k] = mb_v(cosf_(a) * prof[r], sinf_(a) * prof[r] * tall, zs[r], 0, 0);
        }
    for (int r = 0; r < nr - 1; r++)
        for (int k = 0; k < NS; k++) {
            const float sy = sinf_((k + 0.5f) * 2 * PI_F / NS + PI_F / NS);
            const int m = sy > 0.55f ? back : sy < -0.45f ? belly : flank;
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
            static const float zs[9] = { -19, -16, -11, -5, 1, 7, 12, 15.5f, 17.5f }, pr[9] = { 1.2f, 3.6f, 5.8f, 7.4f, 8.3f, 8.3f, 7.2f, 5.0f, 2.2f };
            lure_body(zs, pr, 9, 1.25f, back, flank, belly);
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
            static const float zs[8] = { -17, -14, -9, -3, 4, 10, 14, 16 }, pr[8] = { 2.2f, 4.0f, 5.6f, 6.8f, 7.5f, 7.9f, 8.0f, 7.6f };
            lure_body(zs, pr, 8, 1.0f, back, flank, belly);
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
            static const float zs[7] = { 4, 7, 10.5f, 14, 17.5f, 20.5f, 22.5f }, pr[7] = { 3, 5.6f, 7.4f, 8, 7.2f, 5.0f, 2.2f };
            lure_body(zs, pr, 7, 1.0f, head2, head, head);
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
        s_lure_obj[k] = mb_commit_ex(steel, 0, VX_MESH_SMOOTH);
        vx_obj_scale(s_lure_obj[k], k == LURE_WORM ? 100 : 95);
        vx_obj_show(s_lure_obj[k], 0);
    }
}
#define LURE_SIZE 0.46f                    // the models at 46 %: a lure is a fifth of a bass, as in life
float g_lure_half = 22.0f * LURE_SIZE;     // half the lure's length (world units): the fish keep their nose there
static float s_lure_scale = 1.0f;          // extra scale (in the fish's mouth)
static void lure_pose(float x, float y, float z, float yaw) {
    for (int k = 0; k < NLURES; k++) vx_obj_show(s_lure_obj[k], k == s_lure);
    const float ls = LURE_SIZE * s_lure_scale;
    vx_obj_scale(s_lure_obj[s_lure], iroundf((s_lure == LURE_WORM ? 100 : 95) * ls));
    for (int j = 0; j < WORM_SEGS; j++) vx_obj_scale(s_worm_seg[j], iroundf(100 * ls));
    const float wl = WORM_LEN * ls;
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
        px -= sinf_(a) * wl; pz -= cosf_(a) * wl;
        for (int j = 0; j < WORM_SEGS; j++) {
            a = yaw + sinf_(t * 6 - (j + 1) * 1.1f) * (0.25f + 0.2f * j) * (0.4f + w);
            vx_obj_pos(s_worm_seg[j], iroundf(px), iroundf(py), iroundf(pz));
            vx_obj_rot(s_worm_seg[j], 0, iroundf(deg(a)), 0);
            px -= sinf_(a) * wl; pz -= cosf_(a) * wl;
            py += sinf_(t * 4 - j) * 0.6f * w;
        }
    }
}
static void lure_hide(void) {
    for (int k = 0; k < NLURES; k++) vx_obj_show(s_lure_obj[k], 0);
    for (int j = 0; j < WORM_SEGS; j++) vx_obj_show(s_worm_seg[j], 0);
}

// ---- records -----------------------------------------------------------------------------------------------------
// The anglers' initials on the wall (3 letters each), kept apart from records.bin so the older saves
// stay valid; names.bin also remembers the last initials typed.
static char s_names[NRECORDS + 1][4];
static void records_load(void) {
    if (nv_load("records.bin", s_rec, sizeof s_rec) != (int)sizeof s_rec)
        for (int i = 0; i < NRECORDS; i++) { s_rec[i].kg100 = 0; s_rec[i].species = 0; s_rec[i].stage = 0; }
    if (nv_load("bestrun.bin", &s_best_run100, 4) != 4) s_best_run100 = 0;
    if (nv_load("names.bin", s_names, sizeof s_names) != (int)sizeof s_names)
        for (int i = 0; i <= NRECORDS; i++) { s_names[i][0] = s_names[i][1] = s_names[i][2] = i < NRECORDS ? '-' : 'A'; s_names[i][3] = 0; }
    for (int i = 0; i <= NRECORDS; i++) s_names[i][3] = 0;
}
// Insert a catch; returns its rank (0-based) on the wall, or -1.
static int records_add(int sp, float kg, int stage) {
    const int k = iroundf(kg * 100);
    int pos = NRECORDS;
    while (pos > 0 && s_rec[pos - 1].kg100 < k) pos--;
    if (pos >= NRECORDS) return -1;
    for (int i = NRECORDS - 1; i > pos; i--) { s_rec[i] = s_rec[i - 1]; for (int c = 0; c < 4; c++) s_names[i][c] = s_names[i - 1][c]; }
    s_rec[pos].kg100 = (uint16_t)k; s_rec[pos].species = (uint8_t)sp; s_rec[pos].stage = (uint8_t)stage;
    for (int c = 0; c < 4; c++) s_names[pos][c] = s_names[NRECORDS][c];       // the last initials, to edit
    nv_save("records.bin", s_rec, sizeof s_rec);
    nv_save("names.bin", s_names, sizeof s_names);
    return pos;
}

// ---- game state ----------------------------------------------------------------------------------------------------
static float s_aim, s_power, s_power_t;
static int s_charging;
static int s_boil = -1, s_boil_at;          // a fish breaking the surface at a spot (where to cast)
static float s_boil_x, s_boil_z;
static float s_lx, s_ly, s_lz, s_cast_t, s_cast_len, s_tx, s_tz, s_twitch_t, s_orbit;
static int s_strike_fish, s_strike_until, s_twitches, s_nibble = -1;
static int s_lure_open, s_lure_pop;
static int s_name_slot;                                    // initials entry: the letter being set
static const Rect kNameOk = { W / 2 - 75, 236, 150, 34 };                        // the in-game lure strip (open by touch / shown after B)
static int s_air, s_air_at;                  // fight: the fish is in the air (above-water view)
static float s_air_h, s_air_x, s_air_z;
static float s_fyaw, s_fpx, s_fpz, s_ccp[3], s_cct[3], s_rcp[3], s_rct[3];     // fight: fish heading, last position, camera
static Fight s_fight;
static int s_catch_sp;
static float s_catch_kg;
static uint8_t s_list_sp[16];
static int s_bonus;
static float s_list_kg[16];
// The boat moves: up/down on the d-pad start the outboard and drive, left/right steer.
static float s_bx, s_bz, s_bspeed;
static float s_rtx = W - 170, s_rty = 96;                   // the rod tip on screen
static float s_bvx, s_bvz, s_bturn, s_bpitch, s_bpitch_v, s_broll, s_broll_v;
#define WAKE_N 14
static float s_wake[WAKE_N][3];                 // x, z, birth (s)
static int s_wake_i, s_wake_at;
static float s_acam[3], s_acam_ok;                         // the aim camera, lagging the boat
static void water_ring(float x, float z, float r, int fade);
static int s_ring_seed;                                   // water_ring: which foam ring (its gaps)
static int s_ring_hull;                                   // water_ring: leave out what is under the boat
static int s_engine, s_engine_at, s_motor_at;              // engine 0 off, 1 cranking, 2 running
// Arcade juice: the big banner ("FISH ON!"), a white flash, a hit-stop freeze on the hook-set.
static const char *s_ban;
static int s_ban_at, s_ban_col, s_flash_until, s_hitstop_until;
static void banner(const char *t, int col, int now) { s_ban = t; s_ban_at = now; s_ban_col = col; }
static int s_combo, s_qualified, s_qual_now, s_perfect, s_junk = -1, s_tb;
static int s_junk_on = -1;                                  // a bed item hanging on the line (lake_junk_*)
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

// ---- the lure in the water --------------------------------------------------------------------------------
// The line is a length of nylon (s_line) from the rod tip to the lure. While the lure is further away
// than that, the line is taut; while it is closer, the difference is slack lying in the water.
//  - Not reeling: nothing holds the lure back. Sinkers fall (the worm slowly, the jig fast) and the
//    spool lets line go as they fall, so they reach the bed and rest there; floaters rise.
//  - Reeling: the reel first winds in the slack (the lure doesn't move), then the taut line draws
//    the lure toward the boat. The line comes up from the bed to the rod tip, so the closer the lure
//    gets the more the pull lifts it; the crankbait's lip fights that and dives instead.
//  - A twitch (DOWN) is a sharp pull of the rod: a dart for the crank, a "pop" for the popper, a hop
//    off the bottom for the worm and the jig.
#define LURE_BED 7.0f                          // a lure resting on the lake bed
static int s_on_bed, s_bed_at;                 // touching the bed (and since when)
static float s_slack;                          // line in the water beyond the straight distance
static float lure_physics(int reel, float ramt, int pay, float dt, int now, float ux, float uz) {
    static const float reel_speed[NLURES] = { 270, 200, 150, 185 };
    static const float fall[NLURES] = { -55, -120, 75, 210 };       // free fall, units/s (negative rises)
    float an[3];
    rope_anchor(an);
    const float hx0 = s_lx - an[0], hz0 = s_lz - an[2];
    const float hd = sqrtf_(hx0 * hx0 + hz0 * hz0) + 1e-3f;          // horizontal distance to the rod
    const float D0 = sqrtf_(hd * hd + (s_ly - an[1]) * (s_ly - an[1]));
    float rs = reel ? reel_speed[s_lure] * ramt : 0.0f;
    if (s_junk_on >= 0) rs *= 0.55f;                         // junk on the line: heavy, it drags
    // A twitch: the rod snaps back; the lure darts toward the boat and jumps (each in its own way).
    if (s_in.d_hit) {
        s_twitch_t = 0.35f; s_twitches++;
        if (s_lure == LURE_POPPER) {
            vx_emit(g_fx_bubble, iroundf(s_lx), iroundf(s_ly), iroundf(s_lz), 0, 80, 0, 50, 12);
            vx_emit(g_fx_splash, iroundf(s_lx), iroundf(SURF), iroundf(s_lz), 0, 140, 0, 50, 6);
            sfx("plop");
        } else {
            vx_emit(g_fx_bubble, iroundf(s_lx), iroundf(s_ly), iroundf(s_lz), 0, 80, 0, 40, 4);
            sfx("click");
            sfxv("twitch", 200, 230 + rnd(50));                         // the rod whips, the lure darts
        }
        if (s_lure == LURE_WORM || s_lure == LURE_JIG) {
            s_ly += s_lure == LURE_JIG ? 70.0f : 45.0f;                // the hop
            if (s_on_bed) vx_emit(g_fx_dust, iroundf(s_lx), 8, iroundf(s_lz), 0, 50, 0, 46, 8);   // a puff of silt
        }
    }
    if (s_twitch_t > 0.2f) rs += s_lure == LURE_POPPER ? 160.0f : 380.0f;
    // Vertical: on a slack line the lure falls (or floats up) at its own rate.
    float fy = -fall[s_lure];
    if (rs > 0 && s_slack <= 0) {
        if (s_lure == LURE_CRANK) {             // the lip digs in: dives to ~2.6 m, shallower near the boat
            const float want = SURF - 40 - 230 * clampf((hd - 200) / 700, 0, 1) * clampf(ramt * 1.3f, 0.3f, 1);
            fy = clampf((want - s_ly) * 3, -150, 90);
        } else if (s_lure == LURE_POPPER) {
            fy = 0;
        } else {                                // the line lifts it, more as it comes close
            fy = rs * clampf((an[1] - s_ly) / (hd + 150) * 0.55f, 0.05f, 0.9f) - fall[s_lure] * 0.35f;
        }
    }
    if (pay) fy = -fall[s_lure];                // giving line: it just falls (or floats)
    s_ly += fy * dt;
    if (s_lure == LURE_POPPER || s_ly > SURF - 8) s_ly = SURF - 8;
    const int was_bed = s_on_bed;
    s_on_bed = s_ly <= LURE_BED;
    if (s_on_bed) s_ly = LURE_BED;
    if (s_on_bed && !was_bed) {                 // touchdown: a puff of silt, a soft knock up the line
        s_bed_at = now;
        vx_emit(g_fx_dust, iroundf(s_lx), 8, iroundf(s_lz), 0, 40, 0, 50, 10);
        sfx("click");
    }
    // Horizontal: the reel winds the slack first, then draws the lure toward the rod.
    s_slack -= rs * dt;
    if (pay) s_slack += 160 * dt;
    if (s_slack < 0) {
        const float pull = -s_slack;
        s_slack = 0;
        const float step = pull < hd - 40 ? pull : (hd > 40 ? hd - 40 : 0);
        s_lx -= ux * step; s_lz -= uz * step;
        if (s_on_bed && s_lure != LURE_JIG && s_lure != LURE_WORM) s_on_bed = 0;
    }
    if (s_slack > 900) s_slack = 900;
    // The line out = straight distance + slack (it pays out freely as a lure sinks away from the rod).
    {
        const float hx = s_lx - an[0], hz = s_lz - an[2], vy = s_ly - an[1];
        s_line = sqrtf_(hx * hx + hz * hz + vy * vy) + s_slack;
    }
    (void)D0;
    return rs;
}

static void go(int st, int now) {
    static const char *const names[] = { "title", "records", "stage", "lure", "aim", "cast", "retrieve",
                                         "strike", "fight", "catch", "lost", "weigh", "over", "intro", "select", "name" };
    char b[40] = "bass: ";
    cat(b, names[st]);
    nv_log(NV_LOG_WARN, b);
    if (s_junk_on >= 0 && st != ST_RETRIEVE && st != ST_CATCH) {   // left the retrieve with junk on: it drops
        lake_junk_drop(s_junk_on, s_lx, s_lz);
        s_junk_on = -1;
    }
    s_state = st; s_state_ms = now;
}

static int s_go_at;                          // when this stage's READY/GO started (0 = not yet)
static int s_tb_at;                          // when the last time bonus was added (clock popup)
static int s_last_sec = -1;                  // last whole second beeped in the final countdown
static void stage_conditions(void);
static void start_stage(int now) {
    lake_build(s_stage, s_loop);
    fish_build();
    build_lures();
    lake_view(0);
    {   // each lake has its own theme on the intermission card (ACE-Step)
        char m[8] = "play0";
        m[4] = (char)('0' + s_stage);
        static char lm[8];
        for (int i = 0; i < 8; i++) lm[i] = m[i];
        music(lm, 230);
    }
    s_quota = g_stage[s_stage].quota_kg * (1.0f + 0.35f * s_loop);
    stage_conditions();
    s_time_ms = g_stage[s_stage].time_s * 1000;
    s_total = 0; s_catches = 0; s_stage_best = 0;
    s_aim = 0; s_bx = s_bz = s_bspeed = 0; s_engine = 0;
    s_bvx = s_bvz = s_bturn = s_bpitch = s_bpitch_v = s_broll = s_broll_v = 0; s_acam_ok = 0;
    for (int i = 0; i < WAKE_N; i++) s_wake[i][2] = 0;
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

static void aim_camera_boat(void);
// ---- the boat ----------------------------------------------------------------------------------------------
// A bass boat with some weight to it. The heading turns slowly on the bow's trolling motor at rest
// and faster under power; the hull carries its speed (it glides when the throttle closes) and slides
// a little sideways in fast turns before the keel bites. Pitch: the bow climbs as it gets going, then
// drops as it comes onto the plane; it noses down when you back off. Roll: it leans into the turn.
// Waves rock it all the time. The wake is a V of rings spreading behind the stern, spray off the bow.
static void boat_drive(float dt, int now) {
    const float fx = sinf_(s_aim), fz = cosf_(s_aim);
    const int drive = s_charging ? 0 : (s_in.up ? 1 : s_in.down ? -1 : 0);
    if (drive && s_engine == 0) { s_engine = 1; s_engine_at = now; sfx("motor_start"); s_shake = 4; rumble(9000, 3000, 400); }
    if (s_engine == 1 && now - s_engine_at > 900) { s_engine = 2; s_motor_at = now; }
    float vf = s_bvx * fx + s_bvz * fz, vl = s_bvx * fz - s_bvz * fx;           // forward / sideways
    const float sp01 = clampf(fabsf_(vf) / BOAT_TOP, 0, 1);
    const int steer = s_in.right - s_in.left;
    const float want_turn = steer * (0.55f + 0.85f * clampf(fabsf_(vf) / 260.0f, 0, 1)) * (vf < -20 ? -1.0f : 1.0f);
    s_bturn += (want_turn - s_bturn) * clampf(dt * 4.0f, 0, 1);
    s_aim = wrap_pi(s_aim + s_bturn * dt);
    const float want = s_engine == 2 ? drive * (drive > 0 ? BOAT_TOP : 160.0f) : 0.0f;
    const float acc = drive ? (drive > 0 ? 260.0f : 200.0f) : 120.0f;         // closed throttle: it glides
    vf += clampf(want - vf, -dt * acc, dt * acc);
    vl *= 1.0f - clampf(dt * (2.6f - 1.6f * sp01), 0, 1);                       // the keel bites, less at speed
    vl += -s_bturn * vf * 0.18f * dt * 4.0f;                                    // fast turns slide outward
    const float nfx = sinf_(s_aim), nfz = cosf_(s_aim);
    s_bvx = nfx * vf + nfz * vl; s_bvz = nfz * vf - nfx * vl;
    s_bspeed = vf;
    if (s_engine == 2 && !drive && fabsf_(vf) < 5 && now - s_motor_at > 2600) s_engine = 0;   // idles, then cuts out
    if (s_engine == 2 && drive) s_motor_at = now;
    s_bx += s_bvx * dt; s_bz += s_bvz * dt;
    {   // the bank: a soft bump back off it
        const float r = sqrtf_(s_bx * s_bx + s_bz * s_bz);
        const float lim = lake_shore(atan2f_(s_bx, s_bz)) - 520;
        if (r > lim) {
            const float nx = s_bx / r, nz = s_bz / r, vr = s_bvx * nx + s_bvz * nz;
            s_bx = nx * lim; s_bz = nz * lim;
            if (vr > 0) {
                s_bvx -= nx * vr * 1.4f; s_bvz -= nz * vr * 1.4f;
                if (vr > 60) { sfxv("thud", 320, 200); s_shake = 3 + vr * 0.02f; rumble(14000, 6000, 160); s_bpitch_v += 30; }
            }
        }
    }
    // pitch and roll: damped springs toward the attitude the speed and the turn ask for
    const float hump = sinf_(PI_F * clampf(sp01 * 1.25f, 0, 1));                 // climbing onto the plane
    float want_pitch = -(hump * 7.0f + sp01 * 2.0f);
    if (drive == 0 && vf > 60) want_pitch += 2.5f;                               // throttle off: the bow settles
    if (drive < 0) want_pitch += 3.0f * clampf(-vf / 160.0f, 0, 1);
    const float want_roll = -s_bturn * sp01 * 10.0f;
    s_bpitch_v += ((want_pitch - s_bpitch) * 18.0f - s_bpitch_v * 5.0f) * dt;
    s_bpitch += s_bpitch_v * dt;
    s_broll_v += ((want_roll - s_broll) * 14.0f - s_broll_v * 4.0f) * dt;
    s_broll += s_broll_v * dt;
    // the wake: rings left behind the stern, spray kicked up off the bow
    if (fabsf_(vf) > 70 && now - s_wake_at > 220) {      // rings well apart, not a stack of lines
        s_wake_at = now;
        float *w = s_wake[s_wake_i++ % WAKE_N];
        const float back = vf > 0 ? 190.0f : -150.0f;     // the end it leaves behind: the bow when reversing
        w[0] = s_bx - nfx * back; w[1] = s_bz - nfz * back; w[2] = now * 0.001f;
        const float j = (float)(rnd(60) - 30);
        vx_emit(g_fx_splash, iroundf(s_bx - nfx * 200 + nfz * j), 4, iroundf(s_bz - nfz * 200 - nfx * j), 0, 60 + (int)(sp01 * 90), 0, 50, 2);
        if (vf > 230) {
            vx_emit(g_fx_splash, iroundf(s_bx + nfx * 120 + nfz * 56), 12, iroundf(s_bz + nfz * 120 - nfx * 56), iroundf(nfz * 90), 120, iroundf(-nfx * 90), 40, 1);
            vx_emit(g_fx_splash, iroundf(s_bx + nfx * 120 - nfz * 56), 12, iroundf(s_bz + nfz * 120 + nfx * 56), iroundf(-nfz * 90), 120, iroundf(nfx * 90), 40, 1);
        }
    }
}
static void draw_wake(int now) {
    const float t = now * 0.001f;
    for (int i = 0; i < WAKE_N; i++) {
        const float age = t - s_wake[i][2];
        if (s_wake[i][2] <= 0 || age > 1.7f || age < 0) continue;
        s_ring_hull = 1; s_ring_seed = i;                   // a soft foam ring, fading into the water as it spreads
        water_ring(s_wake[i][0], s_wake[i][1], 16 + age * 105, 70 + iroundf(clampf(age / 1.7f, 0, 1) * 186));
        s_ring_hull = 0;
    }
}
static void aim_camera(void) {
    const float fx = sinf_(s_aim), fz = cosf_(s_aim), sp = fabsf_(s_bspeed);
    // High over the angler's shoulder (Fisherman's Bait): the eye above his head, so the water where
    // the lure lands shows above him instead of behind his back
    const float back = 390 + sp * 0.32f, up = 345 + sp * 0.09f;
    const float want[3] = { s_bx - fx * back, up, s_bz - fz * back };
    const float k = s_acam_ok ? clampf(0.016f * 6.0f, 0, 1) : 1.0f;       // a little lag behind the boat
    for (int j = 0; j < 3; j++) s_acam[j] += (want[j] - s_acam[j]) * k;
    s_acam_ok = 1;
    cam(s_acam[0], s_acam[1], s_acam[2], s_bx + fx * 1300, -90, s_bz + fz * 1300, 60 + sp * 0.014f);   // tilted down: him to the knees
    aim_camera_boat();
}
// The boat on the water: the springs' pitch and roll, waves rocking it, the engine's shiver.
static void aim_camera_boat(void) {
    const int t = nv_millis();
    const float bob = sinf_(t * 0.0022f) * 2.5f + sinf_(t * 0.0037f + 1) * 1.2f + (s_engine == 2 ? sinf_(t * 0.05f) * 0.8f : 0);
    const float wave_p = sinf_(t * 0.0017f) * 1.3f, wave_r = sinf_(t * 0.0013f + 2) * 1.6f;
    const int pitch = iroundf(s_bpitch + wave_p), roll = iroundf(s_broll + wave_r);
    vx_obj_pos(g_boat, iroundf(s_bx), iroundf(bob), iroundf(s_bz));
    vx_obj_rot(g_boat, pitch, iroundf(deg(s_aim)), roll);
    vx_obj_pos(g_boat_trim, iroundf(s_bx), iroundf(bob), iroundf(s_bz));
    vx_obj_rot(g_boat_trim, pitch, iroundf(deg(s_aim)), roll);
}
// ---- the angler on the bow -------------------------------------------------------------------------------
#include "angler.h"
static int s_pose = -1;                               // the pose shown (-1: none, the old 2D rod instead)
static float s_tip[3];                                // the rod tip in the world, this frame
static void cam_right(float *rx, float *rz) {         // the camera's right vector on the ground plane
    const float fx = s_ct[0] - s_cp[0], fz = s_ct[2] - s_cp[2], l = sqrtf_(fx * fx + fz * fz) + 1e-6f;
    *rx = fz / l; *rz = -fx / l;
}
// Show pose k on the bow (or none), work out where its rod tip is (world and screen) for the line.
static float s_an_pos[3];                    // the posed angler billboard's centre
#include "angler_mask.h"
// Is screen point (sx, sy) on the angler's body? The 2D line is drawn after the 3D frame, so the
// part of it that runs behind him (the lure out on the water sits right behind him from the boat
// camera) is hidden here instead of being painted across his back.
static int on_angler(int sx, int sy) {
    if (s_pose < 0) return 0;
    int cx, cy, tx, ty;
    if (!project(s_an_pos[0], s_an_pos[1], s_an_pos[2], &cx, &cy) ||
        !project(s_an_pos[0], s_an_pos[1] + ANGLER_SIZE / 2, s_an_pos[2], &tx, &ty)) return 0;
    const int size = 2 * (cy - ty);
    if (size < 8) return 0;
    const int u = (sx - (cx - size / 2)) * 32 / size, v = (sy - ty) * 32 / size;
    if (u < 0 || u >= 32 || v < 0 || v >= 32) return 0;
    return (int)((k_angler_mask[s_pose][v] >> u) & 1);
}
static void angler_pose(int k) {
    for (int i = 0; i < 4; i++) if (g_angler[i] >= 0) vx_obj_show(g_angler[i], i == k);
    if (k < 0 || g_angler[k] < 0) { s_pose = -1; return; }
    s_pose = k;
    const int t = nv_millis();
    const float bob = sinf_(t * 0.0022f) * 2.5f + sinf_(t * 0.0037f + 1) * 1.2f;
    float rx, rz;
    cam_right(&rx, &rz);
    // the figure sits left of centre in its square (the rod takes the right): shift it so the man
    // stands on the boat's centre line
    const float sh = ANGLER_SIZE * (0.5f - k_body_u[k]);
    const float x = s_bx + sinf_(s_aim) * 70 + rx * sh, z = s_bz + cosf_(s_aim) * 70 + rz * sh, y = 47 + bob + ANGLER_SIZE / 2;
    vx_obj_pos(g_angler[k], iroundf(x), iroundf(y), iroundf(z));
    s_an_pos[0] = x; s_an_pos[1] = y; s_an_pos[2] = z;
    const float u = k_rod_tip[k][0] - 0.5f, v = k_rod_tip[k][1] - 0.5f;
    s_tip[0] = x + rx * u * ANGLER_SIZE; s_tip[1] = y + v * ANGLER_SIZE; s_tip[2] = z + rz * u * ANGLER_SIZE;
    int sx, sy;
    if (project(s_tip[0], s_tip[1], s_tip[2], &sx, &sy)) { s_rtx = (float)sx; s_rty = (float)sy; }
}
static void rod_tip(float *x, float *y, float *z) {
    if (s_pose >= 0) { *x = s_tip[0]; *y = s_tip[1]; *z = s_tip[2]; return; }
    *x = s_bx + sinf_(s_aim) * 40 + cosf_(s_aim) * 30; *y = 150; *z = s_bz + cosf_(s_aim) * 40 - sinf_(s_aim) * 30;
}

// ---- HUD pieces -------------------------------------------------------------------------------------------------------
// ---- arcade readouts: slanted tabular digits (art/digits.py), tinted ------------------------------------
#include "digits.h"
static int dg_index(char c) { return c >= '0' && c <= '9' ? c - '0' : c == '.' ? 10 : c == ':' ? 11 : c == '-' ? 12 : -1; }
static int dg_step(int i) { return i == 10 || i == 11 ? 9 : 21; }
static int dg_w(const char *s, int pct) { int w = 0; for (; *s; s++) { const int i = dg_index(*s); if (i >= 0) w += dg_step(i); } return w * pct / 100; }
static int dg_text(int x, int y, const char *s, int tint, int pct) {
    int pen = 0;
    for (; *s; s++) {
        const int i = dg_index(*s);
        if (i < 0) continue;
        const int ox = i == 10 || i == 11 ? -2 : -5;
        nv_gfx_sprite("dg", k_dg_x[i], 0, k_dg_w[i], DG_H, x + (pen + ox) * pct / 100, y, (k_dg_w[i] * pct + 99) / 100, (DG_H * pct + 99) / 100, tint);
        pen += dg_step(i);
    }
    return pen * pct / 100;
}
static void fmt_tenths(char *out, int ms) {               // 137.4
    if (ms < 0) ms = 0;
    const int d = ms / 100;
    const int k = fmt_int(out, d / 10);
    out[k] = '.'; out[k + 1] = (char)('0' + d % 10); out[k + 2] = 0;
}
// Each lake's conditions, shown in the corner as in the Konami games (weather, the hour, the water):
// they also shift where the fish sit (see fish_depth_bias).
typedef struct { const char *w_it, *w_en, *hour; int temp, depth_bias; } Cond;
static const Cond k_cond[NSTAGES] = {      // morning and evening the fish come up; the midday sun sends them down
    { "SERENO", "CLEAR", "07:40", 12, 50 }, { "TRAMONTO", "SUNSET", "19:20", 22, 90 }, { "NOTTE", "NIGHT", "23:50", 17, 30 },
    { "SOLE", "SUNNY", "14:10", 25, -90 }, { "NUVOLOSO", "CLOUDY", "10:30", 13, 0 }, { "SERENO", "CLEAR", "09:00", 18, 30 },
};
static void stage_conditions(void) { g_depth_bias = (float)k_cond[s_stage].depth_bias; }
// The HUD in play, laid out like Fisherman's Bait: conditions and the BIG 3 total top left, the clock
// in big red figures (tenths of a second) top centre, the lure top right; the livewell under the
// conditions. The gauges (depth, tension) and the line out are drawn by the states that use them.
static void hud_top(void) {
    char b[48], t[24];
    const int now = nv_millis();
    // conditions + BIG 3 TOTAL
    panel(4, 4, 180, 52);
    const Cond *c = &k_cond[s_stage];
    b[0] = 0; cat(b, T(c->w_it, c->w_en)); cat(b, " "); cat(b, c->hour);
    ftext(10, 6, b, C_WHITE, F_S, 100);
    {   // the water temperature, right-aligned in the box; the word goes when the line is too long
        char w2[24];
        fmt_int(t, c->temp);
        w2[0] = 0; cat(w2, T("ACQUA ", "WATER ")); cat(w2, t); cat(w2, "C");
        if (ftext_w(b, F_S, 100) + 10 + ftext_w(w2, F_S, 100) > 168) { w2[0] = 0; cat(w2, t); cat(w2, "C"); }
        ftext(178 - ftext_w(w2, F_S, 100), 6, w2, C_CYAN, F_S, 100);
    }
    ftext(10, 21, T("TOTALE TOP 3", "TOP 3 TOTAL"), C565(120, 255, 140), F_S, 100);

    {
        char kg[16]; const int k = (int)(s_total * 100 + 0.5f);
        int n = fmt_int(kg, k / 100); kg[n++] = '.'; kg[n++] = (char)('0' + (k / 10) % 10); kg[n++] = (char)('0' + k % 10); kg[n] = 0;
        const int x = 10 + ftext_w(T("TOTALE TOP 3", "TOP 3 TOTAL"), F_S, 100) + 8;
        dg_text(x, 19, kg, s_total >= s_quota ? C565(120, 255, 140) : C565(255, 230, 120), 50);
        ftext(x + dg_w(kg, 50) + 3, 21, "KG", C_GREY, F_S, 100);
    }
    {   // the quota as a thin bar under it
        nv_gfx_panel(10, 37, 168, 4, 2, C565(20, 28, 44), C565(20, 28, 44), 230);
        const int f = iroundf(clampf(s_total / (s_quota > 0 ? s_quota : 1), 0, 1) * 168);
        if (f > 3) nv_gfx_panel(10, 37, f, 4, 2, s_total >= s_quota ? C565(120, 255, 150) : C565(255, 230, 90),
                                s_total >= s_quota ? C565(30, 160, 70) : C565(220, 140, 20), 255);
        b[0] = 0; cat(b, T("QUOTA ", "QUOTA ")); fmt_kg(t, s_quota); cat(b, t);
        ftext(10, 42, b, C_GREY, F_S, 100);
    }
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
            const int x = 4 + k * 61;
            nv_gfx_panel(x, 59, 58, 20, 5, C565(20, 40, 70), C565(6, 14, 30), 180);
            if (top[k] < 0) { ftext(x + 4, 61, (const char[]){ (char)('1' + k), 0 }, C565(70, 90, 120), F_S, 100); continue; }
            fish_art_kg(s_list_sp[top[k]], s_list_kg[top[k]], x + 1, 60, 26, 17);
            char t2[16];
            fmt_kg(t2, s_list_kg[top[k]]);
            t2[4] = 0;
            ftext(x + 27, 62, t2, C_WHITE, F_S, 100);
        }
    }
    // the clock: big red figures with tenths, white and throbbing in the last ten seconds
    fmt_tenths(t, s_time_ms);
    {
        const int warn = s_time_ms < 10000, beat = warn && (s_time_ms % 1000) > 820;
        const int pct = beat ? 112 : 100, w = dg_w(t, pct);
        dg_text(W / 2 - w / 2, 2 - (pct - 100) / 6, t, warn && ((s_time_ms / 250) & 1) ? C_WHITE : C565(255, 56, 40), pct);
    }
    if (s_tb_at && now - s_tb_at < 1500 && s_tb > 0) {                  // "+12" floats up off the clock
        char b2[12], n2[8];
        b2[0] = '+'; b2[1] = 0; fmt_int(n2, s_tb); cat(b2, n2);
        ftext(W / 2 + 56, 26 - (now - s_tb_at) / 60, b2, ((now / 100) & 1) ? C_GREEN : C_WHITE, F_M, 100);
        ftext(W / 2 + 56 + ftext_w(b2, F_M, 100) + 2, 30 - (now - s_tb_at) / 60, "SEC", C_WHITE, F_S, 100);
    }
    if (s_combo >= 2) {
        char b2[16], n2[8];
        b2[0] = 0; cat(b2, "COMBO X"); fmt_int(n2, s_combo); cat(b2, n2);
        const int cw = ftext_w(b2, F_S, 100) + 16;
        nv_gfx_panel(W / 2 - cw / 2, 38, cw, 16, 8, C565(255, 170, 50), C565(200, 80, 20), 230);
        ftext(W / 2 - cw / 2 + 8, 38, b2, C_WHITE, F_S, 100);
    }
    // the lure, in a window top right
    {
        const char *ln = T(g_lure_it[s_lure], g_lure_en[s_lure]);
        const int bw = 46 + (ftext_w(ln, F_S, 100) > 52 ? ftext_w(ln, F_S, 100) : 52) + 8;
        panel(W - 4 - bw, 4, bw, 48);
        char n[8] = "lure0";
        n[4] = (char)('0' + s_lure);
        nv_gfx_image(n, W - bw, 6, 44, 44);
        ftext(W - bw + 44, 10, ln, C_WHITE, F_S, 100);
        b[0] = 0; cat(b, "X"); fmt_int(t, s_catches); cat(b, t);
        nv_gfx_image("i_fishes", W - bw + 44, 27, 16, 16);
        ftext(W - bw + 62, 28, b, C_YELLOW, F_S, 100);
    }
}
// The tall twin gauge on the right (Fisherman's Bait's DEP / TEN): the lure's depth (surface at the
// top) in blue, and the line's tension in green -> yellow -> red with the red zone at the top.
static void hud_gauges(float depth01, float tension, float strain, int now) {
    const int x = W - 40, y0 = 74, hh = 138;                     // under the lure box
    panel(x - 6, y0 - 18, 42, hh + 34);
    ftext(x - 2, y0 - 16, T("PR", "DP"), C_CYAN, F_S, 100);
    ftext(x + 17, y0 - 16, T("TE", "TN"), C565(255, 120, 100), F_S, 100);
    // depth tube
    nv_gfx_panel(x, y0, 12, hh, 6, C565(10, 30, 60), C565(4, 12, 30), 255);
    if (depth01 >= 0) {
        const int dy = y0 + iroundf(clampf(depth01, 0, 1) * (hh - 8));
        nv_gfx_panel(x - 2, dy, 16, 8, 4, C565(255, 240, 120), C565(220, 160, 30), 255);
    }
    // tension tube
    nv_gfx_panel(x + 18, y0, 12, hh, 6, C565(26, 30, 44), C565(14, 16, 26), 255);
    nv_gfx_panel(x + 18, y0, 12, hh * 15 / 100, 6, C565(140, 26, 26), C565(100, 16, 16), 255);
    if (tension > 0.01f) {
        const int f = iroundf(clampf(tension, 0, 1) * hh);
        const int hot = tension > 0.85f && ((now / 70) & 1);
        const uint16_t c0 = hot ? C_WHITE : tension > 0.85f ? C565(255, 70, 50) : tension > 0.6f ? C565(255, 220, 60) : C565(90, 240, 120);
        const uint16_t c1 = tension > 0.85f ? C565(170, 20, 20) : tension > 0.6f ? C565(200, 120, 20) : C565(30, 140, 60);
        nv_gfx_panel(x + 19, y0 + hh - f, 10, f, 5, c0, c1, 255);
    }
    if (strain > 0.02f) {                                       // strain builds beside it: let go before it fills
        const int sh = iroundf(clampf(strain, 0, 1) * hh);
        nv_gfx_panel(x + 32, y0 + hh - sh, 3, sh, 1, ((now / 90) & 1) ? C_RED : C_WHITE, C_RED, 255);
    }
    nv_gfx_image("b_reel", x + 1, y0 + hh + 1, 28, 28);
    if (s_crank > 0.05f) {                                      // the right stick turning the handle
        static float hand;
        hand += s_crank * 0.9f;
        const int cx = x + 15, cy = y0 + hh + 15;
        const int hx = cx + iroundf(cosf_(hand) * 11), hy = cy + iroundf(sinf_(hand) * 11);
        nv_gfx_line(cx, cy, hx, hy, C_YELLOW);
        nv_gfx_circle(hx, hy, 3, C_YELLOW);
    }
}
// LINE: the line out, in metres with tenths, bottom left like the original (above the d-pad on touch).
static void hud_line(float units) {
    char t[16];
    const int k = iroundf(units / 10.0f);                      // tenths of a metre
    int n = fmt_int(t, k / 10); t[n++] = '.'; t[n++] = (char)('0' + k % 10); t[n] = 0;
    const int y = pad_connected() ? H - 66 : 150;
    const char *lb = T("LENZA", "LINE");
    const int lw = ftext_w(lb, F_S, 100), nx = 10 + (lw + 4 > 36 ? lw + 4 : 36);   // a long word pushes the number
    const int pw = nx + dg_w(t, 72) + 3 + ftext_w("M", F_S, 100) + 8 - 4;
    panel(4, y, pw > 110 ? pw : 110, 34);
    ftext(10, y + 3, lb, C_WHITE, F_S, 100);
    dg_text(nx, y + 3, t, C565(255, 230, 120), 72);
    ftext(nx + dg_w(t, 72) + 3, y + 15, "M", C_WHITE, F_S, 100);
}
static void hud_msg(int now) {
    if (now < s_msg_until && s_msg[0]) {
        const int age = now - (s_msg_until - 900), pct = age < 120 ? 120 - age / 6 : 100;   // pops in
        int p2 = 70 * pct / 100;
        while (p2 > 30 && ftext_w(s_msg, F_L, p2) > W - 60) p2 -= 5;   // long hints shrink to fit
        const int w = ftext_w(s_msg, F_L, p2), y = 162;              // under the banners in the middle
        panel(W / 2 - w / 2 - 14, y, w + 28, 32);
        ftext(W / 2 - w / 2, y + 1, s_msg, C_YELLOW, F_L, p2);
    }
}
static void bar(int x, int y, int w, int h, float v, int col, int redline) {
    nv_gfx_panel(x - 2, y - 2, w + 4, h + 4, (h + 4) / 2, C_SHADOW, C_SHADOW, 200);
    nv_gfx_panel(x, y, w, h, h / 2, C565(30, 36, 50), C565(20, 24, 36), 255);
    if (redline) nv_gfx_panel(x + w * 85 / 100, y, w - w * 85 / 100, h, h / 2, C565(140, 30, 30), C565(90, 16, 16), 255);
    const int f = iroundf(clampf(v, 0, 1) * w);
    if (f > 2) nv_gfx_panel(x, y, f, h, h / 2, col, col, 255);
}
// ---- the rod and the line ------------------------------------------------------------------------------
// The rod is drawn over the frame, butt at the bottom right, as a bent curve: its tip is animated
// (wind-up over the shoulder while charging, a whip forward on release, pulled toward the fish and
// bent by the line tension in the fight). The line is a real 3D curve from the rod tip to the lure
// or the fish's mouth: sampled, sagging with its slack (never below the water while it lies on it),
// projected point by point.
static float s_rbend;                                       // rod bend (px)
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
    for (int i = 1; i <= 24; i++) {
        const float t = i / 24.0f;
        float px = from3[0] + (x - from3[0]) * t, py = from3[1] + (y - from3[1]) * t, pz = from3[2] + (z - from3[2]) * t;
        py -= sag * 4 * t * (1 - t);
        if (water && py < 1) py = 1;
        int sx, sy;
        if (!project(px, py, pz, &sx, &sy)) continue;
        if (!on_angler((lx + sx) / 2, (ly + sy) / 2))          // behind him: his body hides it
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
// Under the hull? (the rings are drawn over the 3D frame, so the parts below the boat are skipped)
static int under_hull(float x, float z) {
    const float fx = sinf_(s_aim), fz = cosf_(s_aim), dx = x - s_bx, dz = z - s_bz;
    const float along = dx * fx + dz * fz, side = dx * fz - dz * fx;
    return along > -215 && along < 185 && side > -78 && side < 78;
}
static void water_ring(float x, float z, float r, int fade) {
    if (fade >= 250 || r < 2) return;
    const uint16_t col = blend565(C565(236, 246, 255), g_stage[s_stage].water, fade);
    const uint16_t col2 = blend565(C565(236, 246, 255), g_stage[s_stage].water, fade + (256 - fade) / 2);
    int px = 0, py = 0, have = 0;
    for (int i = 0; i <= 32; i++) {
        const float a = i * (2 * PI_F / 32);
        int sx, sy;
        const float wx = x + sinf_(a) * r, wz = z + cosf_(a) * r;
        if ((s_ring_hull && under_hull(wx, wz)) || !project(wx, 1, wz, &sx, &sy)) { have = 0; continue; }
        // the wake is drawn over the 3D frame: the parts behind the angler (his mask) are left out;
        // and it is foam, not a drawn circle: arcs with gaps that widen as the ring spreads and fades
        const uint32_t hsh = ((uint32_t)i * 2654435761u ^ (uint32_t)s_ring_seed * 40503u) >> 24;   // 0..255
        const int gap = s_ring_hull && (int)hsh < 40 + fade * 2 / 3;
        if (have && !gap && !(s_ring_hull && on_angler((px + sx) / 2, (py + sy) / 2))) {
            nv_gfx_line(px, py, sx, sy, col);
            if (s_ring_hull && fade < 150) nv_gfx_line(px, py + 1, sx, sy + 1, col2);   // a soft second pixel
        }
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
static int s_screen_btn = -1, s_logo_at;
// Title: the painting, the logo slamming in (big to its size, a white flash on impact), the
// tagline, and a stack of menu cards on the right — tap one, or pick with the arrows and A.
static const Rect kTitleItem[3] = { { W - 196, 166, 184, 30 }, { W - 196, 202, 184, 30 }, { W - 196, 238, 184, 30 } };
static void menu_item(const Rect *r, const char *label, int sel, int now) {
    const int pulse = sel ? (int)(sinf_(now * 0.008f) * 2) : 0;
    if (sel) card(r->x - 3 - pulse, r->y - 3 - pulse, r->w + 6 + 2 * pulse, r->h + 6 + 2 * pulse, C565(255, 230, 90), C565(255, 140, 20));
    else card(r->x, r->y, r->w, r->h, C565(90, 160, 220), C565(30, 60, 110));
    ftext(r->x + (r->w - ftext_w(label, F_M, 100)) / 2, r->y + r->h / 2 - 11, label, sel ? C_YELLOW : C_WHITE, F_M, 100);
    if (sel) {                                                  // a pointer that nudges toward the item
        const int ax = r->x - 16 + iroundf(sinf_(now * 0.012f) * 3), ay = r->y + r->h / 2;
        nv_gfx_tri(ax, ay - 7, ax, ay + 7, ax + 10, ay, C_YELLOW);
    }
}
// ---- static-layer cache (the menus): what does not move is drawn once and snapshotted with
// nv_gfx_bg_save; the next frames copy it back in one pass (nv_gfx_bg_restore) and draw only what
// moves on top. A menu repainted every frame spent most of it on the full-screen painting, the
// translucent veil over it, a dozen gradient cards and a few hundred glyphs, all unchanged.
static uint32_t s_layer_key;
static int s_layer_ok;
static uint32_t layer_key(int a, int b, int c) {
    return (uint32_t)a * 2654435761u ^ (uint32_t)b * 40503u ^ (uint32_t)c * 97u ^ (uint32_t)s_lang * 7u
           ^ (uint32_t)pad_connected() * 0x9E3779B9u ^ (uint32_t)s_state_ms;
}
static int layer_cached(uint32_t key) {                 // 1: the static layer is back on screen
    if (s_layer_ok && key == s_layer_key) { nv_gfx_bg_restore(0, 0, W, H); return 1; }
    return 0;
}
static void layer_store(uint32_t key) { nv_gfx_bg_save(); s_layer_key = key; s_layer_ok = 1; }
// Warm the image cache while the title idles: the small pictures the next menus need, one every
// fourth frame, decoded off-screen (a 1x1 draw far outside the canvas loads the file and draws
// nothing), so a menu's first frame doesn't stall on the SD card. The full-screen paintings are left
// out: each is a 300 KB read, a visible hitch on the title.
static void prefetch_art(void) {
    static const char *const list[] = { "lk0", "lk1", "lk2", "lk3", "lk4", "lk5", "lure0", "lure1", "lure2", "lure3", "a_gold",
                                        "a_silver", "a_bronze", "fish0", "fish1", "fish2", "fish3", "fish4", "fish5",
                                        "fish6", "fish7", "fish8", "fish9" };
    static unsigned next, tick;
    if ((tick++ & 3) == 0 && next < sizeof list / sizeof list[0]) nv_gfx_image(list[next++], -4000, -4000, 1, 1);
}
static void draw_title(int now) {
    const int lt0 = now - s_logo_at;
    if (lt0 > 1500) prefetch_art();
    const uint32_t key = layer_key(ST_TITLE, s_best_run100, 0);
    if (lt0 > 420 && layer_cached(key)) {                   // settled: painting, tagline, plate, keys cached
        const int lw = 330, lh = 150, lx = W - 168 - lw / 2, ly = 76 - lh / 2;
        nv_gfx_image("logo", lx, ly + iroundf(sinf_(now * 0.0025f) * 2), lw, lh);
        if (s_menu >= 0 && s_menu < 3) {
            static const char *const it[3] = { "GIOCA", "RECORD", "ESCI" }, *const en[3] = { "PLAY", "RECORDS", "QUIT" };
            menu_item(&kTitleItem[s_menu], T(it[s_menu], en[s_menu]), 1, now);
        }
        return;
    }
    art("title");
    const int lt = now - s_logo_at;
    const int pct = lt < 320 ? 190 - lt * 90 / 320 : lt < 420 ? 100 + (420 - lt) * 6 / 100 : 100;
    const int lw = 330 * pct / 100, lh = 150 * pct / 100, lx = W - 168 - lw / 2, ly = 76 - lh / 2;
    if (lt <= 420) nv_gfx_image("logo", lx, ly, lw, lh);           // settled: drawn over the stored layer
    if (lt >= 320 && lt < 380) nv_gfx_panel(0, 0, W, H, 0, C_WHITE, C_WHITE, 200);
    if (lt > 420) {
        const char *tag = T("TORNEO DI PESCA ARCADE", "ARCADE FISHING TOURNAMENT");
        const int tp = fit_pct(tag, F_M, 100, 300);
        ftext(W - 168 - ftext_w(tag, F_M, tp) / 2, 137, tag, C_CYAN, F_M, tp);
    }
    static const char *const it[3] = { "GIOCA", "RECORD", "ESCI" }, *const en[3] = { "PLAY", "RECORDS", "QUIT" };
    const int settled = lt > 420;
    for (int i = 0; i < 3; i++) menu_item(&kTitleItem[i], T(it[i], en[i]), !settled && s_menu == i, now);
    if (s_best_run100 > 0) {
        char b[40], t[20];
        fmt_kg(t, s_best_run100 / 100.0f); b[0] = 0; cat(b, T("MIGLIOR TORNEO  ", "BEST RUN  ")); cat(b, t);
        const int bw = 26 + ftext_w(b, F_S, 100) + 10;          // trophy, text, margin: all inside the plate
        panel(8, 8, bw, 22);                                    // top left: clear of the key strip
        nv_gfx_image("a_trophy", 13, 10, 18, 18);
        ftext(34, 11, b, C_WHITE, F_S, 100);
    }
    if (pad_connected()) {
        static const int k[3] = { K_UD, K_A, K_SELECT };
        const char *l[3] = { T("SCEGLI", "CHOOSE"), "OK", T("ESCI", "EXIT") };
        pad_hints(k, l, 3);
    }
    if (settled) {                     // the logo and the chosen item go on top of the stored layer
        layer_store(key);
        nv_gfx_image("logo", lx, ly + iroundf(sinf_(now * 0.0025f) * 2), lw, lh);
        if (s_menu >= 0 && s_menu < 3) menu_item(&kTitleItem[s_menu], T(it[s_menu], en[s_menu]), 1, now);
    }
}

static void draw_records(void) {
    const uint32_t key = layer_key(ST_RECORDS, s_new_rank, 0);
    if (layer_cached(key)) {
        const char *labs[1] = { T("INDIETRO", "BACK") };
        s_screen_btn = ui_row(labs, 1);
        return;
    }
    art("dock");
    nv_gfx_panel(0, 0, W, H, 0, C565(4, 10, 24), C565(4, 10, 24), 80);
    const char *ttl = T("I 10 PESCI PIÙ GROSSI", "TOP 10 BIGGEST FISH");
    nv_gfx_image("a_trophy", (W - ftext_w(ttl, F_L, 72)) / 2 - 34, 2, 30, 30);
    nv_gfx_image("a_trophy", (W + ftext_w(ttl, F_L, 72)) / 2 + 4, 2, 30, 30);
    ftext((W - ftext_w(ttl, F_L, 72)) / 2, 4, ttl, C_YELLOW, F_L, 72);
    panel(30, 36, W - 60, 214);
    char b[64], t[24];
    for (int i = 0; i < NRECORDS; i++) {
        const int y = 42 + i * 20;
        const Record *r = &s_rec[i];
        if (i == s_new_rank) nv_gfx_panel(36, y - 1, W - 72, 19, 5, C565(120, 100, 20), C565(70, 56, 10), 200);
        else if (i & 1) nv_gfx_panel(36, y - 1, W - 72, 19, 5, C565(40, 70, 110), C565(40, 70, 110), 60);
        fmt_int(t, i + 1); b[0] = 0; cat(b, t); cat(b, ".");
        static const char *const medal[3] = { "a_gold", "a_silver", "a_bronze" };
        if (i < 3 && r->kg100) nv_gfx_image(medal[i], 42, y - 2, 20, 20);
        else ftext(46, y + 1, b, i == s_new_rank ? C_YELLOW : C_GREY, F_S, 100);
        if (!r->kg100) { ftext(110, y + 1, "-", C_GREY, F_S, 100); continue; }
        fish_art_kg(r->species, r->kg100 / 100.0f, 72, y - 1, 30, 19);
        ftext(106, y + 1, s_names[i], C_YELLOW, F_S, 100);
        ftext(148, y + 1, sp_name(r->species), i == s_new_rank ? C_YELLOW : C_WHITE, F_S, 100);
        ftext(278, y + 1, lake_name(r->stage), C_CYAN, F_S, 100);
        fmt_kg(t, r->kg100 / 100.0f);
        ftext(W - 44 - ftext_w(t, F_S, 100), y + 1, t, i == 0 ? C_YELLOW : C_WHITE, F_S, 100);
    }
    if (pad_connected()) { static const int k[1] = { K_B }; const char *l[1] = { T("INDIETRO", "BACK") }; pad_hints(k, l, 1); }
    layer_store(key);
    const char *labs[1] = { T("INDIETRO", "BACK") };
    s_screen_btn = ui_row(labs, 1);
}

static void draw_stage_card(int now) {
    char b[48], t[24];
    const int e = now - s_state_ms;
    lake_art(s_stage);
    // Top: the stage number and the lake's name sliding in.
    const int slide = e < 300 ? (300 - e) / 3 : 0;
    panel(30 - slide, 8, W - 60, 62);
    b[0] = 0; cat(b, T("TAPPA ", "STAGE ")); fmt_int(t, s_stage + 1 + s_loop * NSTAGES); cat(b, t);
    if (s_loop) { cat(b, T("  (GIRO ", "  (LAP ")); fmt_int(t, s_loop + 1); cat(b, t); cat(b, ")"); }
    ftext((W - ftext_w(b, F_S, 100)) / 2 - slide, 13, b, C_CYAN, F_S, 100);
    ftext((W - ftext_w(lake_name(s_stage), F_L, 90)) / 2 - slide, 25, lake_name(s_stage), C_WHITE, F_L, 90);
    // Bottom: the target and the clock, and who lives here (the three likeliest species).
    panel(30 + slide, 168, W - 60, 82);
    nv_gfx_image("i_scale", 46 + slide, 176, 22, 22);
    b[0] = 0; cat(b, T("QUOTA ", "QUOTA ")); fmt_kg(t, s_quota); cat(b, t);
    ftext(72 + slide, 177, b, C_YELLOW, F_M, 100);
    nv_gfx_image("i_clock", W / 2 + 30 + slide, 176, 22, 22);
    fmt_clock(t, s_time_ms);
    ftext(W / 2 + 56 + slide, 177, t, C_YELLOW, F_M, 100);
    {
        int top[3] = { -1, -1, -1 };
        for (int a = 0; a < NSPECIES; a++)
            for (int k = 0; k < 3; k++)
                if (top[k] < 0 || g_stage[s_stage].mix[a] > g_stage[s_stage].mix[top[k]]) {
                    for (int m = 2; m > k; m--) top[m] = top[m - 1];
                    top[k] = a;
                    break;
                }
        for (int k = 0; k < 3; k++) {
            const int x = 40 + slide + k * 144, cw = 140;        // three columns: who lives here
            char n[8] = "fish0";
            n[4] = (char)('0' + top[k]);
            nv_gfx_image(n, x + cw / 2 - 21, 202, 42, 28);
            const char *sn = sp_name(top[k]);
            const int sp = fit_pct(sn, F_S, 100, cw - 6);
            ftext(x + (cw - ftext_w(sn, F_S, sp)) / 2, 230, sn, C_WHITE, F_S, sp);
        }
    }
    const char *labs[2] = { "MENU", T("VIA!", "GO!") };
    s_screen_btn = ui_row(labs, 2);
    if (pad_connected()) { static const int k[2] = { K_A, K_B }; const char *l[2] = { T("VIA!", "GO!"), "MENU" }; pad_hints(k, l, 2); }
}

static void draw_lure_select(int now) {
    static const char *const d_it[NLURES] = { "MEZZ'ACQUA", "A GALLA", "FONDO, LENTO", "FONDO, A SALTI" };
    static const char *const d_en[NLURES] = { "MID WATER", "SURFACE", "BOTTOM, SLOW", "BOTTOM, HOPS" };
    static const char *const h_it[NLURES] = { "RECUPERO COSTANTE: SCENDE A MEZZ'ACQUA", "STRAPPI E PAUSE IN SUPERFICIE",
                                              "LASCIALO AFFONDARE, POI PICCOLI STRAPPI", "SALTELLI SUL FONDO" };
    static const char *const h_en[NLURES] = { "STEADY REELING: IT DIVES TO MID WATER", "TWITCH AND PAUSE ON THE SURFACE",
                                              "LET IT SINK, THEN SMALL TWITCHES", "HOP IT ALONG THE BED" };
    static const uint8_t depth[NLURES] = { 3, 1, 5, 5 }, speed[NLURES] = { 5, 2, 1, 3 };
    static const uint8_t best[NLURES][2] = { { SP_TROUT, SP_PIKE }, { SP_BASS, SP_PERCH }, { SP_CARP, SP_CATFISH }, { SP_ZANDER, SP_BASS } };
    const uint32_t key = layer_key(ST_LURE, s_lure, 0);
    const int cached = layer_cached(key);
    if (!cached) {
        art("school");
        nv_gfx_panel(0, 0, W, H, 0, C565(4, 10, 24), C565(4, 10, 24), 90);
        ftext((W - ftext_w(T("SCEGLI L'ESCA", "CHOOSE YOUR LURE"), F_L, 80)) / 2, 2, T("SCEGLI L'ESCA", "CHOOSE YOUR LURE"), C_YELLOW, F_L, 80);
    }
    // the other cards go into the stored layer; the chosen one (blinking rim, bobbing lure) every frame
    for (int pass = cached; pass < 2; pass++)
    for (int k = 0; k < NLURES; k++) {
        if ((s_lure == k) != (pass == 1)) continue;
        const int x = 10 + k * 124, y = 40, w = 116, h = 196, sel = s_lure == k;
        if (sel) card(x - 3, y - 3, w + 6, h + 6, ((now / 160) & 1) ? C565(255, 236, 110) : C565(255, 160, 30), C565(255, 120, 20));
        else card(x, y, w, h, C565(90, 160, 220), C565(30, 60, 110));
        char n[8] = "lure0";
        n[4] = (char)('0' + k);
        const int bob = sel ? iroundf(sinf_(now * 0.006f) * 3) : 0;
        nv_gfx_image(n, x + w / 2 - 36, y + 4 + bob, 72, 72);
        const char *nm = T(g_lure_it[k], g_lure_en[k]);
        ftext(x + (w - ftext_w(nm, F_M, 100)) / 2, y + 78, nm, sel ? C_YELLOW : C_WHITE, F_M, 100);
        ftext(x + (w - ftext_w(T(d_it[k], d_en[k]), F_S, 100)) / 2, y + 101, T(d_it[k], d_en[k]), C_CYAN, F_S, 100);
        for (int r = 0; r < 2; r++) {                              // depth and speed pips
            const int yy = y + 121 + r * 15;
            const char *lb = r ? T("VEL.", "SPD") : T("PROF.", "DEP");
            ftext(x + 8, yy - 3, lb, C_GREY, F_S, fit_pct(lb, F_S, 100, 43));
            const int lvl = r ? speed[k] : depth[k];
            for (int q = 0; q < 5; q++)
                nv_gfx_panel(x + 54 + q * 11, yy, 9, 9, 3, q < lvl ? C565(255, 214, 60) : C565(50, 60, 80),
                             q < lvl ? C565(230, 140, 20) : C565(30, 36, 50), 255);
        }
        for (int f = 0; f < 2; f++) {                              // who goes for it
            char fn[8] = "fish0";
            fn[4] = (char)('0' + best[k][f]);
            nv_gfx_image(fn, x + 9 + f * 50, y + 154, 48, 32);
        }
        if (pass == 0 && k == (s_lure == NLURES - 1 ? NLURES - 2 : NLURES - 1)) {
            // last static card done: the hint, the keys, then the snapshot
            const char *hint = T(h_it[s_lure], h_en[s_lure]);
            panel((W - ftext_w(hint, F_S, 100)) / 2 - 10, H - 62, ftext_w(hint, F_S, 100) + 20, 19);
            ftext((W - ftext_w(hint, F_S, 100)) / 2, H - 61, hint, C_WHITE, F_S, 100);
            if (pad_connected()) { static const int kk[3] = { K_LR, K_A, K_B }; const char *l[3] = { T("ESCA", "LURE"), "OK", "MENU" }; pad_hints(kk, l, 3); }
            layer_store(key);
        }
    }
    const char *labs[2] = { "MENU", "OK" };
    s_screen_btn = ui_row(labs, 2);
}

// Weigh-in: the crowd while the scale settles (a drum roll), then the verdict painting — the champion
// with his fish, or rowing home. Everything sits in a plate along the bottom and a rank badge in the
// sky, so the painting stays in view.
static void draw_weigh(int now) {
    char b[48], t[24];
    const int e = now - s_state_ms, done = e > 1700;
    const float shown = s_total * clampf(e / 1600.0f, 0, 1);
    if (done && s_total >= s_quota) {                         // qualified: the lake's own picture (gen15.py)
        char wn[8] = "win0";
        wn[3] = (char)('0' + s_stage);
        art(wn);
    } else art(done ? "lose" : s_catches > 0 ? "weigh" : "weigh0");   // nothing caught: the scale hangs empty
    if (done && e < 1780) nv_gfx_panel(0, 0, W, H, 0, C_WHITE, C_WHITE, 180);   // the flash of the reveal
    // top: what this is
    const int ok = s_total >= s_quota;
    const char *ttl = !done ? T("PESATURA", "WEIGH-IN") : ok ? T("QUALIFICATO!", "QUALIFIED!") : T("NON QUALIFICATO", "NOT QUALIFIED");
    const int tpct = done && e < 1900 ? 80 + (1900 - e) / 4 : 80;
    const int tww = ftext_w(ttl, F_L, tpct) + 40;
    panel((W - tww) / 2, 4, tww, 52);
    ftext((W - ftext_w(ttl, F_L, tpct)) / 2, 6, ttl, !done ? C_YELLOW : ok ? C_GREEN : C565(255, 110, 90), F_L, tpct);
    ftext((W - ftext_w(lake_name(s_stage), F_S, 100)) / 2, 38, lake_name(s_stage), C_CYAN, F_S, 100);
    // bottom plate: the weight counting up, the quota, the three fish that count
    panel(10, 196, W - 20, 60);
    fmt_kg(t, shown);
    ftext(22, 200, t, s_total >= s_quota && done ? C_GREEN : C_WHITE, F_L, 100);
    b[0] = 0; cat(b, T("QUOTA ", "QUOTA ")); fmt_kg(t, s_quota); cat(b, t);
    ftext(24, 238, b, C_GREY, F_S, 100);
    int order[16], n = s_catches < 16 ? s_catches : 16;
    for (int i = 0; i < n; i++) order[i] = i;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (s_list_kg[order[j]] > s_list_kg[order[i]]) { const int x = order[i]; order[i] = order[j]; order[j] = x; }
    for (int i = 0; i < 3; i++) {
        const int x = 196 + i * 100;
        nv_gfx_panel(x, 202, 94, 48, 6, C565(30, 54, 90), C565(14, 26, 50), 200);
        if (i >= n) { ftext(x + 40, 218, "-", C_GREY, F_M, 100); continue; }
        if (e < 300 + i * 350) continue;                           // they land one by one
        fish_art_kg(s_list_sp[order[i]], s_list_kg[order[i]], x + 4, 204, 54, 36);
        fmt_kg(t, s_list_kg[order[i]]);
        t[4] = 0;
        ftext(x + 58, 216, t, C_YELLOW, F_S, 100);
    }
    if (done) {
        {   // The rank as a medal (S at double the quota, A at 1.5x, B qualified, C short): gold, silver,
            // bronze or iron, two ribbon tails, a bevelled rim, a face lit from the top left, the letter
            // struck into it, and a ribbon banner with the word. It drops in, big, and settles.
            const float q = s_total / (s_quota > 0 ? s_quota : 1);
            const int rk = q >= 2.0f ? 0 : q >= 1.5f ? 1 : q >= 1.0f ? 2 : 3;
            static const char *const rname[4] = { "S", "A", "B", "C" };
            static const uint16_t hi[4] = { C565(255, 232, 120), C565(240, 244, 250), C565(246, 176, 112), C565(170, 178, 192) };
            static const uint16_t mid[4] = { C565(232, 172, 30), C565(176, 186, 204), C565(196, 110, 50), C565(110, 118, 134) };
            static const uint16_t lo[4] = { C565(140, 80, 10), C565(92, 100, 120), C565(110, 56, 22), C565(52, 58, 72) };
            static const uint16_t ink[4] = { C565(110, 60, 0), C565(60, 66, 84), C565(90, 40, 12), C565(30, 34, 44) };
            static const uint16_t rib[4] = { C565(200, 40, 40), C565(40, 90, 200), C565(30, 140, 70), C565(90, 90, 100) };
            static const uint16_t rib2[4] = { C565(120, 16, 16), C565(16, 44, 120), C565(12, 76, 36), C565(46, 46, 54) };
            const int re = e - 1700, pct = re < 220 ? 170 - re * 70 / 220 : 100;
            const int cx = W - 56, cy = 92, d = 74 * pct / 100, r = d / 2;
            // ribbon tails behind the medal
            nv_gfx_tri(cx - 20, cy + 6, cx - 4, cy + 10, cx - 26, cy + r + 30, rib[rk]);
            nv_gfx_tri(cx - 4, cy + 10, cx - 12, cy + r + 34, cx - 26, cy + r + 30, rib2[rk]);
            nv_gfx_tri(cx + 20, cy + 6, cx + 4, cy + 10, cx + 26, cy + r + 30, rib[rk]);
            nv_gfx_tri(cx + 4, cy + 10, cx + 12, cy + r + 34, cx + 26, cy + r + 30, rib2[rk]);
            // shadow, rim (dark edge, bright bevel), face
            nv_gfx_panel(cx - r + 3, cy - r + 5, d, d, r, C_SHADOW, C_SHADOW, 140);
            nv_gfx_panel(cx - r, cy - r, d, d, r, lo[rk], lo[rk], 255);
            nv_gfx_panel(cx - r + 2, cy - r + 2, d - 4, d - 4, r - 2, hi[rk], lo[rk], 255);
            nv_gfx_panel(cx - r + 7, cy - r + 7, d - 14, d - 14, r - 7, lo[rk], mid[rk], 255);       // the struck ring
            nv_gfx_panel(cx - r + 9, cy - r + 9, d - 18, d - 18, r - 9, hi[rk], mid[rk], 255);       // the face
            nv_gfx_panel(cx - r + 14, cy - r + 11, d / 2 - 6, d / 3 - 4, d / 6, C565(255, 255, 255), hi[rk], 70);   // gleam
            // the letter, struck in: a light edge below, the dark ink on top
            const int lp = pct * 120 / 100, lw = ftext_w(rname[rk], F_L, lp);
            ftext(cx - lw / 2 + 1, cy - 19 * lp / 100 + 1, rname[rk], hi[rk], F_L, lp);
            ftext(cx - lw / 2, cy - 19 * lp / 100, rname[rk], ink[rk], F_L, lp);
            if (rk == 0 && re > 220) {                         // gold: a glint running round the rim
                const float a = (now % 1600) / 1600.0f * 2 * PI_F;
                const int gx = cx + iroundf(sinf_(a) * (r - 4)), gy = cy - iroundf(cosf_(a) * (r - 4));
                nv_gfx_panel(gx - 4, gy - 1, 9, 3, 1, C_WHITE, C_WHITE, 220);
                nv_gfx_panel(gx - 1, gy - 4, 3, 9, 1, C_WHITE, C_WHITE, 220);
            }
            // the word on a little banner across the ribbons
            const char *word = T("RANGO", "RANK");
            const int ww = ftext_w(word, F_S, 100) + 14, by = cy + r + 6;
            nv_gfx_panel(cx - ww / 2, by, ww, 16, 4, rib[rk], rib2[rk], 240);
            ftext(cx - ww / 2 + 7, by + 1, word, C_WHITE, F_S, 100);
        }
        const char *labs[1] = { T("AVANTI", "NEXT") };
        s_screen_btn = ui_row(labs, 1);
        if (pad_connected()) { static const int k[1] = { K_A }; const char *l[1] = { T("AVANTI", "NEXT") }; pad_hints(k, l, 1); }
    }
}

// The end of a tournament: the run's story counting up on the left, the biggest fish of the run in
// its portrait on the right, the verdict, and RETRY / MENU.
static void draw_over(int now) {
    char b[48], t[24];
    const int e = now - s_state_ms;
    art(s_stages_cleared >= NSTAGES ? "champ" : "lose");     // every lake cleared: the champion's picture
    nv_gfx_panel(0, 0, W, H, 0, C565(4, 8, 20), C565(4, 8, 20), 90);
    const char *ttl = T("FINE TORNEO", "TOURNAMENT OVER");
    const int pct = e < 200 ? 150 - e / 4 : 100;
    ftext((W - ftext_w(ttl, F_L, pct)) / 2, 4, ttl, C_YELLOW, F_L, pct);
    panel(20, 50, W / 2 - 30, 176);
    const float k1 = clampf((e - 300) / 500.0f, 0, 1), k2 = clampf((e - 800) / 500.0f, 0, 1), k3 = clampf((e - 1300) / 900.0f, 0, 1);
    ftext(34, 58, T("TAPPE SUPERATE", "STAGES CLEARED"), C_GREY, F_S, 100);
    fmt_int(t, iroundf(s_stages_cleared * k1)); ftext(34, 70, t, C_WHITE, F_L, 80);
    ftext(34, 104, T("PESCI PRESI", "FISH LANDED"), C_GREY, F_S, 100);
    fmt_int(t, iroundf(s_run_catches * k2)); ftext(34, 116, t, C_WHITE, F_L, 80);
    ftext(34, 150, T("PESO TOTALE", "TOTAL WEIGHT"), C_GREY, F_S, 100);
    fmt_kg(t, s_run_total * k3); ftext(34, 162, t, C_YELLOW, F_L, 80);
    b[0] = 0; cat(b, T("RECORD TORNEO  ", "BEST RUN  ")); fmt_kg(t, s_best_run100 / 100.0f); cat(b, t);
    ftext(34, 204, b, C_CYAN, F_S, 100);
    panel(W / 2 + 10, 50, W / 2 - 30, 176);
    ftext(W / 2 + 24, 58, T("IL PIÙ GROSSO", "BIGGEST CATCH"), C_GREY, F_S, 100);
    if (s_run_best_kg > 0 && e > 1800) {
        const int z = e < 2100 ? (e - 1800) * 100 / 300 : 100;
        const int fw = 200 * z / 100, fh = 124 * z / 100;
        fish_art_kg(s_run_best_sp, s_run_best_kg, W * 3 / 4 - fw / 2, 124 - fh / 2, fw, fh);
        ftext(W / 2 + 24, 184, sp_name(s_run_best_sp), C_WHITE, F_S, 100);
        fmt_kg(t, s_run_best_kg); ftext(W / 2 + 24, 196, t, C_YELLOW, F_M, 100);
    } else if (s_run_best_kg <= 0) ftext(W * 3 / 4 - ftext_w(T("NESSUN PESCE", "NO FISH"), F_M, 100) / 2, 130, T("NESSUN PESCE", "NO FISH"), C_GREY, F_M, 100);
    if (e > 2300 && iroundf(s_run_total * 100) >= s_best_run100 && s_run_total > 0) {
        const char *nr = T("NUOVO RECORD DI TORNEO!", "NEW TOURNAMENT RECORD!");
        ftext((W - ftext_w(nr, F_M, 100)) / 2, 230, nr, ((e / 150) & 1) ? C_GREEN : C_WHITE, F_M, 100);
    }
    const char *labs[2] = { "MENU", T("RIPROVA", "RETRY") };
    s_screen_btn = ui_row(labs, 2);
    if (pad_connected()) { static const int k[2] = { K_A, K_B }; const char *l[2] = { T("RIPROVA", "RETRY"), "MENU" }; pad_hints(k, l, 2); }
}

// ---- the record wall's initials: three big letters, arrows to change them, the fish beside -------------
static void draw_name(int now) {
    char b[48], t[24];
    pond_art(s_stage);
    nv_gfx_panel(0, 0, W, H, 0, C565(4, 8, 20), C565(4, 8, 20), 140);
    const char *ttl = T("NUOVO RECORD!", "NEW RECORD!");
    ftext((W - ftext_w(ttl, F_L, 90)) / 2, 2, ttl, ((now / 140) & 1) ? C_YELLOW : C_WHITE, F_L, 90);
    b[0] = 0; cat(b, T("POSTO ", "RANK ")); fmt_int(t, s_new_rank + 1); cat(b, t); cat(b, "  -  "); cat(b, sp_name(s_catch_sp));
    cat(b, " "); fmt_kg(t, s_catch_kg); cat(b, t);
    ftext((W - ftext_w(b, F_S, 100)) / 2, 40, b, C_CYAN, F_S, 100);
    ftext((W - ftext_w(T("LE TUE INIZIALI", "YOUR INITIALS"), F_S, 100)) / 2, 58, T("LE TUE INIZIALI", "YOUR INITIALS"), C_WHITE, F_S, 100);
    const char *nm = s_names[s_new_rank];
    for (int k = 0; k < 3; k++) {
        const int x = W / 2 - 105 + k * 74, sel = k == s_name_slot;
        nv_gfx_tri(x + 31, 82, x + 15, 104, x + 47, 104, sel ? C_YELLOW : C565(120, 140, 170));          // up
        if (sel) card(x - 3, 113, 68, 72, ((now / 160) & 1) ? C565(255, 236, 110) : C565(255, 160, 30), C565(255, 120, 20));
        else card(x, 116, 62, 66, C565(90, 160, 220), C565(30, 60, 110));
        char c[2] = { nm[k], 0 };
        ftext(x + 31 - ftext_w(c, F_L, 130) / 2, 122, c, sel ? C_YELLOW : C_WHITE, F_L, 130);
        nv_gfx_tri(x + 31, 214, x + 15, 192, x + 47, 192, sel ? C_YELLOW : C565(120, 140, 170));         // down
    }
    if (!pad_connected()) ui_btn(kNameOk.x, kNameOk.y, kNameOk.w, kNameOk.h, "OK", C565(70, 220, 110));
    else { static const int k[3] = { K_UD, K_LR, K_A }; const char *l[3] = { T("LETTERA", "LETTER"), T("POSIZIONE", "MOVE"), "OK" }; pad_hints(k, l, 3); }
}

// ---- attract-mode intro (1990s arcade style) ------------------------------------------------------------------
// Painted scenes with camera moves (pan, zoom, shake), letterbox bars, venetian-blind wipes, white
// flashes and typewriter captions, over an ACE-Step theme; then the title logo slams in.
typedef struct { const char *img; int t0, t1, mode; const char *it, *en; } Scene;
static const Scene kIntro[] = {
    { "intro0",  2200,  6000, 0, "ALL'ALBA, SUL LAGO...", "AT DAWN, ON THE LAKE..." },
    { "intro1",  6000,  9600, 1, "IL BASS PIÙ GROSSO TI ASPETTA", "THE BIGGEST BASS IS WAITING" },
    { "intro2",  9600, 13200, 2, "FERRA AL MOMENTO GIUSTO!", "SET THE HOOK AT THE RIGHT MOMENT!" },
    { "intro3", 13200, 17600, 3, "DIVENTA IL RE DEL LAGO!", "BECOME THE KING OF THE LAKE!" },
};
#define INTRO_END 17800
static void draw_intro(int now) {
    const int t = now - s_state_ms;
    nv_gfx_rect(0, 0, W, H, C565(0, 0, 0));
    if (t < 2200) {                                        // "VERTICE" drops in letter by letter
        static const char word[] = "VERTICE";
        int total = 0;                                     // set by the letters' own widths (I is narrow)
        for (int i = 0; i < 7; i++) { char c[2] = { word[i], 0 }; total += tw(c, 6) + 4; }
        int x = W / 2 - (total - 4) / 2;
        for (int i = 0; i < 7; i++) {
            char c[2] = { word[i], 0 };
            const int li = t - i * 110, cw = tw(c, 6);
            if (li >= 0) {
                const int y = li < 300 ? -40 + li * 140 / 300 : 100;
                text_sh(x, y, c, li < 350 ? C_WHITE : C_CYAN, 6);
            }
            x += cw + 4;
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
        else if (s->mode == 1) { const float z = 1.0f + 0.15f * u; w = iroundf(W * z); h = iroundf(H * z); x = (W - w) / 2; y = (H - h) / 2; }   // a slow push: the fish stays whole
        else if (s->mode == 2) { w = W * 27 / 25; h = H * 27 / 25; x = (W - w) / 2 + rnd(7) - 3; y = (H - h) / 2 + rnd(5) - 2; }   // a light shake
        else { const float z = 1.3f - 0.3f * u; w = iroundf(W * z); h = iroundf(H * z); x = (W - w) / 2; y = -iroundf((h - H) * 0.15f); }   // pulls back from the trophy, which stays whole
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
        text_sh((W - tw(cap, 2)) / 2, H - 26, buf, C_YELLOW, 2);
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
    const int mx = 40, my = 120, mr = 32;                                // under the livewell
    const float k = mr / (4100.0f * LAKE_K);
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
static void select_card(int k, int sel, int now) {
    char b[40], t[20];
    {
        const int x = 12 + (k % 3) * 166, y = 38 + (k / 3) * 104, w = 156, h = 96;
        const int open = k <= s_unlocked;
        if (sel) card(x - 3, y - 3, w + 6, h + 6, ((now / 160) & 1) ? C565(255, 236, 110) : C565(255, 160, 30), C565(255, 120, 20));
        else card(x, y, w, h, open ? C565(90, 160, 220) : C565(70, 76, 96), open ? C565(30, 60, 110) : C565(30, 32, 44));
        char n[8] = "lk0";                                         // the card-size thumbnail (art: lake<N>)
        n[2] = (char)('0' + k);
        nv_gfx_image(n, x + 5, y + 5, w - 10, 58);
        if (!open) {                                               // locked: the painting under smoked glass and a padlock
            nv_gfx_panel(x + 5, y + 5, w - 10, 58, 0, C565(8, 10, 20), C565(8, 10, 20), 170);
            nv_gfx_panel(x + w / 2 - 9, y + 13, 18, 20, 9, C565(200, 205, 220), C565(120, 126, 140), 255);
            nv_gfx_panel(x + w / 2 - 5, y + 17, 10, 14, 5, C565(8, 10, 20), C565(8, 10, 20), 255);
            nv_gfx_panel(x + w / 2 - 13, y + 26, 26, 22, 4, C565(230, 190, 70), C565(170, 120, 30), 255);
            nv_gfx_panel(x + w / 2 - 2, y + 32, 4, 9, 2, C565(60, 40, 10), C565(60, 40, 10), 255);
        }
        fmt_int(t, k + 1); b[0] = 0; cat(b, t); cat(b, ". "); cat(b, lake_name(k));
        if (fit_pct(b, F_S, 100, w - 14) < 100) { b[0] = 0; cat(b, lake_name(k)); }   // long name: no number
        ftext(x + 7, y + 64, b, open ? (sel ? C_YELLOW : C_WHITE) : C_GREY, F_S, fit_pct(b, F_S, 100, w - 14));
        if (open) { b[0] = 0; cat(b, T("QUOTA ", "QUOTA ")); fmt_kg(t, g_stage[k].quota_kg); cat(b, t); }
        ftext(x + 7, y + 78, open ? b : T("BLOCCATO", "LOCKED"), open ? C_CYAN : C565(255, 110, 90), F_S, 100);
    }
}
static void draw_select(int now) {
    const uint32_t key = layer_key(ST_SELECT, s_sel, s_unlocked);
    if (!layer_cached(key)) {                     // the painting, veil, title and the other cards: once
        lake_art(s_sel);
        nv_gfx_panel(0, 0, W, H, 0, C565(4, 10, 24), C565(4, 10, 24), 110);      // dim the backdrop
        ftext((W - ftext_w(T("SCEGLI IL LAGO", "CHOOSE A LAKE"), F_L, 80)) / 2, 2, T("SCEGLI IL LAGO", "CHOOSE A LAKE"), C_YELLOW, F_L, 80);
        for (int k = 0; k < NSTAGES; k++) if (k != s_sel) select_card(k, 0, now);
        if (pad_connected()) { static const int k[3] = { K_DPAD, K_A, K_B }; const char *l[3] = { T("SCEGLI", "CHOOSE"), T("VIA!", "GO!"), "MENU" }; pad_hints(k, l, 3); }
        layer_store(key);
    }
    select_card(s_sel, 1, now);                   // the chosen one blinks: every frame
    const char *labs[2] = { "MENU", T("VIA!", "GO!") };
    s_screen_btn = ui_row(labs, 2);
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
        text_c(10, s_junk == 3 ? T("TESORO!", "TREASURE!") : T("PULIZIA!", "CLEAN UP!"), ((t / 120) & 1) ? C_YELLOW : C_WHITE, t < 180 ? 7 : 5);
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
            nv_gfx_tri(W / 2, 108, W / 2 + iroundf(cosf_(a0) * 420), 108 + iroundf(sinf_(a0) * 420),
                       W / 2 + iroundf(cosf_(a1) * 420), 108 + iroundf(sinf_(a1) * 420), cls == 3 ? C565(255, 214, 90) : C565(250, 236, 170));
        }
    }
    // The fish bursts in at its size class and breathes; the class slams down over it.
    const int fw = 160 + cls * 40, fh = fw * 2 / 3;
    const float z = t < 240 ? t / 240.0f * 1.12f : t < 380 ? 1.12f - (t - 240) / 140.0f * 0.12f : 1.0f + sinf_(t * 0.005f) * 0.025f;
    const int w = iroundf(fw * z), h = iroundf(fh * z);
    fish_art_kg(s_catch_sp, s_catch_kg, W / 2 - w / 2, 106 - h / 2, w, h);
    const int wc = weight_class(s_catch_sp, s_catch_kg);
    const int pct = wc <= CL_SMALL ? 85 : (t < 200 ? 170 - t * 60 / 200 : 110);
    const char *ttl = class_name(wc);
    ftext((W - ftext_w(ttl, F_L, pct)) / 2, 2, ttl, cls >= 2 && ((t / 100) & 1) ? C_YELLOW : C_WHITE, F_L, pct);
    if (s_qual_now && t > 1100) {                          // the quota is made
        const char *q = T("QUOTA RAGGIUNTA!", "QUOTA REACHED!");
        const int qw = ftext_w(q, F_M, 100) + 24;
        nv_gfx_panel((W - qw) / 2, 172, qw, 24, 12, C565(60, 200, 100), C565(20, 110, 50), 235);
        ftext((W - qw) / 2 + 12, 173, q, ((t / 120) & 1) ? C_WHITE : C_YELLOW, F_M, 100);
    }
    // The plate: species and weight rolling up on the left; time bonus, streak, hook-set and the
    // livewell verdict on the right; the stage total filling toward the quota along the bottom.
    panel(16, 202, W - 32, 92);
    ftext(30, 208, sp_name(s_catch_sp), s_catch_sp == SP_GOLD ? C_YELLOW : C_WHITE, F_M, 100);
    fmt_kg(s, s_catch_kg * clampf((t - 250) / 800.0f, 0, 1));
    ftext(28, 228, s, C_YELLOW, F_L, 110);
    int ly = 208;
    const int rx = W / 2 + 60;
    b[0] = 0; cat(b, T("TEMPO +", "TIME +")); fmt_int(s, s_tb); cat(b, s); cat(b, " S");
    ftext(rx, ly, b, C_GREEN, F_S, 100); ly += 15;
    if (s_combo >= 2) { b[0] = 0; cat(b, "COMBO X"); fmt_int(s, s_combo); cat(b, s); ftext(rx, ly, b, C565(255, 160, 50), F_S, 100); ly += 15; }
    if (s_perfect) { ftext(rx, ly, T("FERRATA PERFETTA", "PERFECT HOOK-SET"), C_CYAN, F_S, 100); ly += 15; }
    ftext(rx, ly, well_keeps(s_catch_kg) ? T("NEL VIVAIO", "INTO THE LIVEWELL") : T("RILASCIATO", "RELEASED"),
          well_keeps(s_catch_kg) ? C_GREEN : C_GREY, F_S, 100);
    if (s_new_rank >= 3) {                                 // on the record wall
        b[0] = 0; cat(b, T("RECORD N.", "RECORD #")); fmt_int(s, s_new_rank + 1); cat(b, s);
        const int bw = ftext_w(b, F_S, 100) + 18;
        nv_gfx_panel(W - bw - 10, 40, bw, 18, 9, C565(255, 190, 50), C565(190, 90, 10), 240);
        ftext(W - bw - 1, 41, b, C_WHITE, F_S, 100);              // outlined glyphs: light ink only
    }
    nv_gfx_panel(28, 280, W - 56, 8, 4, C565(20, 30, 46), C565(20, 30, 46), 230);
    {
        const int fill = iroundf(clampf(s_total / (s_quota > 0 ? s_quota : 1), 0, 1) * (W - 60) * clampf((t - 400) / 600.0f, 0, 1));
        if (fill > 8) nv_gfx_panel(30, 281, fill, 6, 3, s_total >= s_quota ? C565(120, 255, 150) : C565(255, 230, 90),
                                   s_total >= s_quota ? C565(30, 160, 70) : C565(220, 140, 20), 255);
    }
    if (pad_connected() && t > 900) { key_badge(W - 110, 6, K_A); ftext(W - 110 + key_w(K_A) + 5, 6, T("AVANTI", "NEXT"), C_WHITE, F_S, 100); }
    if (t < 70) nv_gfx_panel(0, 0, W, H, 0, C_WHITE, C_WHITE, 220);
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
    int lx = (W - tw(title, 4)) / 2;
    for (int i = 0; i < n; i++) {
        char c[2] = { title[i], 0 };
        const int y = 12 + iroundf(sinf_(t * 0.012f - i * 0.5f) * 5);
        text_sh(lx, y, c, ((t / 120 + i) % 3) ? C_WHITE : C_YELLOW, 4);
        lx += tw(c, 4);
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
        cat(r, ord_sfx(rk));
        text_sh(54 - tw(r, 4) / 2, 150, r, C_WHITE, 4);
        text_sh(W - 54 - tw(T("POSTO", "PLACE"), 2) / 2, 156, T("POSTO", "PLACE"), C_WHITE, 2);
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
        text_sh(W / 2 + 160 - tw(tb, 2), 246, tb, C_GREEN, 2);
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

// ---- the sound of the scene, every frame: music and ambience by screen, the gear's loops following
// the reel, the drag, the line and the outboard (the OS mixes them; see sfx above).
static void sound_frame(int now) {
    const int play = s_state == ST_AIM || s_state == ST_CAST || s_state == ST_RETRIEVE || s_state == ST_STRIKE ||
                     s_state == ST_FIGHT || s_state == ST_LOST;
    const int under = (s_state == ST_RETRIEVE || s_state == ST_STRIKE || s_state == ST_FIGHT) && !s_air;
    static char pm[8] = "play0";
    pm[4] = (char)('0' + s_stage);
    if (s_paused) {
        loop_set(&s_vreel, 0, 0, 0); loop_set(&s_vdrag, 0, 0, 0); loop_set(&s_vcreak, 0, 0, 0); loop_set(&s_vmotor, 0, 0, 0);
        if (s_vmus >= 0) nv_snd_set(s_vmus, 70, -1);
        return;
    }
    if (s_state == ST_FIGHT) music2("fight", pm, 210);       // FISH ON: the music kicks in
    else if (s_state == ST_STAGE || s_state == ST_LURE) music(pm, 220);
    else if (play) music(pm, under ? 110 : 150);         // the lake's theme while you fish, softer below
    else if (s_state == ST_CATCH) music(pm, 70);
    else if (s_state == ST_WEIGH) music(0, 0);
    else if (s_state == ST_OVER) { if (s_stages_cleared >= NSTAGES) music("champ", 230); else music("menu2", 200); }
    else if (s_state == ST_NAME) music("menu2", 160);
    ambience(play ? (under ? "amb_down" : "amb_up") : 0, under ? 210 : 170);
    const int reeling = play && now - s_reel_at < 140;
    loop_set(&s_vreel, "reel_loop", reeling ? 200 : 0, 150 + iroundf(s_reel_amt * 170));
    const int fight = s_state == ST_FIGHT;
    loop_set(&s_vdrag, "drag_loop", fight && s_fight.drag ? 230 : 0, 210 + iroundf(s_fight.tension * 90));
    const float tn = fight ? (s_fight.tension - 0.78f) / 0.22f : 0;
    loop_set(&s_vcreak, "creak_loop", tn > 0 ? iroundf(clampf(tn, 0, 1) * 300) : 0, 230 + iroundf(clampf(tn, 0, 1) * 80));
    const float sp = fabsf_(s_bspeed) / 460.0f;
    loop_set(&s_vmotor, "motor", s_state == ST_AIM && s_engine == 2 ? 110 + iroundf(sp * 140) : 0, 190 + iroundf(sp * 190));
}

// ---- the game loop -----------------------------------------------------------------------------------------------------------
NV_EXPORT("run")
// The continuous layer, state by state.
static void haptics_frame(int now, float dt) {
    if (!s_have_pad) return;
    float lo = 0, hi = 0;
    static int prev_state = -1, prev_air, prev_bed, tick_at, beats, beat_at;
    static float phase;
    int force = 0;
    const int entered = s_state != prev_state, t = now - s_state_ms;
    switch (s_state) {
    case ST_AIM:                                              // the outboard: a hum that rises with the speed
        if (s_engine == 2) {
            const float sp = clampf(fabsf_(s_bspeed) / BOAT_TOP, 0, 1);
            lo = 2500 + sp * 11000 + sinf_(now * 0.09f) * 900;
            hi = 1200 + sp * 3500;
        }
        break;
    case ST_CAST:
        if (entered) rumble(5000, 26000, 70);                 // the rod whips forward
        break;
    case ST_RETRIEVE:
        if (s_reel_amt > 0.05f && now - tick_at > (int)(150 / (0.35f + s_reel_amt))) {   // the reel's clicks
            tick_at = now;
            rumble(0, 5000 + iroundf(s_reel_amt * 7000), 26);
        }
        if (s_on_bed && !prev_bed) rumble(11000, 4000, 70);   // the lure knocks on the bottom
        break;
    case ST_FIGHT: {
        const float ten = clampf(s_fight.tension, 0, 1);
        const float effort = clampf(s_fight.run, 0, 1) * (0.3f + 0.7f * s_fight.stamina);
        if (s_air) break;                                     // in the air the line goes light...
        if (prev_air) { rumble(36000, 22000, 150); break; }   // ...and it slams back into the water
        lo = ten * (7000 + 24000 * effort);                   // the weight of the fish
        phase += dt * (2.5f + 7.0f * effort);                 // head shakes, one knock per tail beat
        if (phase >= 1.0f) { phase -= 1.0f; beat_at = now; force = 1; }
        if (now - beat_at < 60) hi = 4000 + 30000 * effort;
        if (s_fight.drag && now - tick_at > 55) { tick_at = now; hi += 22000; }   // the drag's ratchet
        if (ten > 0.85f) hi += (ten - 0.85f) / 0.15f * 22000;  // the red: the line hums
        if (s_fight.jumping) hi += 8000;
        break;
    }
    case ST_CATCH:                                            // landed: three beats
        if (entered) beats = 0;
        if (beats < 3 && t >= beats * 230) { rumble(beats == 2 ? 42000 : 30000, beats == 2 ? 52000 : 30000, beats == 2 ? 200 : 110); beats++; }
        break;
    default: break;
    }
    rumble_bg(lo, hi, force);
    prev_state = s_state; prev_air = s_air; prev_bed = s_on_bed;
}

void run(void) {
    char lang[8] = "";
    nv_lang(lang, sizeof lang);
    static const char k_codes[5][3] = { "it", "en", "es", "fr", "de" };
    s_lang = L_EN;
    for (int i = 0; i < 5; i++) if (lang[0] == k_codes[i][0] && lang[1] == k_codes[i][1]) s_lang = i;
    records_load();
    if (nv_load("unlock.bin", &s_unlocked, 4) != 4 || s_unlocked < 0 || s_unlocked >= NSTAGES) s_unlocked = 0;
#ifdef BASS_TEST_UNLOCK
    s_unlocked = NSTAGES - 1;
#endif
    s_stage = 0; s_loop = 0;
    lake_build(0, 0); fish_build(); build_lures(); lake_view(0);
#ifdef BASS_CFG_MIP                  // perf experiments (autoplay builds): engine quality settings
    vx_config(VX_CFG_MIP_BIAS, BASS_CFG_MIP);
#endif
#ifdef BASS_CFG_NOTEX
    vx_config(VX_CFG_NO_TEXTURES, 1);
#endif
#ifdef BASS_CFG_EAGER
    vx_config(VX_CFG_EAGER_BG, 1);
#endif
    int last = nv_millis();
    s_state_ms = last;
    music("intro", 256);
    s_state = ST_INTRO;
#ifdef BASS_TEST_WAKE    // simulator only: straight into the boat, backing up (the wake, the angler on top)
    s_state = ST_AIM; s_state_ms = last; lake_view(0); s_time_ms = 120000; s_quota = 2.4f;
#endif
#ifdef BASS_TEST_CATCH   // simulator only: open straight on a catch (1 fish, 2 podium, 3 junk)
    s_catch_sp = BASS_TEST_SP; s_catch_kg = BASS_TEST_KG; s_tb = 12; s_combo = 3; s_perfect = 1; s_total = 3.9f; s_quota = 2.4f;
    s_qual_now = 1; s_list_sp[0] = (uint8_t)s_catch_sp; s_list_kg[0] = s_catch_kg; s_catches = 1;
    s_new_rank = BASS_TEST_CATCH == 2 ? 1 : 5; s_junk = BASS_TEST_CATCH == 3 ? 1 : -1;
    s_state = ST_CATCH;
    if (BASS_TEST_CATCH == 5) {           // the weigh-in, qualified
        s_list_sp[1] = SP_PIKE; s_list_kg[1] = 2.1f; s_list_sp[2] = SP_TROUT; s_list_kg[2] = 1.2f; s_catches = 3; s_total = 6.5f;
        s_state = ST_WEIGH;
    }
    if (BASS_TEST_CATCH == 8) { s_catches = 0; s_total = 0; s_state = ST_WEIGH; }   // the weigh-in, not qualified
    if (BASS_TEST_CATCH == 6) s_state = ST_RECORDS;
    if (BASS_TEST_CATCH == 7) { s_new_rank = 2; s_name_slot = 1; s_state = ST_NAME; }
    if (BASS_TEST_CATCH == 4) {           // the tournament-over page
        s_run_catches = 7; s_stages_cleared = 2; s_run_total = 9.4f; s_run_best_kg = s_catch_kg; s_run_best_sp = s_catch_sp;
        s_state = ST_OVER;
    }
#endif
#ifdef BASS_TEST_VIEW    // simulator only: from the boat, turning round, on lake BASS_TEST_VIEW - 1
    lake_build(BASS_TEST_VIEW - 1, 0); fish_build(); build_lures(); lake_view(0);
    for (int f = 0; nv_gfx_present(); f++) {
        s_aim = f * 0.0785f;                               // a full turn in 80 frames
        s_acam_ok = 0; aim_camera();
        vx_render();
#ifdef BASS_TEST_PROJ        // the engine's vx_project against the game's own project(), a few points
        {
            static const int pts[4][3] = { { 0, 100, 1500 }, { 800, 0, 3000 }, { -600, 300, 900 }, { 0, 0, 400 } };
            for (int k = 0; k < 4; k++) {
                int32_t o[3]; int gx = -9999, gy = -9999;
                const int a = vx_project(pts[k][0], pts[k][1], pts[k][2], o), b = project(pts[k][0], pts[k][1], pts[k][2], &gx, &gy);
                char t[96] = "proj ", n[16];
                fmt_int(n, a); cat(t, n); cat(t, " "); fmt_int(n, a ? o[0] : 0); cat(t, n); cat(t, ","); fmt_int(n, a ? o[1] : 0); cat(t, n);
                cat(t, " game "); fmt_int(n, b); cat(t, n); cat(t, " "); fmt_int(n, gx); cat(t, n); cat(t, ","); fmt_int(n, gy); cat(t, n);
                if (f == 20) nv_log(NV_LOG_INFO, t);
            }
        }
#endif
    }
    return;
#endif
#ifdef BASS_TEST_LOG     // simulator only: the floating dead tree, the camera going round it
    lake_view(0);
    {
        int k = 0;
        while (k < NSPOTS && g_spot[k].kind != SPOT_LOG) k++;
        const float lx = g_spot[k].x, lz = g_spot[k].z;
        for (int f = 0; nv_gfx_present(); f++) {
            const float a = f * 0.04f, d = BASS_TEST_LOG;
            cam(lx + sinf_(a) * d, 40 + d * 0.3f, lz + cosf_(a) * d, lx, 10, lz, 55);
            vx_render();
        }
    }
    return;
#endif
#ifdef BASS_TEST_JUNK    // simulator only: the junk on the bed, the camera low over it
    lake_view(1);
    for (int f = 0; nv_gfx_present(); f++) {
        cam(sinf_(f * 0.03f) * 120, 70, 1250, 0, 15, 1500, 60);
        vx_render();
    }
    return;
#endif
#ifdef BASS_TEST_FISH    // simulator only: the eight species side by side under water, turning
    lake_view(1);
    fish_spawn(0, 1500, 0);
    for (int f = 0; nv_gfx_present(); f++) {
        cam(0, 210, 880, 0, 200, 1500, 62);
        for (int sp = 0; sp < NSPECIES; sp++) {
            const int i = sp * 3;
            fish_pose_test(i, (sp % 5 - 2.0f) * 125, 140 + (sp / 5) * 110, 1500, f * 0.05f + sp * 0.4f);
        }
        vx_render();
    }
    return;
#endif
    int music_at = last;
    while (nv_gfx_present()) {
        const int now = nv_millis();
        float dt = (now - last) / 1000.0f;
        last = now;
        if (dt > 0.05f) dt = 0.05f;
        s_shake *= 1.0f - clampf(dt * 7, 0, 1);
        read_input();
#ifdef BASS_TEST_WAKE
        if (s_state == ST_AIM) { s_in.up = 0; s_in.down = (now - s_state_ms) % 9000 < 6000; s_in.left = (now - s_state_ms) > 3000 && (now - s_state_ms) < 4500; }
#endif
        haptics_frame(now, dt);
        {   // frame meter in the log (GET /api/logs): fps, 3D render time, what is on screen
            static int pf_at, pf_n, pf_vx;
            pf_n++; pf_vx += vx_stat(VX_STAT_US);
            if (!pf_at) pf_at = now;
            if (now - pf_at >= 3000) {
                char b[96], t[12];
                b[0] = 0; cat(b, "bass: perf st "); fmt_int(t, s_state); cat(b, t);
                cat(b, " fps10 "); fmt_int(t, pf_n * 10000 / (now - pf_at)); cat(b, t);
                cat(b, " vx_us "); fmt_int(t, pf_vx / pf_n); cat(b, t);
                cat(b, " tris "); fmt_int(t, vx_stat(VX_STAT_TRIS)); cat(b, t);
                nv_log(NV_LOG_WARN, b);
                pf_at = now; pf_n = 0; pf_vx = 0;
            }
        }
        const int in_play = s_state == ST_AIM || s_state == ST_CAST || s_state == ST_RETRIEVE || s_state == ST_STRIKE ||
                            s_state == ST_FIGHT || s_state == ST_LOST;
        // Pause: the "II" key in play, START/SELECT on a pad. Resume, back to the menu, or quit.
        if (in_play && !s_paused && ((s_in.tap && in_rect(&kPause, s_in.tx, s_in.ty)) || pressed(NV_PAD_START | NV_PAD_SELECT))) {
            s_paused = 1; s_pause_sel = 0; s_in.tap = 0;
        }
        if (s_paused) {
            sound_frame(now);
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
        if ((s_state == ST_TITLE || s_state == ST_RECORDS || s_state == ST_SELECT) && now - music_at > 200) { music_at = now; music("menu2", 230); }
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
                s_logo_at = now; s_menu = 0; music_at = now; music("menu2", 230);
                go(ST_TITLE, now);
            }
            break;
        case ST_TITLE: {
            if (now - s_state_ms > 45000) { music("intro", 256); go(ST_INTRO, now); break; }   // attract mode
            s_orbit += dt * 0.12f;
            if (pressed(NV_PAD_UP)) { s_menu = (s_menu + 2) % 3; snd_click(); }
            if (pressed(NV_PAD_DOWN)) { s_menu = (s_menu + 1) % 3; snd_click(); }
            int pick = -1;
            if (s_in.tap) for (int i = 0; i < 3; i++) if (in_rect(&kTitleItem[i], s_in.tx, s_in.ty)) { s_menu = i; pick = i; }
            if (pressed(NV_PAD_A | NV_PAD_START)) pick = s_menu;
            if (pressed(NV_PAD_SELECT) || pick == 2) return;
            if ((s_in.tap || (s_pad & ~(NV_PAD_KEYBOARD | NV_PAD_GAMEPAD))) && now - s_state_ms > 1000) s_state_ms = now - 1000;   // activity delays attract
            if (pick == 0) { s_sel = s_unlocked; s_new_rank = -1; snd_click(); go(ST_SELECT, now); }
            if (pick == 1) { s_new_rank = -1; snd_click(); go(ST_RECORDS, now); }
            break;
        }
        case ST_SELECT: {
            int mv = 0, start = s_screen_btn == 1 || pressed(NV_PAD_A | NV_PAD_START);
            if (pressed(NV_PAD_LEFT)) mv = -1;
            if (pressed(NV_PAD_RIGHT)) mv = 1;
            if (pressed(NV_PAD_UP)) mv = -3;
            if (pressed(NV_PAD_DOWN)) mv = 3;
            if (mv) { s_sel = (s_sel + mv + NSTAGES) % NSTAGES; snd_click(); }
            if (s_in.tap && s_in.ty > 36 && s_in.ty < 246) {   // tap a card to pick it, again to go
                const int k = (int)clampf((s_in.tx - 10) / 166.0f, 0, 2) + (s_in.ty >= 140 ? 3 : 0);
                if (k == s_sel) start = 1; else { s_sel = k; snd_click(); }
            }
            if (s_screen_btn == 0 || pressed(NV_PAD_B)) { s_screen_btn = -1; s_logo_at = now; go(ST_TITLE, now); break; }
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
            if (now - s_state_ms > 400 && (s_screen_btn == 1 || pressed(NV_PAD_A | NV_PAD_START))) { snd_click(); go(ST_LURE, now); }
            if (s_screen_btn == 0 || pressed(NV_PAD_B)) { lake_build(0, 0); fish_build(); build_lures(); lake_view(0); s_logo_at = now; go(ST_TITLE, now); }
            s_screen_btn = -1;
            break;
        case ST_LURE:
            aim_camera();
            if (pressed(NV_PAD_LEFT)) { s_lure = (s_lure + NLURES - 1) % NLURES; snd_click(); }
            if (pressed(NV_PAD_RIGHT)) { s_lure = (s_lure + 1) % NLURES; snd_click(); }
            if (s_screen_btn == 0 || pressed(NV_PAD_B)) { lake_build(0, 0); fish_build(); build_lures(); lake_view(0); s_logo_at = now; s_screen_btn = -1; go(ST_TITLE, now); break; }
            if (s_screen_btn == 1 && now - s_state_ms > 250) { s_screen_btn = -1; snd_click(); to_aim(now); break; }
            s_screen_btn = -1;
            if (s_in.tap && s_in.ty > 36 && s_in.ty < 240) {        // tap a lure to pick it, again to go
                const int k = (int)clampf((s_in.tx - 8) / 124.0f, 0, NLURES - 1);
                if (k == s_lure && now - s_state_ms > 250) { snd_click(); to_aim(now); break; }
                s_lure = k; snd_click();
            } else if (pressed(NV_PAD_A | NV_PAD_START) && now - s_state_ms > 250) { snd_click(); to_aim(now); }
            break;
        case ST_AIM: {
            boat_drive(dt, now);
            // Change the lure (line in): B steps through them, or tap the lure box to open the strip
            // and tap one. The strip shows for a moment after each change.
            if (s_in.b_hit) { s_lure = (s_lure + 1) % NLURES; snd_click(); s_lure_pop = now + 1300; }
            if (s_in.tap && s_in.tx > W - 150 && s_in.ty < 54) { s_lure_open = !s_lure_open; snd_click(); s_in.tap = 0; }
            if (s_lure_open && s_in.tap) {
                for (int k = 0; k < NLURES; k++)
                    if (s_in.tx >= W - 4 - (NLURES - k) * 46 && s_in.tx < W - 4 - (NLURES - k - 1) * 46 && s_in.ty >= 56 && s_in.ty < 104) {
                        s_lure = k; s_lure_open = 0; s_lure_pop = now + 700; snd_click(); s_in.tap = 0;
                    }
            }
            aim_camera();
            lake_birds(now);
            angler_pose(s_charging && s_in.a ? 1 : 0);       // the angler on the bow: winding up while charging
            if (s_pose < 0) {
                if (s_charging && s_in.a) rod_seek(W - 40 - 20 * s_power, 26 + 10 * s_power, 26, 14, dt);   // wound back
                else rod_seek(W - 170, 96, 0, 8, dt);
            }
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
                s_bspeed = 0; s_bvx = s_bvz = 0; s_bturn = 0;
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
            angler_pose(s_release_t < 0.45f ? 2 : 3);        // the cast, then ready to reel
            if (s_pose < 0) {
                if (s_release_t < 0.16f) rod_seek(W / 2 + 70, 150, -30, 30, dt);    // the whip
                else rod_seek(W - 190, 104, 8, 6, dt);                               // follow-through
            }
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
                    sfxv("bubbles", 190, 256);                   // the camera goes in with the lure
                    fish_spawn(s_tx, s_tz, s_stage);
                    s_lx = s_tx; s_lz = s_tz; s_ly = SURF - 8; s_twitch_t = 0; s_twitches = 0;
                    {
                        float an[3];
                        rope_anchor(an);
                        const float ex = s_lx - an[0], ey = s_ly - an[1], ez = s_lz - an[2];
                        s_slack = 60;                                       // a little slack from the cast
                        s_line = sqrtf_(ex * ex + ey * ey + ez * ez) + s_slack;
                        s_on_bed = 0;
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
            s_twitch_t -= dt;
            const float d = sqrtf_((s_lx - s_bx) * (s_lx - s_bx) + (s_lz - s_bz) * (s_lz - s_bz)) + 1e-3f;
            const float ux = (s_lx - s_bx) / d, uz = (s_lz - s_bz) / d;
            float speed = 0;
            if (s_state == ST_RETRIEVE) {
                speed = lure_physics(reel, ramt, pay, dt, now, ux, uz);
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
            float wx, wy, wz;
            int watch = s_state == ST_RETRIEVE ? fish_watch(s_lx, s_ly, s_lz, &wx, &wy, &wz) : -1;
            if (s_state == ST_STRIKE && s_junk < 0) {          // the strike: its face, rushing at the lens
                watch = s_strike_fish; wx = fish_mark_x(watch); wy = s_ly; wz = fish_mark_z(watch);
            }
            static int shot = -1, shot_at, prev_watch = -1, prev_st = -1;
            {   // Calm direction: the fish on screen changes only when the new state has lasted a moment
                // (fish drifting in and out of interest made the view jump to and fro), never on the strike.
                static int cur = -1, cand = -1, cand_at;
                if (watch == cur || s_state == ST_STRIKE) { cur = watch; cand = watch; }
                else {
                    if (watch != cand) { cand = watch; cand_at = now; }
                    if (now - cand_at > (cur < 0 ? 450 : 900)) cur = watch;
                }
                if (cur != watch && cur >= 0) { watch = cur; wx = fish_mark_x(cur); wy = s_ly; wz = fish_mark_z(cur); }
                else watch = cur;
            }
            if (watch >= 0) {
                // The bite, cut like Fisherman's Bait: a fish following is seen from behind it, the
                // lure ahead; once it mouths the lure the view cuts between its profile, its face
                // behind the lure, and the view from below - each shot held long enough to read
                // (about 2.4 s), drifting a little. A peck and back-off doesn't end the bite's shots.
                static int bite_until;
                if (fish_nibbling() == watch) bite_until = now + 1000;
                const int mouthing = now < bite_until, striking = s_state == ST_STRIKE;
                int cut = watch != prev_watch;
                if (striking) { if (prev_st != ST_STRIKE) { shot = 1; cut = 1; } }
                else if (!mouthing) { if (shot != 2) cut = 1; shot = 2; }
                else if (cut || (shot == 2 && prev_watch == watch && now - shot_at > 900)) { shot = 0; cut = 1; }
                else if (now - shot_at > 2400) { shot = shot == 0 ? 1 : shot == 1 ? 3 : 0; cut = 1; }
                if (cut) shot_at = now;
                const float t = (now - shot_at) / 1000.0f;
                float ufx = s_lx - wx, ufz = s_lz - wz;
                const float ul = sqrtf_(ufx * ufx + ufz * ufz);
                if (ul > 5) { ufx /= ul; ufz /= ul; } else { ufx = ux; ufz = uz; }
                const float sx = ufz, sz = -ufx;                  // the side of the pair
                const float mx = (wx + s_lx) / 2, my = (wy + s_ly) / 2, mz = (wz + s_lz) / 2;
                float p[3], q[3];
                if (shot == 0) {          // profile, close, sliding along
                    p[0] = mx + sx * 150 + ufx * (t * 30 - 20); p[1] = my + 14; p[2] = mz + sz * 150 + ufz * (t * 30 - 20);
                    q[0] = mx; q[1] = my; q[2] = mz;
                } else if (shot == 1) {   // its face behind the lure, pushing in (fast on the strike)
                    const float k = striking ? 190 - clampf(t, 0, 0.8f) * 80 : 175 - t * 25;   // the lure stays small, aside
                    p[0] = s_lx + ufx * k + sx * 70; p[1] = s_ly + 12; p[2] = s_lz + ufz * k + sz * 70;
                    q[0] = wx; q[1] = wy + 4; q[2] = wz;
                } else if (shot == 3) {   // from below, looking up at them against the surface
                    p[0] = mx - sx * 170 - ufx * 40; p[1] = my - 85; p[2] = mz - sz * 170 - ufz * 40;
                    q[0] = mx; q[1] = my + 10; q[2] = mz;
                } else {                  // the chase: low behind the fish, the lure ahead
                    p[0] = wx - ufx * 150 + sx * 45; p[1] = wy + 22 + t * 6; p[2] = wz - ufz * 150 + sz * 45;
                    q[0] = s_lx; q[1] = s_ly; q[2] = s_lz;
                }
                p[1] = clampf(p[1], 22, SURF - 40);
                const float k = cut ? 1.0f : clampf(dt * 8, 0, 1);
                for (int j = 0; j < 3; j++) { s_rcp[j] += (p[j] - s_rcp[j]) * k; s_rct[j] += (q[j] - s_rct[j]) * k; }
                cam(s_rcp[0], s_rcp[1], s_rcp[2], s_rct[0], s_rct[1], s_rct[2], 56);
                lake_clear_near((s_rcp[0] + mx) / 2, (s_rcp[2] + mz) / 2, 200);
            }
            prev_watch = watch; prev_st = s_state;
            if (watch < 0) {
                // Close on the lure (it fills the lower middle of the view, the fish coming at it from
                // the far side face the lens), eased so hops and darts don't jerk the picture.
                const float back = d > 200 ? 112.0f : d - 88.0f;    // close on the (small) lure, never behind the boat
                // Below a floating lure the camera stays down and looks up at it against the bright
                // surface; deeper, it rides a little above the lure.
                const float cy = clampf(s_ly + 46, 40, SURF - 85);
                const float want_p[3] = { s_lx - ux * back, cy, s_lz - uz * back };
                // looking up at a floating lure the aim goes higher, so the lure sits mid-frame, not under the clock
                const float want_t[3] = { s_lx + ux * 150, s_ly + (cy < s_ly ? 95.0f : 0.0f), s_lz + uz * 150 };
                const float k = s_state_ms == now || now - s_state_ms < 40 ? 1.0f : clampf(dt * 5, 0, 1);
                for (int j = 0; j < 3; j++) { s_rcp[j] += (want_p[j] - s_rcp[j]) * k; s_rct[j] += (want_t[j] - s_rct[j]) * k; }
                cam(s_rcp[0], s_rcp[1], s_rcp[2], s_rct[0], s_rct[1], s_rct[2], 64);
                lake_clear_near(s_lx - ux * back * 0.6f, s_lz - uz * back * 0.6f, 230);   // no plant fills the lens
            }
            if ((now / 120) % 4 == 0)                             // drifting specks in the water
                vx_emit(g_fx_dust, iroundf(s_lx + rnd(500) - 250), iroundf(s_ly + rnd(200) - 100), iroundf(s_lz + rnd(500) - 250), 0, 10, 0, 20, 1);
            LureState ls = { s_twitch_t > 0 ? 2 : (reel ? 0 : 1), s_lx, s_ly, s_lz, s_lure };
            const int nib = s_state == ST_RETRIEVE ? fish_nibbling() : -1;
            if (nib >= 0) {                                        // a fish mouthing the lure: "biting"
                static int tick_at;
                (void)tick_at;
                if (g_fish_peck) { sfxv("tick", 300, 230 + rnd(60)); rumble(5000, 12000, 60); s_shake = 1.5f; }   // each peck: a tick up the line
                static int peck_at;                                // each peck knocks the lure a little
                if (g_fish_peck) peck_at = now;
                const float pe = (now - peck_at) / 250.0f, kick = pe < 1 ? sinf_(pe * PI_F) : 0.0f;
                lure_pose(s_lx + ux * kick * 4, s_ly + kick * 2, s_lz + uz * kick * 4, yaw + kick * 0.18f);
                if (s_in.d_hit || s_in.a_hit || pressed(NV_PAD_UP)) {   // struck at a nibble: too early
                    fish_spook(nib);
                    msg(T("TROPPO PRESTO!", "TOO EARLY!"), now, 900);
                    sfx("splash");
                }
            }
            s_nibble = nib;
            {   // "no biters" (as the announcer said): a hint after a long empty retrieve
                static int quiet_since, hinted, seen_entry = -1;
                if (seen_entry != s_state_ms || fish_any_interest() || nib >= 0) {   // a new retrieve, or company
                    seen_entry = s_state_ms; quiet_since = now; hinted = 0;
                }
                else if (!hinted && now - quiet_since > 14000) {
                    hinted = 1;
                    msg(T("NIENTE ABBOCCATE: CAMBIA ESCA O POSTO", "NO BITERS: TRY ANOTHER LURE OR SPOT"), now, 2200);
                }
            }
            g_fish_peck = 0;
            const int st = fish_update(&ls, dt, now);
            if (s_state == ST_RETRIEVE) {
                // Junk on the bed (Fisherman's Bait's tin cans and boots, but real ones lying there): a
                // lure dragged along the bottom over one snags it; it comes off the bed and hangs on the
                // line, heavy, and has to be reeled all the way in. No fish bites while it is on.
                if (s_junk_on < 0 && s_ly < LURE_BED + 25) {
                    const int j = lake_junk_at(s_lx, s_ly, s_lz, 34);
                    if (j >= 0) {
                        s_junk_on = j;
                        fish_release_others(-1);
                        msg(T("AGGANCIATO QUALCOSA SUL FONDO!", "SNAGGED SOMETHING ON THE BOTTOM!"), now, 1800);
                        sfx("click"); sfxv("thud", 240, 160);
                        rumble(16000, 9000, 260);
                        s_shake = 3;
                        vx_emit(g_fx_dust, iroundf(s_lx), 10, iroundf(s_lz), 0, 60, 0, 60, 14);
                    }
                }
                if (s_junk_on >= 0) lake_junk_move(s_junk_on, s_lx + ux * 10, s_ly + 4, s_lz + uz * 10);   // trailing the lure
                if (st >= 0 && s_junk_on < 0) {
                    s_strike_fish = st; s_strike_until = now + 850;
                    s_junk = -1;                                   // a fish that bites is a fish
                    snd_strike();
                    banner(T("PRESO!", "HIT!"), C_YELLOW, now);
                    rumble(20000, 45000, 180);
                    s_shake = 7;
                    s_fpx = fish_mark_x(st); s_fpz = fish_mark_z(st);   // the lunge starts where it was
                    vx_emit(g_fx_bubble, iroundf(s_lx), iroundf(s_ly), iroundf(s_lz), 0, 160, 0, 80, 20);
                    go(ST_STRIKE, now);
                } else if (d < 130 && s_junk_on >= 0) {           // the junk at the boat: off the line
                    static const int bonus[4] = { 8, 12, 10, 25 };
                    s_junk = lake_junk_kind(s_junk_on);
                    lake_junk_take(s_junk_on);
                    s_junk_on = -1;
                    fish_release_others(-1);
                    s_tb = bonus[s_junk]; s_time_ms += s_tb * 1000; s_tb_at = nv_millis();
                    s_new_rank = -1; s_perfect = 0;
                    sfx("junk"); lake_view(0); lure_hide();
                    go(ST_CATCH, now);
                } else if (d < 130) {
                    msg(T("RECUPERATA", "REELED IN"), now, 700);
                    to_aim(now);
                }
            } else {                                               // strike window: set the hook!
                // The lunge: the fish shoots onto the lure in a fifth of a second, then shakes its head.
                const float k = clampf((now - (s_strike_until - 850)) / 200.0f, 0, 1), e = 1 - (1 - k) * (1 - k);
                const float tx = s_lx + ux * 34, tz = s_lz + uz * 34;
                if (s_strike_fish >= 0)
                    fish_pose(s_strike_fish, s_fpx + (tx - s_fpx) * e, s_ly, s_fpz + (tz - s_fpz) * e, yaw,
                              sinf_(now * 0.05f) * (k < 1 ? 0.1f : 0.3f), 0);
                const int set = s_in.d_hit || s_in.a_hit || s_in.crank_hit || pressed(NV_PAD_UP);   // pull back, crank, or lift
                // A big predator that hit hard can hook itself at the end of the window.
                const int sp = s_strike_fish >= 0 ? fish_species(s_strike_fish) : -1;
                const int selfhook = !set && now > s_strike_until - 60 && s_junk < 0 && (sp == SP_PIKE || sp == SP_BASS || sp == SP_ZANDER || sp == SP_GAR)
                                     && fish_kg(s_strike_fish) > 2.5f && rnd(100) < 35;
                if (set && s_junk >= 0) {                              // junk on the hook: CLEAN UP!
                    static const int bonus[4] = { 8, 12, 10, 25 };
                    fish_release_others(-1);
                    s_tb = bonus[s_junk]; s_time_ms += s_tb * 1000; s_tb_at = nv_millis();
                    s_new_rank = -1; s_perfect = 0;
                    sfx("junk"); lake_view(0); lure_hide();
                    go(ST_CATCH, now);
                } else if (set || selfhook) {                // set the hook: rod back, crank or lift
                    s_perfect = set && now - (s_strike_until - 850) < 300;   // a snap hook-set tires the fish
                    if (selfhook) msg(T("SI È FERRATO DA SOLO!", "IT HOOKED ITSELF!"), now, 1100);
                    fish_release_others(s_strike_fish);
                    fight_start(&s_fight, s_strike_fish, s_lx - s_bx, s_ly, s_lz - s_bz);
                    s_air = 0;
                    if (s_perfect) { s_fight.stamina = 0.8f; msg(T("FERRATA PERFETTA!", "PERFECT HOOK-SET!"), now, 1100); }
                    else if (fish_kg(s_strike_fish) >= 4.0f) msg(T("PESCE GROSSO!", "BIG ONE!"), now, 1100);
                    banner(T("ALLAMATO!", "FISH ON!"), C_YELLOW, now);
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
                    s_junk = -1;
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
            if (s_fight.bolt_now) {                                  // the boat-side dash
                msg(s_fight.bolt_now == 2 ? T("TORNA ALLA CARICA!", "IT'S BACK FOR MORE!") : T("SCAPPA! MOLLA!", "IT BOLTS! LET GO!"), now, 1300);
                sfx("drum"); sfx("splash"); s_shake = 8;
                rumble(40000, 50000, 260);
            }
            {   // the pad feels it: a jolt on each hard run, a buzz while the line is in the red
                if (s_fight.surge > 0.38f) rumble(26000, 42000, 160);   // the jolt of a run (the hum in the red is haptics_frame's)
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
            float wig = sinf_(now * (0.010f + 0.012f * effort)) * (0.05f + 0.22f * effort);
            {   // Head shakes on the hook: every second or two a fish with strength left jerks its head
                // hard from side to side for a moment (a puff of bubbles off the lip) - not only the swim.
                static int shake_at, shake_next;
                if (!shake_next) shake_next = now + 900;
                if (now > shake_next && s_fight.stamina > 0.25f && !s_fight.jumping) {
                    shake_at = now; shake_next = now + 1100 + rnd(1300) - (int)(s_fight.stamina * 400);
                    float mx, my, mz;
                    fish_mouth(s_fight.fish, &mx, &my, &mz);
                    vx_emit(g_fx_bubble, iroundf(mx), iroundf(my), iroundf(mz), 0, 120, 0, 40, 6);
                }
                const int st = now - shake_at;
                if (st < 420) wig += sinf_(st * 0.06f) * (0.38f * s_fight.stamina + 0.08f) * (1 - st / 420.0f);
            }
            const float pitch = s_fight.jumping ? -0.6f : (s_fight.stamina < 0.2f ? -0.25f : 0.0f);
            fish_pose(s_fight.fish, fx, s_fight.fy, fz, s_fyaw, wig, pitch);
            {   // The fight, directed like the bite: the view on the line (where the runs read best) and,
                // every few seconds or on what the fish does, a cut to a closer shot - its profile as it
                // thrashes on the hook, its face with the lure in its lip as it is dragged in, or from
                // below against the bright surface. Each shot eases after the fish; a cut is a cut.
                enum { F_LINE, F_SIDE, F_FACE, F_BELOW };
                static int fshot = F_LINE, fshot_at, fnext = 0, prev_fight_ms = -1;
                const float flen = 1.12f * (52 + fish_kg(s_fight.fish) * 17 > 220 ? 220 : 52 + fish_kg(s_fight.fish) * 17);
                const float fb = 110 + flen * 2.0f;                 // closer on small fish: it fills the view
                const int near_boat = s_fight.dist < fb + 260;
                int cut = 0;
                if (prev_fight_ms != s_state_ms) { prev_fight_ms = s_state_ms; fshot = F_LINE; fshot_at = now; fnext = 0; cut = 1; }
                const int held = now - fshot_at;
                int want = -1;
                if ((s_fight.bolt_now || s_fight.surge > 0.38f) && fshot == F_LINE && held > 1500) want = F_SIDE;   // a run
                else if (fshot == F_LINE ? held > 3600 : held > 2400) {
                    static const int order[4] = { F_SIDE, F_BELOW, F_FACE, F_SIDE };
                    if (fshot != F_LINE) want = F_LINE;               // back to the line between close shots
                    else { want = order[fnext & 3]; fnext++; }
                    if (want == F_FACE && (s_fight.stamina > 0.5f || near_boat || fish_kg(s_fight.fish) < 0.6f)) want = F_SIDE;   // the face: a tiring fish big enough to read head-on
                }
                if (s_fight.stamina < 0.15f && fshot == F_LINE && held > 2000 && !near_boat && fish_kg(s_fight.fish) >= 0.6f) want = F_FACE;   // dragged in, mouth first
                if (want >= 0 && want != fshot) { fshot = want; fshot_at = now; cut = 1; }
#ifdef BASS_DEBUG_SHOTS
                if (cut) { char b[32] = "bass: fshot 0"; b[12] = (char)('0' + fshot); nv_log(NV_LOG_INFO, b); }
#endif
                const float hx = sinf_(s_fyaw), hz = cosf_(s_fyaw), sx = hz, sz = -hx;   // heading, its side
                const float side = s_fight.run_dir < 0 ? -1.0f : 1.0f, t = (now - fshot_at) / 1000.0f;
                float want_p[3], want_t[3], fov = 64;
                if (fshot == F_SIDE) {            // close profile, drifting along it as it thrashes
                    const float r = 70 + flen * 1.5f;
                    want_p[0] = fx + sx * r * side + hx * (t * 18 - 20); want_p[1] = s_fight.fy + 10 + flen * 0.1f;
                    want_p[2] = fz + sz * r * side + hz * (t * 18 - 20);
                    want_t[0] = fx + hx * flen * 0.2f; want_t[1] = s_fight.fy; want_t[2] = fz + hz * flen * 0.2f;
                    fov = 56;
                } else if (fshot == F_FACE) {     // ahead of it, looking into its face, the line from its lip
                    const float r = 95 + flen * 1.6f;
                    want_p[0] = fx + hx * r + sx * 34; want_p[1] = s_fight.fy + 6; want_p[2] = fz + hz * r + sz * 34;
                    want_t[0] = fx + hx * flen * 0.15f; want_t[1] = s_fight.fy; want_t[2] = fz + hz * flen * 0.15f;
                    fov = 58;
                } else if (fshot == F_BELOW) {    // under it, the silhouette against the surface
                    const float r = 60 + flen * 1.2f;
                    want_p[0] = fx - sx * r * side - hx * 30; want_p[1] = s_fight.fy - 70 - flen * 0.35f;
                    want_p[2] = fz - sz * r * side - hz * 30;
                    want_t[0] = fx; want_t[1] = s_fight.fy + 20; want_t[2] = fz;
                    fov = 60;
                } else {
                    // Behind the fish on the line to the boat; near the boat that would put the camera past
                    // the hull (its underside filling the view, and costly), so it swings round beside the fish.
                    const float sw = clampf((fb + 220 - s_fight.dist) / 320.0f, 0, 1);
                    float dxc = -ux * (1 - sw) + uz * sw, dzc = -uz * (1 - sw) - ux * sw;
                    const float dn = sqrtf_(dxc * dxc + dzc * dzc) + 1e-4f;
                    dxc /= dn; dzc /= dn;
                    want_p[0] = fx + dxc * fb; want_p[1] = clampf(s_fight.fy + 50 + flen * 0.3f, 40, SURF - 70); want_p[2] = fz + dzc * fb;
                    want_t[0] = fx; want_t[1] = s_fight.fy - 10; want_t[2] = fz;
                }
                want_p[1] = clampf(want_p[1], 22, SURF - 40);
                // close shots follow tightly (a fish being reeled in moves fast, it must stay in the frame)
                const float k = cut ? 1.0f : clampf(dt * (fshot == F_LINE ? 3.5f : 9.0f), 0, 1);
                for (int j = 0; j < 3; j++) { s_ccp[j] += (want_p[j] - s_ccp[j]) * k; s_cct[j] += (want_t[j] - s_cct[j]) * k; }
                static float s_cfov = 64;
                s_cfov = cut ? fov : s_cfov + (fov - s_cfov) * clampf(dt * 3, 0, 1);
                cam(s_ccp[0], s_ccp[1], s_ccp[2], s_cct[0], s_cct[1], s_cct[2], s_cfov);
                lake_clear_near((s_ccp[0] + s_cct[0]) / 2, (s_ccp[2] + s_cct[2]) / 2, 260);
            }
            {   // the rod bends toward the fish, more under tension; dips on a jump
                int sx = W / 2, sy = H / 2;
                project(fx, s_air ? s_air_h : s_fight.fy, fz, &sx, &sy);
                const float tx = W - 170 + (sx - W / 2) * 0.35f,
                            ty = 70 + s_fight.tension * 60 + (s_fight.jumping ? 50 : 0) - g_rod_lift * 34;
                rod_seek(tx, ty, 18 + s_fight.tension * 70, 6, dt);
            }
            if ((now / 160) % 4 == 0) vx_emit(g_fx_bubble, iroundf(fx), iroundf(s_fight.fy + 20), iroundf(fz), 0, 100, 0, 30, 1);
            if (s_fight.jumping && (now / 100) % 2 == 0) vx_emit(g_fx_bubble, iroundf(fx), iroundf(s_fight.fy), iroundf(fz), 0, 260, 0, 90, 6);
            if (s_in.a || s_fight.drag) sfx_reel(now);             // cranking, or the drag clicking
            {   // a jump starting: the splash
                static int was_jumping;
                if (s_fight.jumping && !was_jumping) sfx("jump");
                was_jumping = s_fight.jumping;
            }
            {   // The leap (Fisherman's Bait): the fish clears the water. The view cuts above the surface,
                // zoomed on it from beside the boat: it climbs nose up, shakes its head at the top and
                // falls back in. Rod down (B / DOWN) while it is in the air, or it throws the hook.
                const int air = s_fight.jumping && s_fight.jump_t < JUMP_AIR && s_fight.jump_t > JUMP_IN;
                if (air != s_air) {
                    s_air = air;
                    lake_view(!air);
                    if (air) { vx_emit(g_fx_splash, iroundf(fx), 4, iroundf(fz), 0, 320, 0, 90, 24); sfx("splash"); s_air_at = now; s_air_x = fx; s_air_z = fz; }
                    else vx_emit(g_fx_bubble, iroundf(fx), iroundf(SURF - 30), iroundf(fz), 0, -40, 0, 90, 24);
                }
                if (air) {
                    const float u = (JUMP_AIR - s_fight.jump_t) / (JUMP_AIR - JUMP_IN);
                    s_air_h = 12 + (80 + fish_kg(s_fight.fish) * 12) * sinf_(u * PI_F);
                    fish_pose(s_fight.fish, fx, s_air_h, fz, s_fyaw, sinf_(now * 0.045f) * 0.5f, -1.0f * (1 - 2 * u));
                    // A close side shot at the waterline, the fish filling a good part of the frame.
                    const float side = s_fight.run_dir < 0 ? -1.0f : 1.0f;
                    const float kg = fish_kg(s_fight.fish), len = 1.12f * (52 + kg * 17 > 220 ? 220 : 52 + kg * 17);
                    const float back = 70 + len * 2.2f, off = 60 + len * 2.6f;
                    const float cx = fx - ux * back + uz * off * side, cz = fz - uz * back - ux * off * side;
                    cam(cx, 40 + len * 0.4f, cz, fx, s_air_h * 0.75f + 8, fz, 50);
                    angler_pose(3);
                    aim_camera_boat();
                }
            }
            {   // the lure hangs from the corner of the mouth (its hooks in the lip), nose toward the
                // boat where the line runs; posed after the jump so it rides the leap too
                float mx, my, mz;
                fish_mouth(s_fight.fish, &mx, &my, &mz);
                const float dx = s_bx - mx, dz = s_bz - mz, dl = sqrtf_(dx * dx + dz * dz) + 1e-3f;
                s_lure_wave = 0.15f + 0.6f * clampf(s_fight.run, 0, 1);   // it shakes as the fish fights
                const float fsc = (52 + fish_kg(s_fight.fish) * 17) / 100.0f;      // the fish's model scale
                s_lure_scale = clampf(fsc * 1.1f, 0.6f, 1.0f);                     // a little fish, a little bait
                lure_pose(mx + dx / dl * 10 * s_lure_scale, my, mz + dz / dl * 10 * s_lure_scale, atan2f_(dx, dz));
                s_lure_scale = 1.0f;
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
                s_tb = time_bonus(s_catch_sp, s_catch_kg, s_combo);
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
                if (s_new_rank >= 0 && s_junk < 0) { s_name_slot = 0; snd_click(); go(ST_NAME, now); }
                else if (s_time_ms <= 0) { fish_hide(); go(ST_WEIGH, now); } else to_aim(now);
            }
            break;
        }
        case ST_NAME: {                                    // initials for the record wall
            char *nm = s_names[s_new_rank];
            int step = 0;
            if (pressed(NV_PAD_UP)) step = 1;
            if (pressed(NV_PAD_DOWN)) step = -1;
            if (pressed(NV_PAD_LEFT) && s_name_slot > 0) { s_name_slot--; snd_click(); }
            if (pressed(NV_PAD_RIGHT) && s_name_slot < 2) { s_name_slot++; snd_click(); }
            int done = 0;
            if (pressed(NV_PAD_A | NV_PAD_START)) { if (s_name_slot < 2) { s_name_slot++; snd_click(); } else done = 1; }
            if (s_in.tap) {
                for (int k = 0; k < 3; k++) {
                    const int x = W / 2 - 105 + k * 74;
                    if (s_in.tx >= x && s_in.tx < x + 62) {
                        if (s_in.ty >= 78 && s_in.ty < 112) { s_name_slot = k; step = 1; }
                        else if (s_in.ty >= 186 && s_in.ty < 220) { s_name_slot = k; step = -1; }
                        else if (s_in.ty >= 112 && s_in.ty < 186) { s_name_slot = k; snd_click(); }
                    }
                }
                if (in_rect(&kNameOk, s_in.tx, s_in.ty)) done = 1;
            }
            if (step) {
                static const char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-";
                int i = 0;
                while (abc[i] && abc[i] != nm[s_name_slot]) i++;
                if (!abc[i]) i = 0;
                i = (i + step + 37) % 37;
                nm[s_name_slot] = abc[i];
                sfxv("tick", 260, 256);
            }
            if (done && now - s_state_ms > 300) {
                for (int c = 0; c < 4; c++) s_names[NRECORDS][c] = nm[c];         // remembered for next time
                nv_save("names.bin", s_names, sizeof s_names);
                sfx("catch");
                if (s_time_ms <= 0) { fish_hide(); go(ST_WEIGH, now); } else to_aim(now);
            }
            break;
        }
        case ST_LOST:
            angler_pose(3);
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
            // Never skipped by accident: a finger still on the reel button or A still held when the
            // clock ran out must be let go once before the verdict accepts a tap or a press.
            static int armed_for = -1;
            if (now - s_state_ms > 1800 && !s_prev_down && !(s_pad & (NV_PAD_A | NV_PAD_START))) armed_for = s_state_ms;
            if (armed_for == s_state_ms && now - s_state_ms > 2500 && (s_screen_btn == 0 || pressed(NV_PAD_A | NV_PAD_START))) {
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
            if (now - s_state_ms > 1200 && (s_screen_btn == 1 || pressed(NV_PAD_A | NV_PAD_START))) {   // retry this lake
                s_screen_btn = -1;
                s_loop = 0; s_run_total = 0; s_run_catches = 0; s_run_best_kg = 0; s_stages_cleared = 0;
                start_stage(now);
            } else if (now - s_state_ms > 1200 && (s_screen_btn == 0 || pressed(NV_PAD_B))) {
                s_screen_btn = -1; s_logo_at = now;
                s_stage = 0; s_loop = 0;
                lake_build(0, 0); fish_build(); build_lures(); lake_view(0);
                s_menu = 0; go(ST_TITLE, now);
            }
            break;
        }

        sound_frame(now);
        // Full-screen paintings hide the 3D frame: don't render it under them.
        if (!(s_state == ST_TITLE || s_state == ST_RECORDS || s_state == ST_STAGE || s_state == ST_WEIGH || s_state == ST_OVER || s_state == ST_LURE || s_state == ST_INTRO ||
              s_state == ST_SELECT || s_state == ST_CATCH || s_state == ST_NAME))
            vx_render();

        // ---- 2D over the frame ----
        switch (s_state) {
        case ST_INTRO: draw_intro(now); break;
        case ST_TITLE: draw_title(now); break;
        case ST_RECORDS: draw_records(); break;
        case ST_SELECT: draw_select(now); break;
        case ST_STAGE: draw_stage_card(now); break;
        case ST_LURE: draw_lure_select(now); break;
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
            draw_wake(now);
            if (s_boil >= 0 && now - s_boil_at < 1400)             // rings where the fish rose
                water_rings(s_boil_x, s_boil_z, (now - s_boil_at) / 1000.0f, 150, 3);
            if (s_pose < 0) draw_rod(0, now);
            {   // the lure hanging from the tip on a short line
                const int lx = iroundf(s_rtx) + (s_charging && s_in.a ? 6 : 0), ly = iroundf(s_rty) + 22;
                nv_gfx_line(iroundf(s_rtx), iroundf(s_rty), lx, ly, C565(236, 236, 244));
                nv_gfx_circle(lx, ly + 3, 4, s_lure == LURE_CRANK ? C_RED : s_lure == LURE_POPPER ? C_YELLOW : s_lure == LURE_JIG ? C565(40, 60, 190) : C565(130, 60, 170));
            }
            panel(W / 2 - 104, 246, 208, 24);
            bar(W / 2 - 96, 254, 192, 8, s_power, s_power > 0.85f ? C_RED : C_YELLOW, 0);
            if (s_in.a || !pad_connected()) text_c(228, s_in.a ? T("RILASCIA PER LANCIARE", "RELEASE TO CAST") : T("A LANCIO  < > GIRA  SU/GIÙ MOTORE", "A CAST  < > TURN  UP/DOWN MOTOR"), C_WHITE, 1);
            draw_sonar(now);
            if (s_engine == 1) text_c(150, T("AVVIO MOTORE...", "STARTING MOTOR..."), C_YELLOW, 2);
            if (s_lure_open || now < s_lure_pop) {               // the lure strip under the lure box
                panel(W - 8 - NLURES * 46, 56, NLURES * 46 + 4, 50);
                for (int k = 0; k < NLURES; k++) {
                    const int x = W - 4 - (NLURES - k) * 46;
                    if (k == s_lure) nv_gfx_panel(x, 58, 44, 46, 8, C565(255, 230, 90), C565(230, 140, 20), 255);
                    char n[8] = "lure0";
                    n[4] = (char)('0' + k);
                    nv_gfx_image(n, x + 2, 60, 40, 40);
                }
            }
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
            if (s_pose < 0) draw_rod(0, now);
            break;
        }
        case ST_RETRIEVE:
        case ST_STRIKE: {
            draw_rope();
            hud_top();
            hud_gauges(1.0f - s_ly / SURF, s_slack > 0 ? 0.0f : (s_in.a ? 0.25f + 0.2f * s_reel_amt : 0.08f), 0, now);
            hud_line(s_line);
            const char *act = s_twitch_t > 0 ? T("STRAPPO", "TWITCH") : s_in.b ? T("MOLLO FILO", "GIVING LINE") : s_in.a ? (s_slack > 0 ? T("RECUPERO IL MOLLE", "TAKING UP SLACK") : T("RECUPERO", "REELING"))
                            : s_on_bed ? T("SUL FONDO", "ON THE BOTTOM") : (s_lure == LURE_WORM || s_lure == LURE_JIG) ? T("AFFONDA...", "SINKING...") : T("PAUSA", "PAUSE");
            {
                const int aw = ftext_w(act, F_S, 100) + 16;
                nv_gfx_panel(W - 52 - aw, 186, aw, 18, 9, C565(20, 50, 80), C565(8, 20, 40), 200);
                ftext(W - 44 - aw, 187, act, C_CYAN, F_S, 100);
            }
            for (int i = 0; i < fish_slots(); i++) {               // what the fish think of the lure
                float fx, fy, fz;
                const int mk = fish_mark(i, &fx, &fy, &fz);
                int sx, sy;
                if (!mk || !project(fx, fy, fz, &sx, &sy) || sy < 40 || sy > H - 20) continue;
                const char *t = mk == 2 ? "!" : "?";
                nv_gfx_circle(sx + 2, sy + 2, 9, C_SHADOW);
                nv_gfx_circle(sx, sy, 9, mk == 2 ? C_RED : C_YELLOW);
                text_sh(sx - tw(t, 2) / 2, sy - 6, t, C_WHITE, 2);
            }
            if (s_nibble >= 0 && s_state == ST_RETRIEVE) {        // biting: wait for the take
                const char *bt = T("ABBOCCA... ASPETTA!", "BITING... WAIT!");
                const int bw = ftext_w(bt, F_M, 100) + 26, jig = (now / 90) & 1;
                nv_gfx_panel((W - bw) / 2, 112 + jig, bw, 26, 13, C565(40, 160, 200), C565(10, 70, 110), 225);
                ftext((W - bw) / 2 + 13, 114 + jig, bt, C_WHITE, F_M, 100);
            }
            if (s_state == ST_STRIKE) {                            // the take: set the hook now
                const int left = s_strike_until - now, big = ((now / 70) & 1);
                const char *ft = T("FERRA!", "STRIKE!");
                const int pct = 100 + (left > 700 ? (left - 700) / 2 : 0);
                const int fw = ftext_w(ft, F_L, pct) + 36;
                nv_gfx_panel((W - fw) / 2, 150, fw, 44, 12, big ? C565(255, 90, 60) : C565(255, 200, 40), C565(150, 30, 20), 235);
                ftext((W - ftext_w(ft, F_L, pct)) / 2, 152, ft, C_WHITE, F_L, pct);
                nv_gfx_panel((W - fw) / 2 + 6, 197, (fw - 12) * (left > 0 ? left : 0) / 850, 5, 2, C_WHITE, C_YELLOW, 255);   // the window running out
            }
            s_icon_a = "b_reel"; s_icon_b = 0;
            s_arrows_label = T("GUIDA / GIÙ STRAPPO", "STEER / DOWN TWITCH");
            draw_controls(T("MULINELLO", "REEL"), T("MOLLA", "RELEASE"), 3);
            break;
        }
        case ST_FIGHT: {
            hud_top();
            int sx, sy;
            // The line from the rod (bottom of the screen) to the fish's mouth.
            const float d = sqrtf_((s_lx - s_bx) * (s_lx - s_bx) + (s_lz - s_bz) * (s_lz - s_bz)) + 1e-3f, ux = (s_lx - s_bx) / d, uz = (s_lz - s_bz) / d;
            const float fx = s_bx + ux * s_fight.dist + uz * s_fight.fx, fz = s_bz + uz * s_fight.dist - ux * s_fight.fx;
            float hx, hy, hz;
            fish_mouth(s_fight.fish, &hx, &hy, &hz);                  // tied to the nose of the lure in its mouth
            {
                const float dx = s_bx - hx, dz = s_bz - hz, dl = sqrtf_(dx * dx + dz * dz) + 1e-3f;
                hx += dx / dl * 22; hz += dz / dl * 22;
            }
            (void)fx; (void)fz;
            if (project(hx, hy, hz, &sx, &sy)) {   // taut line: straighter the harder it pulls
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
            hud_gauges(1.0f - (s_air ? 1.0f : s_fight.fy / SURF), s_fight.tension, s_fight.strain, now);
            hud_line(s_fight.dist);
            {   // the fish: stamina bar and how far out it is, one plate under the clock and combo
                char db[24], dt[12];
                fmt_int(dt, iroundf(s_fight.dist / 100)); db[0] = 0; cat(db, dt); cat(db, " M");
                panel(W / 2 - 104, 84, 208, 22);                      // under the livewell row
                const char *fl = T("PESCE", "FISH");
                const int fp = fit_pct(fl, F_S, 100, 45);
                ftext(W / 2 - 98, 89 + (100 - fp) / 12, fl, C_GREY, F_S, fp);
                bar(W / 2 - 50, 91, 100, 8, s_fight.stamina, C_CYAN, 0);
                ftext(W / 2 + 98 - ftext_w(db, F_S, 100), 86, db, C_WHITE, F_S, 100);
            }
            if (s_fight.strain > 0.45f)
                text_c(116, T("STA PER ROMPERSI! MOLLA!", "ABOUT TO SNAP! LET GO!"), ((now / 90) & 1) ? C_RED : C_WHITE, 2);
            else if (s_fight.tension > 0.85f && s_in.a)
                text_c(116, T("ROSSO! SMETTI DI RECUPERARE", "RED! STOP REELING"), ((now / 120) & 1) ? C_RED : C_WHITE, 2);
            else if (s_fight.slack_t > 0.6f)
                text_c(116, T("LENZA MOLLE! RECUPERA", "SLACK LINE! REEL IN"), ((now / 120) & 1) ? C_YELLOW : C_WHITE, 2);
            else if (s_fight.drag) text_c(116, T("FRIZIONE: IL PESCE PRENDE FILO", "DRAG: THE FISH TAKES LINE"), C_YELLOW, 1);
            else if (s_fight.stamina < 0.15f) text_c(116, T("È STANCO! RECUPERA!", "IT'S TIRED! REEL!"), C_GREEN, 2);
            else if (now - s_state_ms < 3500 && !s_fight.jumping)
                text_c(116, T("A RECUPERA - B MOLLA QUANDO È ROSSA", "A REEL - B RELEASE WHEN IT GOES RED"), C_WHITE, 1);
            // Which way the fish is running: steer the rod the other way.
            if (s_fight.run_dir != 0 && s_fight.run > 0.35f && !s_air) {
                const int ax = s_fight.run_dir > 0 ? W - 90 : 60, s = s_fight.run_dir > 0 ? 1 : -1;
                nv_gfx_tri(ax - s * 16, 130, ax - s * 16, 162, ax + s * 16, 146, C_RED);
                text_sh(ax - 30, 168, T("TIRA", "PULL"), C_RED, 1);
            }
            if (s_fight.jumping) {                                  // above the fish, out of its way
                const char *jt = s_fight.jump_ok ? T("CANNA GIÙ: BRAVO!", "ROD DOWN: GOOD!") : T("SALTO! CANNA GIÙ!", "JUMP! ROD DOWN!");
                const int jw = ftext_w(jt, F_L, 70) + 30;
                panel((W - jw) / 2, 110, jw, 36);
                ftext((W - ftext_w(jt, F_L, 70)) / 2, 112, jt, s_fight.jump_ok ? C_GREEN : ((now / 90) & 1) ? C_YELLOW : C_WHITE, F_L, 70);
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
        case ST_NAME: draw_name(now); break;
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
