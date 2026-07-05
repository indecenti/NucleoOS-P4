// Pianino - a simple colorful one-octave piano for kids (ABI v3 game surface).
// Eight big rainbow keys (Do Re Mi Fa Sol La Si Do'), four instrument presets (Synth/Piano/
// Drum/Xylo) picked from a button row, plus a little sparkle burst on every tap. A milestone toast
// celebrates every N notes played.
//
// MULTITOUCH (ABI v3): reads up to 3 fingers via nv_touch_count()/nv_touch_at(), so a kid can hold
// a 3-note chord at once. Each key fires on the frame a finger lands on it (per-key edge detection),
// glissando works (sliding onto a new key retriggers), and a held key never machine-guns.
//
// SOUND: two paths, chosen per preset.
//   - PIANO / XYLO use nv_sound() with real recorded-quality sample banks (tools/gen_pianino_sfx.py):
//     harmonic piano + inharmonic bell, professional timbre. The samples are deliberately LOW
//     amplitude (peak 0.25). The board's audio goes to a bus-powered USB speaker; full-scale WAVs
//     (-1 dBFS) draw a current spike that BROWNS OUT the board (clean reset, no coredump - confirmed
//     via Diagnostics showing "no crash recorded"). At 0.25 they match nv_gfx_tone's amplitude and
//     are stable. They're also kept short (~300 ms) because nv_sound plays one fully before the next
//     (queue depth 2), so long samples would lag fast tapping.
//   - SYNTH / DRUM use nv_gfx_tone(): pure synth, snappier still, distinct electronic/percussive
//     character. gfx_tone has no waveform control so it can only vary frequency + duration.
#include "nucleo_sdk.h"

static int W, H;
static int C_BG, C_INK;
static int KEY_COL[8] = {
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000  // filled at runtime (NV_RGB)
};
static int PRESET_COL[4] = { 0x0000, 0x0000, 0x0000, 0x0000 };   // filled at runtime (NV_RGB)

// use_wav: 1 -> nv_sound(wav[k]) (sample bank);  0 -> nv_gfx_tone(freq[k], dur) (synth).
typedef struct {
    const char *name; const char *label[8];
    int use_wav; const char *wav[8];
    int freq[8]; int dur;
} Preset;
static const Preset PRESETS[4] = {
    { "SYNTH", { "DO", "RE", "MI", "FA", "SOL", "LA", "SI", "DO" }, 0, { 0 },
      { 262, 294, 330, 349, 392, 440, 494, 523 }, 140 },
    { "PIANO", { "DO", "RE", "MI", "FA", "SOL", "LA", "SI", "DO" }, 1,
      { "piano_do", "piano_re", "piano_mi", "piano_fa", "piano_sol", "piano_la", "piano_si", "piano_do2" },
      { 0 }, 0 },
    { "DRUM",  { "KICK", "SNARE", "HAT", "TOM1", "TOM2", "CLAP", "RIDE", "CRASH" }, 0, { 0 },
      { 90, 180, 3200, 140, 110, 2400, 3000, 3500 }, 60 },
    { "XYLO",  { "DO", "RE", "MI", "FA", "SOL", "LA", "SI", "DO" }, 1,
      { "bell_do", "bell_re", "bell_mi", "bell_fa", "bell_sol", "bell_la", "bell_si", "bell_do2" },
      { 0 }, 0 },
};
static int g_preset = 0;

static int KX[8], KY, KW, KH;
static int PX[4], PRESET_Y, PRESET_H, PBW;

static int g_redraw = 2;
static void mark_dirty(void) { g_redraw = 2; }

static int rnd(int lo, int hi) { int r = nv_rand(); if (r < 0) r = -r; return lo + r % (hi - lo + 1); }

static int lighten(int c, int amt) { int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    r += (31 - r) * amt / 255; g += (63 - g) * amt / 255; b += (31 - b) * amt / 255; return (r << 11) | (g << 5) | b; }
static int darken(int c, int amt) { int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    r -= r * amt / 255; g -= g * amt / 255; b -= b * amt / 255; return (r << 11) | (g << 5) | b; }

static void fill_round(int x, int y, int w, int h, int r, int col) {
    if (2 * r > w) r = w / 2; if (2 * r > h) r = h / 2;
    if (r < 1) { nv_gfx_rect(x, y, w, h, col); return; }
    nv_gfx_rect(x + r, y, w - 2 * r, h, col);
    nv_gfx_rect(x, y + r, w, h - 2 * r, col);
    nv_gfx_circle(x + r, y + r, r, col);
    nv_gfx_circle(x + w - 1 - r, y + r, r, col);
    nv_gfx_circle(x + r, y + h - 1 - r, r, col);
    nv_gfx_circle(x + w - 1 - r, y + h - 1 - r, r, col);
}
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void text_cs(int cx, int cy, const char *s, int col, int sc) {
    nv_gfx_text(cx - (6 * sc * slen(s)) / 2, cy - (7 * sc) / 2, s, col, sc);
}

