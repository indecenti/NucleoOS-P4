// Synth - a polyphonic synthesizer for NucleoOS, built on the AMY engine (amy_port.c).
//
// Audio: ABI v10 raw stream. Every loop iteration renders AMY blocks (AMY_BLOCK_SIZE stereo frames
// at AMY_SAMPLE_RATE) until the OS queue holds TARGET_FRAMES, so latency stays ~TARGET_FRAMES/44.1
// ms whatever the frame rate. Render time per block is measured and logged (tag app:synth).
// Rendering: ABI v6 persist mode. Each element caches what it drew and repaints only on change: an
// idle screen costs zero draw calls; a pressed key repaints that key (and its black neighbours).
// Input: multi-touch (ABI v3) for the keyboard (chords, glissando); finger 0 drives the controls.
#include "nucleo_sdk.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "amy/amy.h"
#include "names.h"

#ifdef SYNTH_BENCH
// Bench build (-DSYNTH_BENCH, for a host without ABI v10): the audio stream is simulated by a
// real-time sink, and the loop plays a 6-note chord on every preset, so the log shows render cost.
static int64_t bench_t0; static int64_t bench_frames;
static int64_t bench_us(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000; }
static int32_t bench_open(int32_t r, int32_t c) { (void)r; (void)c; bench_t0 = bench_us(); return 1; }
static int32_t bench_write(const void *p, int32_t n) { (void)p; bench_frames += n / 4; return n; }
static int32_t bench_backlog(void) {
    int64_t played = (bench_us() - bench_t0) * AMY_SAMPLE_RATE / 1000000;
    if (played > bench_frames) { bench_frames = played; }
    return (int32_t)((bench_frames - played) * 4);
}
static void bench_close(void) {}
#define nv_audio_open bench_open
#define nv_audio_write bench_write
#define nv_audio_backlog bench_backlog
#define nv_audio_close bench_close
#endif

#define W 1024
#define H 600
#define VOICES 6
#define TARGET_FRAMES 2048                 // queued audio we keep ahead (~46 ms at 44.1 kHz)
#ifdef SYNTH_BENCH
#define REPORT_EVERY 1
#else
#define REPORT_EVERY 5                     // log the render cost every 5 s
#endif
#define BLOCK_BYTES (AMY_BLOCK_SIZE * AMY_NCHANS * 2)
#define NPATCH 257                         // 0-127 Juno-106, 128-255 DX7, 256 piano

// ---- palette ---------------------------------------------------------------------------------------
#define C_BG     NV_RGB(17, 13, 28)
#define C_BAR    NV_RGB(27, 20, 45)
#define C_CARD   NV_RGB(46, 33, 76)
#define C_CARDLP NV_RGB(30, 21, 50)
#define C_ACC    NV_RGB(236, 72, 153)
#define C_ACCLP  NV_RGB(160, 38, 100)
#define C_VIO    NV_RGB(139, 92, 246)
#define C_TXT    NV_RGB(243, 239, 252)
#define C_DIM    NV_RGB(150, 138, 182)
#define C_TRACK  NV_RGB(58, 44, 92)
#define C_WKEY   NV_RGB(246, 244, 251)
#define C_WKEYLP NV_RGB(205, 198, 222)
#define C_WDOWN  NV_RGB(249, 168, 212)
#define C_BKEY   NV_RGB(30, 25, 44)
#define C_BKEYHI NV_RGB(62, 54, 84)
#define C_BDOWN  NV_RGB(200, 56, 140)
#define C_WARN   NV_RGB(251, 146, 60)

// ---- layout ----------------------------------------------------------------------------------------
#define BAR_H   64
#define PR_X0   12
#define PR_Y0   76
#define PR_W    192
#define PR_H    70
#define PR_GAP  10
#define CT_Y    238                         // control row
#define CT_H    92
#define KB_Y    340                         // keyboard
#define KB_H    (H - KB_Y)
#define KB_X0   2
#define NWHITE  15                          // C..C, two octaves + top C
#define KW      68
#define BW      40
#define BH      160
#define NKEYS   25