// ---- key rendering: rounded "3D" key, flattens + brightens while pressed. Two fill_round calls
// (12 host draws) total, no gloss layer - the gloss cost as much again per key for a subtle
// highlight that isn't worth it 8x/frame during the tap-triggered redraw burst.
static void key_card(int x, int y, int w, int h, int col, int pressed, const char *label) {
    int r = w / 8; if (r > 22) r = 22; if (r < 8) r = 8;
    int lip = pressed ? 2 : 8;
    fill_round(x, y + lip, w, h, r, darken(col, 46));
    int fy = y + (pressed ? 5 : 0);
    int fh = h - (pressed ? 5 : 0);
    fill_round(x, fy, w, fh, r, pressed ? lighten(col, 25) : col);
    int sc = slen(label) <= 3 ? 4 : 3;
    text_cs(x + w / 2, fy + fh - 40, label, NV_RGB(255, 255, 255), sc);
}

// ---- preset button: pastel + colored text when idle, flat + bright when selected -----------------
static void preset_btn(int x, int y, int w, int h, int idx) {
    const int base = PRESET_COL[idx];
    const int sel = (g_preset == idx);
    const int r = 10;
    if (sel) {
        fill_round(x, y + 3, w, h - 3, r, darken(base, 50));
        fill_round(x, y, w, h - 3, r, base);
        text_cs(x + w / 2, y + (h - 3) / 2, PRESETS[idx].name, NV_RGB(255, 255, 255), 3);
    } else {
        fill_round(x, y, w, h, r, lighten(base, 190));
        text_cs(x + w / 2, y + h / 2, PRESETS[idx].name, darken(base, 60), 3);
    }
}

// ---- sparkle burst on tap: purely a function of age since spawn, bounded ~280ms. Shorter life +
// fewer particles than the original design (was 500ms/6) - every alive particle forces a full
// redraw (mark_dirty each frame), so this is the main lever on how long a tap "costs" to draw.
#define MAXP 12
#define PART_LIFE_MS 280
typedef struct { int x0, y0, vx, col, born; } Part;
static Part g_part[MAXP];
static int g_part_i = 0;
static void spawn_particles(int x, int y, int col) {
    for (int i = 0; i < 3; i++) {
        Part *p = &g_part[g_part_i % MAXP]; g_part_i++;
        p->x0 = x; p->y0 = y; p->vx = rnd(-3, 3); p->col = col; p->born = nv_millis();
    }
}
static int particles_alive(void) {
    int now = nv_millis();
    for (int i = 0; i < MAXP; i++) if (g_part[i].born && now - g_part[i].born < PART_LIFE_MS) return 1;
    return 0;
}
static void draw_particles(void) {
    int now = nv_millis();
    for (int i = 0; i < MAXP; i++) {
        Part *p = &g_part[i];
        int age = now - p->born;
        if (!p->born || age < 0 || age >= PART_LIFE_MS) continue;
        int x = p->x0 + p->vx * age / 60;
        int y = p->y0 - age * 90 / PART_LIFE_MS;
        int r = 7 - age * 7 / PART_LIFE_MS;
        if (r < 1) continue;
        nv_gfx_circle(x, y, r, lighten(p->col, age * 180 / PART_LIFE_MS));
    }
}

static void draw_title(void) {
    const char *s = "PIANINO"; int n = 7, scale = 5, cw = 6 * scale;
    int x = W / 2 - (cw * n) / 2, y = 8;
    char buf[2] = { 0, 0 };
    for (int i = 0; i < n; i++) { buf[0] = s[i]; nv_gfx_text(x + i * cw, y, buf, KEY_COL[i % 8], scale); }
}

static int key_at(int x, int y) {
    if (y < KY || y >= KY + KH) return -1;
    for (int i = 0; i < 8; i++) if (x >= KX[i] && x < KX[i] + KW) return i;
    return -1;
}
static int preset_at(int x) {
    for (int i = 0; i < 4; i++) if (x >= PX[i] && x < PX[i] + PBW) return i;
    return -1;
}

// ---- persistence: play-count (for the milestone toast) + last preset -----------------------------
#define SAVE_MAGIC 0x50494E4F
static int g_taps = 0;
static void save_state(void) { int buf[3] = { SAVE_MAGIC, g_taps, g_preset }; nv_save("save.dat", buf, (int)sizeof buf); }
static void load_state(void) {
    int buf[3] = { 0, 0, 0 };
    if (nv_load("save.dat", buf, (int)sizeof buf) == (int)sizeof buf && buf[0] == SAVE_MAGIC) {
        g_taps = buf[1];
        g_preset = (buf[2] >= 0 && buf[2] < 4) ? buf[2] : 0;
    }
}
static const struct { int n; const char *msg; } MILESTONES[6] = {
    { 10,  "10 note suonate!" }, { 50,  "50 note, bravo!" }, { 100, "100 note, sei un pianista!" },
    { 250, "250 note, incredibile!" }, { 500, "500 note!" }, { 1000, "1000 note, maestro!" },
};

static int g_press_until[8] = { 0 };   // per-key press-flash end (ms) — several keys can be down at once

static void press(int k) {
    const Preset *p = &PRESETS[g_preset];
    if (p->use_wav) nv_sound(p->wav[k]);
    else            nv_gfx_tone(p->freq[k], p->dur);
    g_press_until[k] = nv_millis() + 160;
    spawn_particles(KX[k] + KW / 2, KY + 24, KEY_COL[k]);
    g_taps++;
    for (int i = 0; i < 6; i++) if (MILESTONES[i].n == g_taps) { nv_toast(NV_TOAST_OK, MILESTONES[i].msg); save_state(); }
    mark_dirty();
}
static void select_preset(int p) {
    if (p == g_preset) return;
    g_preset = p;
    nv_gfx_tone(700, 40);   // confirmation blip
    save_state();
    mark_dirty();
}

static void draw_all(void) {
    nv_gfx_clear(C_BG);
    draw_title();
    for (int i = 0; i < 4; i++) preset_btn(PX[i], PRESET_Y, PBW, PRESET_H, i);
    const Preset *p = &PRESETS[g_preset];
    int now = nv_millis();
    for (int i = 0; i < 8; i++) key_card(KX[i], KY, KW, KH, KEY_COL[i], now < g_press_until[i], p->label[i]);
    draw_particles();
}

NV_EXPORT("run")
void run(void) {
    W = nv_gfx_width(); H = nv_gfx_height();
    C_BG  = NV_RGB(250, 246, 236);
    C_INK = NV_RGB(90, 84, 74);
    KEY_COL[0] = NV_RGB(235, 64, 64);    // Do  - red
    KEY_COL[1] = NV_RGB(245, 140, 40);   // Re  - orange
    KEY_COL[2] = NV_RGB(245, 205, 50);   // Mi  - yellow
    KEY_COL[3] = NV_RGB(95, 190, 90);    // Fa  - green
    KEY_COL[4] = NV_RGB(40, 175, 190);   // Sol - teal
    KEY_COL[5] = NV_RGB(60, 130, 230);   // La  - blue
    KEY_COL[6] = NV_RGB(150, 100, 220);  // Si  - purple
    KEY_COL[7] = NV_RGB(230, 90, 170);   // Do' - pink
    PRESET_COL[0] = NV_RGB(70, 130, 220);    // Synth - blue
    PRESET_COL[1] = NV_RGB(150, 100, 70);    // Piano - wood brown
    PRESET_COL[2] = NV_RGB(220, 90, 60);     // Drum  - red/orange
    PRESET_COL[3] = NV_RGB(60, 180, 140);    // Xylo  - teal green

    int margin = 10, gap = 6;
    KY = 96; KH = H - 14 - KY;
    KW = (W - 2 * margin - 7 * gap) / 8;
    for (int i = 0; i < 8; i++) KX[i] = margin + i * (KW + gap);

    PRESET_Y = 48; PRESET_H = 40;
    int pgap = 8;
    PBW = (W - 2 * margin - 3 * pgap) / 4;
    for (int i = 0; i < 4; i++) PX[i] = margin + i * (PBW + pgap);

    load_state();
    mark_dirty();

    // Multi-touch (ABI v3): every frame we read ALL fingers (up to 3 used here) and press each key
    // a finger newly landed on — so a kid can hold a chord with three fingers. Per-key edge detection
    // via a held-bitmask: a key fires when its bit goes 0->1, and a finger sliding onto a new key
    // (bit changes) retriggers it (glissando), while a held key never machine-guns.
    int prev_keys = 0;          // keys held last frame (bit k)
    int prev_preset_down = 0;   // was a finger on the preset row last frame (edge-fire selection)
    int prev_flash = 0;         // keys visually flashing last frame (for redraw-on-change)
    while (nv_gfx_present()) {
        if (nv_gfx_back()) break;   // single screen: back closes the app

        int n = nv_touch_count();
        if (n > 3) n = 3;           // cap at 3 fingers (kids piano)
        int keys = 0, preset_hit = -1;
        for (int i = 0; i < n; i++) {
            int x, y;
            if (!nv_touch_at(i, &x, &y)) continue;
            if (y >= KY) { int k = key_at(x, y); if (k >= 0) keys |= (1 << k); }
            else if (y >= PRESET_Y && y < PRESET_Y + PRESET_H) { int pp = preset_at(x); if (pp >= 0) preset_hit = pp; }
        }
        for (int k = 0; k < 8; k++) if ((keys & (1 << k)) && !(prev_keys & (1 << k))) press(k);
        prev_keys = keys;

        if (preset_hit >= 0 && !prev_preset_down) select_preset(preset_hit);
        prev_preset_down = (preset_hit >= 0);

        // redraw when the set of flashing keys changes (new press OR a flash expiring)
        int now = nv_millis(), flash = 0;
        for (int k = 0; k < 8; k++) if (now < g_press_until[k]) flash |= (1 << k);
        if (flash != prev_flash) { mark_dirty(); prev_flash = flash; }
        if (particles_alive()) mark_dirty();

        if (g_redraw > 0) { draw_all(); g_redraw--; }
    }
    save_state();
}