typedef struct { int x, y, w, h; } Rect;
static int in_rect(Rect r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

static const Rect R_PREV  = {268, 8, 64, 48};
static const Rect R_NAME  = {340, 8, 520, 48};
static const Rect R_NEXT  = {868, 8, 64, 48};
static const Rect R_LOAD  = {940, 8, 80, 48};
static const Rect R_VOL   = {16, CT_Y, 290, CT_H};
static const Rect R_TONE  = {322, CT_Y, 290, CT_H};
static const Rect R_REV   = {628, CT_Y + 22, 150, 62};
static const Rect R_OCTDN = {796, CT_Y + 22, 62, 62};
static const Rect R_OCT   = {862, CT_Y, 88, CT_H};
static const Rect R_OCTUP = {954, CT_Y + 22, 62, 62};

// ---- presets ---------------------------------------------------------------------------------------
typedef struct { int patch; const char *it, *en; } Preset;
static const Preset PRESETS[10] = {
    {47,  "JUNO PAD",   "JUNO PAD"},     // Juno A68 Synth Pad
    {64,  "ARCHI",      "STRINGS"},      // Juno B11 Strings
    {0,   "OTTONI",     "BRASS"},        // Juno A11 Brass Set 1
    {36,  "BASSO JUNO", "JUNO BASS"},    // Juno A55 Synth Bass II
    {32,  "LEAD",       "LEAD"},         // Juno A51 Lead I
    {138, "PIANO EL.",  "E.PIANO"},      // DX7 E.PIANO 1
    {153, "CAMPANE",    "BELLS"},        // DX7 TUB BELLS
    {142, "BASSO FM",   "FM BASS"},      // DX7 BASS 1
    {144, "ORGANO",     "ORGAN"},        // DX7 E.ORGAN 1
    {256, "PIANOFORTE", "PIANO"},        // AMY dpwe piano (partials)
};
#define NPRESET 10

// ---- state -----------------------------------------------------------------------------------------
typedef struct { int32_t magic, patch, octave, vol, tone, reverb; } Saved;
#define SAVE_MAGIC 0x53594e31               // "SYN1"
static int g_patch = 47, g_oct = 3, g_vol = 50, g_tone = 50, g_rev = 1;
static int g_it = 0;
static int g_audio = 0;                     // stream open
static int g_load = -1;                     // DSP load %, shown top right
static int g_busy_drawn = -1;
static float g_eq_base[3];                  // the patch's own EQ (its "x" command), tone rides on it

static void save_state(void) {
    Saved s = {SAVE_MAGIC, g_patch, g_oct, g_vol, g_tone, g_rev};
    nv_save("synth.bin", &s, sizeof s);
}
static void load_state(void) {
    Saved s;
    if (nv_load("synth.bin", &s, sizeof s) == (int)sizeof s && s.magic == SAVE_MAGIC) {
        if (s.patch >= 0 && s.patch < NPATCH) g_patch = s.patch;
        if (s.octave >= 1 && s.octave <= 6) g_oct = s.octave;
        if (s.vol >= 0 && s.vol <= 100) g_vol = s.vol;
        if (s.tone >= 0 && s.tone <= 100) g_tone = s.tone;
        g_rev = s.reverb ? 1 : 0;
    }
}

// ---- AMY control -----------------------------------------------------------------------------------
const char *synth_patch_cmd(int n);        // amy_port.c

static void send_mix(void) {
    amy_event e = amy_default_event();
    e.volume = g_vol * (2.0f / 100.0f);      // 50% -> AMY volume 1.0 (6-note chord peaks ~-6 dBFS)
    float t = (g_tone - 50) * 0.3f;          // +-15 dB of treble, a little of the mids
    e.eq_l = g_eq_base[0];
    e.eq_m = g_eq_base[1] + t * 0.3f;
    e.eq_h = g_eq_base[2] + t;
    e.reverb_level = g_rev ? 0.7f : 0.0f;
    amy_add_event(&e);
}

// A patch's own EQ is the last "x<l>,<m>,<h>" in its wire string; keep it so the tone slider is
// an offset on top of what the patch designer chose.
static void parse_patch_eq(int n) {
    g_eq_base[0] = g_eq_base[1] = g_eq_base[2] = 0;
    const char *s = synth_patch_cmd(n), *last = NULL;
    for (const char *p = s; p && *p; p++)
        if (*p == 'x' && (p == s || p[-1] == 'Z')) last = p + 1;
    if (!last) return;
    for (int i = 0; i < 3; i++) {
        char num[16]; int k = 0;
        while (*last && *last != ',' && *last != 'Z' && k < 15) num[k++] = *last++;
        num[k] = 0;
        g_eq_base[i] = k ? (float)atof(num) : 0.0f;
        if (*last != ',') break;
        last++;
    }
}

static void load_patch(int n) {
    amy_event e = amy_default_event();
    e.synth = 1;
    e.num_voices = VOICES;
    e.patch_number = (uint16_t)n;
    amy_add_event(&e);
    parse_patch_eq(n);
    send_mix();
}

static void note(int midi, float vel) {
    amy_event e = amy_default_event();
    e.synth = 1;
    e.midi_note = (float)midi;
    e.velocity = vel;
    amy_add_event(&e);
}

// ---- drawing helpers -------------------------------------------------------------------------------
static void rrect(int x, int y, int w, int h, int r, int c) {
    nv_gfx_rect(x + r, y, w - 2 * r, h, c);
    nv_gfx_rect(x, y + r, r, h - 2 * r, c);
    nv_gfx_rect(x + w - r, y + r, r, h - 2 * r, c);
    nv_gfx_circle(x + r, y + r, r, c);
    nv_gfx_circle(x + w - r - 1, y + r, r, c);
    nv_gfx_circle(x + r, y + h - r - 1, r, c);
    nv_gfx_circle(x + w - r - 1, y + h - r - 1, r, c);
}
// Card with a darker lip underneath (3D look).
static void card(Rect r, int fill, int lip) {
    rrect(r.x, r.y + 4, r.w, r.h - 4, 10, lip);
    rrect(r.x, r.y, r.w, r.h - 4, 10, fill);
}
static void text_in(Rect r, const char *s, int col, int scale, int dy) {
    int tw = nv_gfx_text_width(s, scale);
    nv_gfx_text(r.x + (r.w - tw) / 2, r.y + (r.h - 4 - 7 * scale) / 2 + dy, s, col, scale);
}

// ---- top bar ---------------------------------------------------------------------------------------
static void draw_name(void) {
    nv_gfx_rect(R_NAME.x, R_NAME.y, R_NAME.w, R_NAME.h, C_BAR);
    rrect(R_NAME.x, R_NAME.y, R_NAME.w, R_NAME.h, 10, C_BG);
    char num[8];
    snprintf(num, sizeof num, "%d", g_patch);
    nv_gfx_text(R_NAME.x + 14, R_NAME.y + 17, num, C_ACC, 2);
    int sc = nv_gfx_text_width(PATCH_NAMES[g_patch], 3) <= R_NAME.w - 90 ? 3 : 2;
    nv_gfx_text(R_NAME.x + 70, R_NAME.y + (R_NAME.h - 7 * sc) / 2, PATCH_NAMES[g_patch], C_TXT, sc);
}
static void draw_load(void) {
    nv_gfx_rect(R_LOAD.x, R_LOAD.y, R_LOAD.w, R_LOAD.h, C_BAR);
    if (!g_audio) {
        nv_gfx_text(R_LOAD.x + 8, R_LOAD.y + 10, "AUDIO", C_WARN, 2);
        nv_gfx_text(R_LOAD.x + 8, R_LOAD.y + 28, g_it ? "OCCUP." : "BUSY", C_WARN, 2);
        return;
    }
    char b[16];
    snprintf(b, sizeof b, "%d%%", g_load < 0 ? 0 : g_load);
    nv_gfx_text(R_LOAD.x + 8, R_LOAD.y + 8, "DSP", C_DIM, 2);
    nv_gfx_text(R_LOAD.x + 8, R_LOAD.y + 28, b, g_load > 70 ? C_WARN : C_DIM, 2);
}
static void draw_bar(void) {
    nv_gfx_rect(0, 0, W, BAR_H, C_BAR);
    nv_gfx_text(20, 18, "SYNTH", C_TXT, 4);
    nv_gfx_rect(20, 52, 118, 4, C_ACC);
    card(R_PREV, C_CARD, C_CARDLP);
    text_in(R_PREV, "<", C_TXT, 4, 0);
    card(R_NEXT, C_CARD, C_CARDLP);
    text_in(R_NEXT, ">", C_TXT, 4, 0);
    draw_name();
    draw_load();
}

// ---- presets ---------------------------------------------------------------------------------------
static Rect preset_rect(int i) {
    Rect r = {PR_X0 + (i % 5) * (PR_W + PR_GAP), PR_Y0 + (i / 5) * (PR_H + PR_GAP), PR_W, PR_H};
    return r;
}
static int g_pr_drawn[NPRESET];
static void draw_preset(int i) {
    Rect r = preset_rect(i);
    int on = PRESETS[i].patch == g_patch;
    nv_gfx_rect(r.x, r.y, r.w, r.h, C_BG);
    card(r, on ? C_ACC : C_CARD, on ? C_ACCLP : C_CARDLP);
    const char *lab = g_it ? PRESETS[i].it : PRESETS[i].en;
    text_in(r, lab, C_TXT, 3, -6);
    char sub[12];
    snprintf(sub, sizeof sub, "%s %d", PRESETS[i].patch < 128 ? "JUNO" : PRESETS[i].patch < 256 ? "DX7" : "AMY",
             PRESETS[i].patch);
    text_in(r, sub, on ? C_TXT : C_DIM, 1, 16);
    g_pr_drawn[i] = on;
}
static void draw_presets(int force) {
    for (int i = 0; i < NPRESET; i++) {
        int on = PRESETS[i].patch == g_patch;
        if (force || g_pr_drawn[i] != on) draw_preset(i);
    }
}

// ---- controls --------------------------------------------------------------------------------------
static void draw_slider(Rect r, const char *label, int v) {
    nv_gfx_rect(r.x, r.y, r.w, r.h, C_BG);
    nv_gfx_text(r.x + 4, r.y + 10, label, C_DIM, 2);
    char b[8];
    snprintf(b, sizeof b, "%d", v);
    nv_gfx_text(r.x + r.w - 4 - nv_gfx_text_width(b, 2), r.y + 10, b, C_TXT, 2);
    int tx = r.x + 14, tw = r.w - 28, ty = r.y + 56;
    rrect(tx, ty - 5, tw, 10, 5, C_TRACK);
    int kx = tx + tw * v / 100;
    if (kx - tx > 10) rrect(tx, ty - 5, kx - tx, 10, 5, C_ACC);
    nv_gfx_circle(kx, ty, 15, C_TXT);
    nv_gfx_circle(kx, ty, 9, C_ACC);
}
static void draw_rev(void) {
    nv_gfx_rect(R_REV.x, R_REV.y - 22, R_REV.w, R_REV.h + 22, C_BG);
    nv_gfx_text(R_REV.x + 4, CT_Y + 10 - 22 + 22, g_it ? "RIVERBERO" : "REVERB", C_DIM, 2);
    Rect b = {R_REV.x, R_REV.y + 8, R_REV.w, R_REV.h - 8};
    card(b, g_rev ? C_VIO : C_CARD, g_rev ? NV_RGB(96, 58, 190) : C_CARDLP);
    text_in(b, g_rev ? "ON" : "OFF", C_TXT, 3, 0);
}
static void draw_oct(void) {
    nv_gfx_rect(R_OCTDN.x, CT_Y, R_OCTUP.x + R_OCTUP.w - R_OCTDN.x, CT_H, C_BG);
    nv_gfx_text(R_OCTDN.x + 4, CT_Y + 10, g_it ? "OTTAVA" : "OCTAVE", C_DIM, 2);
    Rect dn = {R_OCTDN.x, R_OCTDN.y + 8, R_OCTDN.w, R_OCTDN.h - 8};
    Rect up = {R_OCTUP.x, R_OCTUP.y + 8, R_OCTUP.w, R_OCTUP.h - 8};
    card(dn, g_oct > 1 ? C_CARD : C_CARDLP, C_CARDLP);
    text_in(dn, "-", g_oct > 1 ? C_TXT : C_DIM, 4, 0);
    card(up, g_oct < 6 ? C_CARD : C_CARDLP, C_CARDLP);
    text_in(up, "+", g_oct < 6 ? C_TXT : C_DIM, 4, 0);
    char b[4] = {'C', (char)('0' + g_oct), 0};
    Rect mid = {R_OCT.x, R_OCTDN.y + 8, R_OCT.w, R_OCTDN.h - 8};
    text_in(mid, b, C_TXT, 4, 0);
}

// ---- keyboard --------------------------------------------------------------------------------------
static const int WOFF[7] = {0, 2, 4, 5, 7, 9, 11};
static int base_note(void) { return 12 * (g_oct + 1); }       // g_oct 3 -> C3 = 48
static int white_note(int w) { return base_note() + 12 * (w / 7) + WOFF[w % 7]; }
static int has_black(int w) { int m = w % 7; return w < NWHITE - 1 && m != 2 && m != 6; }
static int black_x(int w) { return KB_X0 + (w + 1) * KW - BW / 2; }   // black key right of white w

static int g_kdown[NKEYS], g_kdrawn[NKEYS];

static void draw_black(int w) {
    int k = white_note(w) + 1 - base_note();
    int d = g_kdown[k], x = black_x(w);
    nv_gfx_rect(x, KB_Y, BW, BH, C_BG);
    nv_gfx_rect(x + 2, KB_Y, BW - 4, BH - 4, d ? C_BDOWN : C_BKEY);
    if (!d) nv_gfx_rect(x + 6, KB_Y + BH - 22, BW - 12, 12, C_BKEYHI);
    g_kdrawn[k] = d;
}
static void draw_white(int w) {
    int k = white_note(w) - base_note();
    int d = g_kdown[k], x = KB_X0 + w * KW;
    nv_gfx_rect(x, KB_Y, KW, KB_H, C_BG);
    nv_gfx_rect(x + 2, KB_Y, KW - 4, KB_H - 4, d ? C_WDOWN : C_WKEY);
    nv_gfx_rect(x + 2, KB_Y + KB_H - (d ? 8 : 16), KW - 4, d ? 4 : 12, d ? C_ACC : C_WKEYLP);
    if (w % 7 == 0) {
        char b[4] = {'C', (char)('0' + g_oct + w / 7), 0};
        nv_gfx_text(x + (KW - nv_gfx_text_width(b, 2)) / 2, KB_Y + KB_H - 44, b, d ? C_ACCLP : C_DIM, 2);
    }
    g_kdrawn[k] = d;
    if (w > 0 && has_black(w - 1)) draw_black(w - 1);
    if (has_black(w)) draw_black(w);
}
static void draw_keyboard(int force) {
    for (int w = 0; w < NWHITE; w++) {
        int k = white_note(w) - base_note();
        if (force || g_kdrawn[k] != g_kdown[k]) draw_white(w);
    }
    for (int w = 0; w < NWHITE; w++) {
        if (!has_black(w)) continue;
        int k = white_note(w) + 1 - base_note();
        if (g_kdrawn[k] != g_kdown[k]) draw_black(w);
    }
}

// Key under (x,y) as an offset 0..24 from the base note, velocity 0..1 from how low on the key the
// finger lands (like striking harder). -1 = not on the keyboard.
static int key_at(int x, int y, float *vel) {
    if (y < KB_Y || x < KB_X0 || x >= KB_X0 + NWHITE * KW) return -1;
    if (y < KB_Y + BH) {
        for (int w = 0; w < NWHITE; w++) {
            if (!has_black(w)) continue;
            int bx = black_x(w);
            if (x >= bx - 4 && x < bx + BW + 4) {
                *vel = 0.45f + 0.55f * (float)(y - KB_Y) / BH;
                return white_note(w) + 1 - base_note();
            }
        }
    }
    int w = (x - KB_X0) / KW;
    if (w >= NWHITE) w = NWHITE - 1;
    *vel = 0.45f + 0.55f * (float)(y - KB_Y) / KB_H;
    return white_note(w) - base_note();
}

// ---- audio -----------------------------------------------------------------------------------------
static int64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
static int64_t g_render_us;                 // accumulated since the last report
static int g_blocks, g_underruns, g_last_report;

static void pump_audio(void) {
    if (!g_audio) return;
    int backlog = nv_audio_backlog();
    if (backlog < 0) { g_audio = 0; return; }
    if (backlog == 0 && g_blocks > 0) g_underruns++;
    int guard = 32;
    while (backlog < TARGET_FRAMES * AMY_NCHANS * 2 && guard--) {
        int64_t t0 = now_us();
        amy_execute_deltas();
        amy_render(0, AMY_OSCS, 0);
        int16_t *blk = amy_fill_buffer();
        g_render_us += now_us() - t0;
        g_blocks++;
        int n = nv_audio_write(blk, BLOCK_BYTES);
        if (n < 0) { g_audio = 0; return; }
        backlog += BLOCK_BYTES;
    }
}

static void report(void) {
    int now = nv_millis();
    if (now - g_last_report < 1000 || g_blocks == 0) return;
    int block_us = (int)(g_render_us / g_blocks);
    int budget_us = (int)(AMY_BLOCK_SIZE * 1000000LL / AMY_SAMPLE_RATE);
    int load = block_us * 100 / budget_us;
    static int reports;
    if (++reports % REPORT_EVERY == 1 % REPORT_EVERY || g_underruns) {
        char b[160];
        snprintf(b, sizeof b, "render %d us/block (%d frames, budget %d us) load %d%% blocks %d underruns %d mem %d KB",
                 block_us, AMY_BLOCK_SIZE, budget_us, load, g_blocks, g_underruns,
                 (int)(__builtin_wasm_memory_size(0) * 64));
        nv_log(NV_LOG_INFO, b);
    }
    g_render_us = 0; g_blocks = 0; g_underruns = 0; g_last_report = now;
    if (load != g_load) { g_load = load; draw_load(); }
}

static void try_open_audio(void) {
    g_audio = nv_audio_open(AMY_SAMPLE_RATE, AMY_NCHANS) == 1;
    if (g_audio != g_busy_drawn) {
        g_busy_drawn = g_audio;
        draw_load();
        char b[64];
        snprintf(b, sizeof b, "audio stream %d Hz x%d: %s", AMY_SAMPLE_RATE, AMY_NCHANS, g_audio ? "open" : "BUSY");
        nv_log(g_audio ? NV_LOG_INFO : NV_LOG_WARN, b);
    }
}

// ---- main ------------------------------------------------------------------------------------------
enum { UI_NONE, UI_KEYS, UI_PREV, UI_NEXT, UI_VOL, UI_TONE, UI_REV, UI_OCTDN, UI_OCTUP, UI_PRESET0 };

static int ui_hit(int x, int y) {
    if (y >= KB_Y) return UI_KEYS;
    if (in_rect(R_PREV, x, y)) return UI_PREV;
    if (in_rect(R_NEXT, x, y)) return UI_NEXT;
    if (in_rect(R_VOL, x, y)) return UI_VOL;
    if (in_rect(R_TONE, x, y)) return UI_TONE;
    if (in_rect(R_REV, x, y)) return UI_REV;
    if (in_rect(R_OCTDN, x, y)) return UI_OCTDN;
    if (in_rect(R_OCTUP, x, y)) return UI_OCTUP;
    for (int i = 0; i < NPRESET; i++) if (in_rect(preset_rect(i), x, y)) return UI_PRESET0 + i;
    return UI_NONE;
}
static int slider_val(Rect r, int x) {
    int v = (x - (r.x + 14)) * 100 / (r.w - 28);
    return v < 0 ? 0 : v > 100 ? 100 : v;
}

static void set_patch(int p) {
    if (p < 0) p = NPATCH - 1;
    if (p >= NPATCH) p = 0;
    g_patch = p;
    load_patch(p);
    draw_name();
    draw_presets(0);
    save_state();
}

NV_EXPORT("run")
void run(void) {
    char lang[8] = {0};
    nv_lang(lang, sizeof lang);
    g_it = lang[0] == 'i' && lang[1] == 't';
    load_state();

    amy_config_t c = amy_default_config();
    c.features.echo = 0;
    c.features.custom = 0;
    c.features.audio_in = 0;
    c.features.default_synths = 0;
    c.features.startup_bleep = 0;
    c.features.partials = 1;
    c.features.reverb = 1;
    c.features.chorus = 1;
    c.platform.multicore = 0;
    c.platform.multithread = 0;
    c.ks_oscs = 0;
    c.max_oscs = 120;
    c.max_buses = 1;
    c.max_voices = 16;
    c.max_synths = 4;
    c.max_sequencer_tags = 8;
    c.max_memory_patches = 4;
    c.overload_threshold = 0;
    amy_start(c);
    load_patch(g_patch);

    nv_gfx_persist(1);
    nv_gfx_clear(C_BG);
    draw_bar();
    draw_presets(1);
    draw_slider(R_VOL, "VOLUME", g_vol);
    draw_slider(R_TONE, g_it ? "TIMBRO" : "TONE", g_tone);
    draw_rev();
    draw_oct();
    draw_keyboard(1);
    try_open_audio();
    g_last_report = nv_millis();
    int last_open_try = nv_millis();

    int prev_down = 0, ui = UI_NONE, dirty_save = 0;
    unsigned char held[128];
    memset(held, 0, sizeof held);

    while (nv_gfx_present()) {
        if (nv_gfx_back()) break;
        pump_audio();

        // --- keyboard: every finger ---
        unsigned char cur[128];
        memset(cur, 0, sizeof cur);
        float vel[NKEYS];
        int n = nv_touch_count();
        for (int i = 0; i < n && i < 5; i++) {
            int x, y;
            if (!nv_touch_at(i, &x, &y)) continue;
            float v;
            int k = key_at(x, y, &v);
            if (k < 0) continue;
            vel[k] = v;
            cur[base_note() + k] = 1;
        }
        for (int m = 0; m < 128; m++) {
            if (cur[m] && !held[m]) {
                int k = m - base_note();
                note(m, (k >= 0 && k < NKEYS) ? vel[k] : 0.8f);
            } else if (!cur[m] && held[m]) note(m, 0);
            held[m] = cur[m];
        }
        for (int k = 0; k < NKEYS; k++) g_kdown[k] = cur[base_note() + k];
        draw_keyboard(0);

        // --- controls: finger 0 ---
        int x, y, down = nv_touch(&x, &y);
        if (down && !prev_down) ui = ui_hit(x, y);
        if (down && ui == UI_VOL) {
            int v = slider_val(R_VOL, x);
            if (v != g_vol) { g_vol = v; send_mix(); draw_slider(R_VOL, "VOLUME", g_vol); dirty_save = 1; }
        } else if (down && ui == UI_TONE) {
            int v = slider_val(R_TONE, x);
            if (v != g_tone) { g_tone = v; send_mix(); draw_slider(R_TONE, g_it ? "TIMBRO" : "TONE", g_tone); dirty_save = 1; }
        }
        if (!down && prev_down) {
            int up = ui_hit(x, y);
            if (up == ui) {
                if (ui == UI_PREV) set_patch(g_patch - 1);
                else if (ui == UI_NEXT) set_patch(g_patch + 1);
                else if (ui == UI_REV) { g_rev = !g_rev; send_mix(); draw_rev(); save_state(); }
                else if (ui == UI_OCTDN && g_oct > 1) { g_oct--; draw_oct(); draw_keyboard(1); save_state(); }
                else if (ui == UI_OCTUP && g_oct < 6) { g_oct++; draw_oct(); draw_keyboard(1); save_state(); }
                else if (ui >= UI_PRESET0) set_patch(PRESETS[ui - UI_PRESET0].patch);
            }
            if (dirty_save) { save_state(); dirty_save = 0; }
            ui = UI_NONE;
        }
        prev_down = down;

#ifdef SYNTH_BENCH
        {   // every 3 s: next preset, chord on for 2 s
            static int bstep = -1, bnext, bon;
            static const int CH[6] = {48, 52, 55, 60, 64, 67};
            int t = nv_millis();
            if (t >= bnext) {
                if (bon) { for (int i = 0; i < 6; i++) note(CH[i], 0); bon = 0; bnext = t + 1000; }
                else {
                    bstep = (bstep + 1) % NPRESET;
                    set_patch(PRESETS[bstep].patch);
                    for (int i = 0; i < 6; i++) note(CH[i], 0.8f);
                    char b[64]; snprintf(b, sizeof b, "bench: patch %d chord", g_patch);
                    nv_log(NV_LOG_INFO, b);
                    bon = 1; bnext = t + 2000;
                }
            }
        }
#endif
        if (!g_audio && nv_millis() - last_open_try > 2000) { last_open_try = nv_millis(); try_open_audio(); }
        report();
    }
    for (int m = 0; m < 128; m++) if (held[m]) note(m, 0);
    if (g_audio) nv_audio_close();
    amy_stop();
}
