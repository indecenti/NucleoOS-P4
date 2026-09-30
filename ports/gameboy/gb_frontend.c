// gb_frontend.c — Game Boy (DMG) emulator front-end for NucleoOS: Peanut-GB core + MiniGB APU.
//
// One source, several apps (ports/gameboy/build.sh):
//   embedded game  -include <gen>/<id>_rom.h (defines gb_rom_data[], GB_ROM_SIZE, GB_TITLE) — the
//                  ROM is compiled in; cart RAM is saved to "/game.sav" in the app's data folder
//                  (manifest permission "fs").
//   GB_GENERIC     a file picker over /roms/*.gb (manifest permission "home": /sdcard/home is "/");
//                  each game's cart RAM is saved next to it as /roms/<name>.sav.
//
// Screen: 160x144 scaled 3x or 4x (menu), centred on the 1024x600 canvas in persist mode (ABI 6):
// the controls are drawn once, then each shown frame blits ONE band covering the GB lines that
// changed since the last shown frame, pre-scaled in app memory. Emulation runs at 59.73 Hz paced by
// nv_millis only (audio follows, see "audio" below: it can never stall the emulator); frames that
// won't be shown are emulated with line drawing off (frame skip). The emulation hot loop makes
// no host calls: audio registers live in the in-app APU, the APU renders one frame of samples
// after each emulated frame.
// Input: multitouch on-screen pad (ABI 3) + USB/BLE pads and USB keyboard (nv_gfx_pad, ABI 9, and
// the left stick via nv_pad_state, ABI 11). L/R, the pad's Guide button or the MENU key open the
// pause menu (resume, reset, zoom, palette, exit); the OS back gesture opens it too, twice exits.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#ifdef GB_GENERIC
#include <dirent.h>
#include <sys/stat.h>
#endif

#include "nucleo_sdk.h"
#include "minigb_apu.h"

uint8_t audio_read(uint16_t addr);
void audio_write(uint16_t addr, uint8_t val);
#define ENABLE_SOUND 1
#define ENABLE_LCD 1
#define PEANUT_GB_12_COLOUR 1
#include "peanut_gb.h"

#ifndef GB_FS_ROOT          // the PC harness points this at a scratch folder
#define GB_FS_ROOT ""
#endif
#ifndef GB_TITLE
#define GB_TITLE "GAME BOY"
#endif

enum { CW = 1024, CH = 600, GW = 160, GH = 144 };
#define GB_FPS_X10000 597275            // 4194304 / 70224 = 59.7275 Hz
enum { MAX_STEP = 4 };                  // most frames emulated per shown frame
_Static_assert(AUDIO_SAMPLE_RATE <= 48000, "g_pcm sized for <= 48 kHz");

// ---- colours ----------------------------------------------------------------------------------
#define C_BG     NV_RGB(22, 24, 30)
#define C_BEZEL  NV_RGB(52, 56, 66)
#define C_ZONE   NV_RGB(32, 35, 43)
#define C_KEY    NV_RGB(72, 76, 90)
#define C_KEY_ON NV_RGB(130, 190, 150)
#define C_AB     NV_RGB(150, 36, 84)
#define C_AB_ON  NV_RGB(235, 110, 160)
#define C_TEXT   NV_RGB(215, 218, 226)
#define C_DIM    NV_RGB(130, 134, 146)
#define C_PANEL  NV_RGB(16, 18, 24)
#define C_HI     NV_RGB(64, 120, 90)

// Four palettes: BG, OBJ0, OBJ1 x 4 shades (lightest first), as 0xRRGGBB.
static const uint32_t k_pal[4][3][4] = {
    {{0xe0f8d0, 0x88c070, 0x346856, 0x081820}, {0xe0f8d0, 0x88c070, 0x346856, 0x081820},
     {0xe0f8d0, 0x88c070, 0x346856, 0x081820}},                      // green (BGB)
    {{0xe8e8e8, 0xa0a0a0, 0x585858, 0x181818}, {0xe8e8e8, 0xa0a0a0, 0x585858, 0x181818},
     {0xe8e8e8, 0xa0a0a0, 0x585858, 0x181818}},                      // grey
    {{0xc4cfa1, 0x8b956d, 0x4d533c, 0x1f1f1f}, {0xc4cfa1, 0x8b956d, 0x4d533c, 0x1f1f1f},
     {0xc4cfa1, 0x8b956d, 0x4d533c, 0x1f1f1f}},                      // pocket
    {{0xffffff, 0xff8484, 0x943a3a, 0x000000}, {0xffffff, 0x63a5ff, 0x0000ff, 0x000000},
     {0xffffff, 0x7bff31, 0x0063c5, 0x000000}},                      // colour (CGB-style)
};
static const char *const k_pal_name[2][4] = {{"GREEN", "GREY", "POCKET", "COLOUR"},
                                             {"VERDE", "GRIGIO", "POCKET", "COLORI"}};

// ---- state ------------------------------------------------------------------------------------
static struct gb_s g_gb;
static struct minigb_apu_ctx g_apu;
static const uint8_t *g_rom;
static size_t g_rom_size;
static uint8_t g_ram[0x20000];                 // cart RAM (MBC5 max: 16 x 8 KB)
static size_t g_save_size;                     // battery-backed bytes to persist (0 = none)
static bool g_ram_dirty;
static int32_t g_ram_touch;                    // nv_millis of the last cart RAM write
static char g_save_path[256];
static bool g_error;
static char g_error_msg[64];

static uint8_t g_fb[GH][GW];                   // palette index 0..11 per pixel
static uint8_t g_prev[GH][GW];                 // what the canvas shows
static bool g_full = true;                     // next blit covers the whole screen
static uint16_t *g_out;                        // pre-scaled band, up to 640x576
static uint16_t g_lut[12];

static int g_scale = 4, g_palette = 0, g_it = 0;
static int g_gx, g_gy, g_gw, g_gh;             // game rect on the canvas
static int g_audio;                            // stream open
static int32_t g_audio_retry;
static int16_t g_pcm[2048];                    // >= AUDIO_SAMPLES_TOTAL (1096 at 32768 Hz)

// control geometry
static int g_dpx, g_dpy, g_dps;                // d-pad centre + size
static int g_ax, g_ay, g_bx, g_by, g_abr;      // A/B centres + radius
static int g_selx, g_stax, g_ssy;              // select/start centres (x) and row y
static int g_menux, g_menuy;                   // MENU key centre
enum { KEY_W = 116, KEY_H = 40 };
static uint8_t g_drawn_keys = 0xFF;            // joypad bits last drawn (active high)
static bool g_drawn_menu;

// ---- Peanut-GB callbacks ----------------------------------------------------------------------
uint8_t audio_read(uint16_t addr) { return minigb_apu_audio_read(&g_apu, addr); }
void audio_write(uint16_t addr, uint8_t val) { minigb_apu_audio_write(&g_apu, addr, val); }

static uint8_t rom_read(struct gb_s *gb, const uint_fast32_t addr) {
    (void)gb;
    return addr < g_rom_size ? g_rom[addr] : 0xFF;
}
static uint8_t cart_ram_read(struct gb_s *gb, const uint_fast32_t addr) {
    (void)gb;
    return addr < sizeof g_ram ? g_ram[addr] : 0xFF;
}
static void cart_ram_write(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val) {
    (void)gb;
    if (addr < sizeof g_ram && g_ram[addr] != val) {
        g_ram[addr] = val;
        g_ram_dirty = true;
        g_ram_touch = nv_millis();
    }
}
static void gb_error_cb(struct gb_s *gb, const enum gb_error_e err, const uint16_t addr) {
    (void)gb;
    if (!g_error) {
        g_error = true;
        snprintf(g_error_msg, sizeof g_error_msg, "EMU ERROR %d AT %04X", (int)err, (unsigned)addr);
        nv_log(NV_LOG_ERROR, g_error_msg);
    }
}
static void lcd_draw_line(struct gb_s *gb, const uint8_t *px, const uint_fast8_t line) {
    (void)gb;
    if (line >= GH) return;
    uint8_t *d = g_fb[line];
    for (int x = 0; x < GW; x++) {
        const uint8_t p = px[x];
        d[x] = (uint8_t)(((p >> 4) & 3) * 4 + (p & 3));   // OBJ0 0-3, OBJ1 4-7, BG 8-11
    }
}

// ---- helpers ----------------------------------------------------------------------------------
static int rgb565(uint32_t c) { return NV_RGB((c >> 16) & 255, (c >> 8) & 255, c & 255); }
static void set_palette(int p) {
    g_palette = p & 3;
    for (int l = 0; l < 3; l++)
        for (int s = 0; s < 4; s++) g_lut[l * 4 + s] = (uint16_t)rgb565(k_pal[g_palette][l][s]);
    g_full = true;
}
static const char *tr(const char *en, const char *it) { return g_it ? it : en; }
static void text_c(int cx, int y, const char *s, int col, int sc) {
    nv_gfx_text(cx - nv_gfx_text_width(s, sc) / 2, y, s, col, sc);
}

// settings (per app): scale + palette
typedef struct { uint32_t magic; int32_t scale, palette; } cfg_t;
#define CFG_MAGIC 0x47424331u   // "GBC1"
static void cfg_load(void) {
    cfg_t c;
    if (nv_load("gb.cfg", &c, sizeof c) == (int32_t)sizeof c && c.magic == CFG_MAGIC) {
        g_scale = c.scale == 3 ? 3 : 4;
        g_palette = c.palette & 3;
    }
}
static void cfg_save(void) {
    cfg_t c = {CFG_MAGIC, g_scale, g_palette};
    nv_save("gb.cfg", &c, sizeof c);
}

// ---- cart RAM persistence -----------------------------------------------------------------------
static void save_flush(void) {
    if (!g_ram_dirty || !g_save_size || !g_save_path[0]) return;
    FILE *f = fopen(g_save_path, "wb");
    if (!f) { nv_log(NV_LOG_WARN, "save: cannot write"); g_ram_dirty = false; return; }
    const size_t n = fwrite(g_ram, 1, g_save_size, f);
    fclose(f);
    g_ram_dirty = false;
    if (n != g_save_size) nv_log(NV_LOG_WARN, "save: short write");
}
static void save_load(void) {
    memset(g_ram, 0xFF, sizeof g_ram);
    if (!g_save_size || !g_save_path[0]) return;
    FILE *f = fopen(g_save_path, "rb");
    if (!f) return;
    const size_t n = fread(g_ram, 1, g_save_size, f);
    fclose(f);
    char b[80];
    snprintf(b, sizeof b, "save: loaded %u of %u bytes", (unsigned)n, (unsigned)g_save_size);
    nv_log(NV_LOG_INFO, b);
}

// ---- layout + static drawing --------------------------------------------------------------------
static void layout(void) {
    g_gw = GW * g_scale; g_gh = GH * g_scale;
    g_gx = (CW - g_gw) / 2; g_gy = (CH - g_gh) / 2;
    const int side = g_gx;                        // 192 (4x) or 272 (3x) px on each side
    g_dps = side - 28 < 200 ? side - 28 : 200;
    g_dpx = side / 2; g_dpy = 300;
    g_abr = 42;
    const int rc = CW - side / 2;
    g_ax = rc + 40; g_ay = 270;
    g_bx = rc - 40; g_by = 340;
    g_selx = side / 2; g_stax = rc; g_ssy = 520;
    g_menux = side / 2; g_menuy = 56;
}

static void draw_key(int cx, int cy, const char *label, bool on) {
    nv_gfx_rect(cx - KEY_W / 2, cy - KEY_H / 2, KEY_W, KEY_H, on ? C_KEY_ON : C_KEY);
    text_c(cx, cy - 7, label, on ? C_BG : C_TEXT, 2);
}
static void draw_dpad(uint8_t k) {
    const int cx = g_dpx, cy = g_dpy, L = g_dps, t = L / 3;
    nv_gfx_circle(cx, cy, L / 2 + 14, C_ZONE);
    nv_gfx_rect(cx - t / 2, cy - L / 2, t, L, C_KEY);
    nv_gfx_rect(cx - L / 2, cy - t / 2, L, t, C_KEY);
    if (k & JOYPAD_UP)    nv_gfx_rect(cx - t / 2, cy - L / 2, t, t, C_KEY_ON);
    if (k & JOYPAD_DOWN)  nv_gfx_rect(cx - t / 2, cy + L / 2 - t, t, t, C_KEY_ON);
    if (k & JOYPAD_LEFT)  nv_gfx_rect(cx - L / 2, cy - t / 2, t, t, C_KEY_ON);
    if (k & JOYPAD_RIGHT) nv_gfx_rect(cx + L / 2 - t, cy - t / 2, t, t, C_KEY_ON);
    const int a = t / 4, o = L / 2 - t / 2;          // arrow heads
    nv_gfx_tri(cx, cy - o - a, cx - a, cy - o + a / 2, cx + a, cy - o + a / 2, C_BG);
    nv_gfx_tri(cx, cy + o + a, cx - a, cy + o - a / 2, cx + a, cy + o - a / 2, C_BG);
    nv_gfx_tri(cx - o - a, cy, cx - o + a / 2, cy - a, cx - o + a / 2, cy + a, C_BG);
    nv_gfx_tri(cx + o + a, cy, cx + o - a / 2, cy - a, cx + o - a / 2, cy + a, C_BG);
}
static void draw_ab(int cx, int cy, const char *l, bool on) {
    nv_gfx_circle(cx, cy, g_abr, on ? C_AB_ON : C_AB);
    text_c(cx, cy - 10, l, C_TEXT, 3);
}
static void draw_controls(uint8_t k, bool menu, bool force) {
    const uint8_t ch = force ? 0xFF : (uint8_t)(k ^ g_drawn_keys);
    if (ch & (JOYPAD_UP | JOYPAD_DOWN | JOYPAD_LEFT | JOYPAD_RIGHT)) draw_dpad(k);
    if (ch & JOYPAD_A) draw_ab(g_ax, g_ay, "A", k & JOYPAD_A);
    if (ch & JOYPAD_B) draw_ab(g_bx, g_by, "B", k & JOYPAD_B);
    if (ch & JOYPAD_SELECT) draw_key(g_selx, g_ssy, "SELECT", k & JOYPAD_SELECT);
    if (ch & JOYPAD_START) draw_key(g_stax, g_ssy, "START", k & JOYPAD_START);
    if (force || menu != g_drawn_menu) draw_key(g_menux, g_menuy, "MENU", menu);
    g_drawn_keys = k;
    g_drawn_menu = menu;
}
static void draw_all(void) {
    nv_gfx_clear(C_BG);
    nv_gfx_rect(g_gx - 10, g_gy - 10 < 0 ? 0 : g_gy - 10, g_gw + 20,
                g_gh + 20 > CH ? CH : g_gh + 20, C_BEZEL);
    draw_controls(0, false, true);
    g_full = true;
}

// ---- video --------------------------------------------------------------------------------------
static void blit_frame(void) {
    int y0 = -1, y1 = -1;
    for (int y = 0; y < GH; y++)
        if (g_full || memcmp(g_fb[y], g_prev[y], GW)) { if (y0 < 0) y0 = y; y1 = y; }
    if (y0 < 0) return;
    const int S = g_scale, W = GW * S;
    uint16_t *o = g_out;
    for (int y = y0; y <= y1; y++) {
        const uint8_t *s = g_fb[y];
        uint16_t *row = o;
        if (S == 4) {
            uint32_t *r32 = (uint32_t *)row;
            for (int x = 0; x < GW; x++) {
                const uint32_t c = g_lut[s[x]];
                const uint32_t cc = c | (c << 16);
                r32[0] = cc; r32[1] = cc; r32 += 2;
            }
        } else {
            for (int x = 0; x < GW; x++) {
                const uint16_t c = g_lut[s[x]];
                row[0] = c; row[1] = c; row[2] = c; row += 3;
            }
        }
        for (int k = 1; k < S; k++) memcpy(o + k * W, o, (size_t)W * 2);
        o += W * S;
        memcpy(g_prev[y], g_fb[y], GW);
    }
    const int rows = (y1 - y0 + 1) * S;
    nv_gfx_blit_raw(g_out, rows * W * 2, g_gx, g_gy + y0 * S, W, rows);
    g_full = false;
}

// ---- audio --------------------------------------------------------------------------------------
// The emulation is paced by the clock (nv_millis) ONLY; audio is a slave that must never stall it.
// Why: the OS audio stream (nv_audio_pcm, MUSIC owner) has a ~0.2 s pre-roll gate — its feeder
// doesn't start draining until that much is queued — so pacing on the backlog with a smaller target
// deadlocked the emulator on the device (backlog stuck, 0 fps). Rules:
//   - at open, queue AUD_PRIME of silence: the pre-roll is passed at once;
//   - a GB frame of samples is queued only while the backlog is under AUD_HI (else dropped), so the
//     latency settles around AUD_HI and nv_audio_write never blocks;
//   - if the backlog stops draining for > 150 ms (a gate or a stuck sink), frames are still queued up
//     to AUD_HARD so a larger pre-roll gets filled — never beyond, never waiting for it;
//   - under AUD_LO (after the write) the last sample is held for 5 ms (the APU yields 803.6 frames per GB frame at
//     48 kHz and we take 803: a tiny deficit that would otherwise underrun every ~20 s).
// 48 kHz: the ES8311/I2S native rate (32768 is not a real codec rate).
#define AUD_MS(ms) ((AUDIO_SAMPLE_RATE * 4 / 1000) * (ms))    // bytes of 16-bit stereo
enum { AUD_PRIME = AUD_MS(220), AUD_HI = AUD_MS(100), AUD_LO = AUD_MS(50), AUD_HARD = AUD_MS(600) };
static int32_t g_aud_prev = -1;                // backlog right after our last write
static int32_t g_aud_drain_t;                  // nv_millis when the backlog was last seen draining
static int32_t g_aud_bl;                       // last backlog read (stats)
static int32_t g_aud_dropped, g_aud_padded;    // stats
static int16_t g_zero[1024];

static void audio_write_or_close(const void *p, int32_t n) {
    if (g_audio && nv_audio_write(p, n) < 0) {
        g_audio = 0;
        nv_log(NV_LOG_WARN, "audio stream lost: playing muted");
    }
}
static void audio_try_open(void) {
    g_audio = nv_audio_open(AUDIO_SAMPLE_RATE, 2) == 1;
    g_audio_retry = nv_millis();
    if (!g_audio) { nv_log(NV_LOG_WARN, "audio stream busy: playing muted"); return; }
    for (int left = AUD_PRIME; left > 0 && g_audio; left -= (int)sizeof g_zero)
        audio_write_or_close(g_zero, left < (int)sizeof g_zero ? left : (int)sizeof g_zero);
    g_aud_prev = -1;
    g_aud_drain_t = nv_millis();
}
static void audio_frame(void) {
    minigb_apu_audio_callback(&g_apu, g_pcm);   // one GB frame of stereo samples (keeps APU time)
    if (!g_audio) return;
    int32_t bl = nv_audio_backlog();
    if (bl < 0) { g_audio = 0; return; }
    const int32_t now = nv_millis();
    if (g_aud_prev < 0 || bl < g_aud_prev) g_aud_drain_t = now;
    const bool stalled = now - g_aud_drain_t > 150;
    g_aud_bl = bl;
    if (bl >= (stalled ? AUD_HARD : AUD_HI)) { g_aud_dropped++; g_aud_prev = bl; return; }
    // Headroom: 4 channels at full volume reach half scale; halve again (USB speakers brown out
    // the board on loud peaks, see apps/pianino).
    for (int i = 0; i < AUDIO_SAMPLES_TOTAL; i++) g_pcm[i] = (int16_t)(g_pcm[i] >> 1);
    audio_write_or_close(g_pcm, AUDIO_SAMPLES_TOTAL * 2);
    bl += AUDIO_SAMPLES_TOTAL * 2;
    if (g_audio && !stalled && bl < AUD_LO) {           // hold the last sample ~5 ms
        static int16_t hold[AUDIO_SAMPLE_RATE / 200 * 2];
        for (int i = 0; i < (int)(sizeof hold / 2); i += 2) {
            hold[i] = g_pcm[AUDIO_SAMPLES_TOTAL - 2];
            hold[i + 1] = g_pcm[AUDIO_SAMPLES_TOTAL - 1];
        }
        audio_write_or_close(hold, (int32_t)sizeof hold);
        bl += (int32_t)sizeof hold;
        g_aud_padded++;
    }
    g_aud_prev = bl;
}

// ---- pacing (clock only) --------------------------------------------------------------------------
static int32_t g_t0;
static int64_t g_done;
static void pace_reset(void) { g_t0 = nv_millis(); g_done = 0; }
static int frames_due(void) {
    const int64_t due = (int64_t)(nv_millis() - g_t0) * GB_FPS_X10000 / 10000000;
    int64_t n = due - g_done;
    if (n > 2 * MAX_STEP) { pace_reset(); return 1; }   // long stall: don't race to catch up
    if (n > MAX_STEP) n = MAX_STEP;
    g_done += n;
    return (int)n;
}

// ---- input --------------------------------------------------------------------------------------
typedef struct { uint8_t keys; bool menu; int tap_x, tap_y; bool tap; } input_t;
static int g_touch_prev;   // fingers down last poll (for tap edges)

static bool in_rect(int x, int y, int cx, int cy, int w, int h) {
    return x >= cx - w / 2 && x < cx + w / 2 && y >= cy - h / 2 && y < cy + h / 2;
}
static int dist2(int x, int y, int cx, int cy) { return (x - cx) * (x - cx) + (y - cy) * (y - cy); }

static uint32_t g_pad_prev;
static void read_input(input_t *in) {
    memset(in, 0, sizeof *in);
    const int n = nv_touch_count();
    for (int i = 0; i < n && i < 5; i++) {
        int x, y;
        if (!nv_touch_at(i, &x, &y)) continue;
        if (i == 0 && g_touch_prev == 0) { in->tap = true; in->tap_x = x; in->tap_y = y; }
        const int R = g_dps / 2 + 40;
        if (dist2(x, y, g_dpx, g_dpy) <= R * R && x < g_gx + 24) {
            const int dx = x - g_dpx, dy = y - g_dpy, dead = g_dps / 10;
            const int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
            if (ax > dead && ax * 2 >= ay) in->keys |= dx < 0 ? JOYPAD_LEFT : JOYPAD_RIGHT;
            if (ay > dead && ay * 2 >= ax) in->keys |= dy < 0 ? JOYPAD_UP : JOYPAD_DOWN;
        } else if (dist2(x, y, g_ax, g_ay) <= (g_abr + 18) * (g_abr + 18)) {
            in->keys |= JOYPAD_A;
        } else if (dist2(x, y, g_bx, g_by) <= (g_abr + 18) * (g_abr + 18)) {
            in->keys |= JOYPAD_B;
        } else if (in_rect(x, y, g_selx, g_ssy, KEY_W + 30, KEY_H + 36)) {
            in->keys |= JOYPAD_SELECT;
        } else if (in_rect(x, y, g_stax, g_ssy, KEY_W + 30, KEY_H + 36)) {
            in->keys |= JOYPAD_START;
        } else if (in_rect(x, y, g_menux, g_menuy, KEY_W + 30, KEY_H + 36)) {
            in->menu = true;
        } else if (x > g_gx + g_gw && y > g_ay - 110 && y < g_by + 80) {
            // between/around A and B on the right panel: the closer one (rolling thumb)
            in->keys |= dist2(x, y, g_ax, g_ay) < dist2(x, y, g_bx, g_by) ? JOYPAD_A : JOYPAD_B;
        }
    }
    g_touch_prev = n;
    // USB keyboard + every controller merged (SNES layout, sticks included by the OS)
    const uint32_t p = (uint32_t)nv_gfx_pad();
    if (p & NV_PAD_UP) in->keys |= JOYPAD_UP;
    if (p & NV_PAD_DOWN) in->keys |= JOYPAD_DOWN;
    if (p & NV_PAD_LEFT) in->keys |= JOYPAD_LEFT;
    if (p & NV_PAD_RIGHT) in->keys |= JOYPAD_RIGHT;
    if (p & (NV_PAD_A | NV_PAD_Y)) in->keys |= JOYPAD_A;
    if (p & (NV_PAD_B | NV_PAD_X)) in->keys |= JOYPAD_B;
    if (p & NV_PAD_START) in->keys |= JOYPAD_START;
    if (p & NV_PAD_SELECT) in->keys |= JOYPAD_SELECT;
    uint32_t menu_bits = p & (NV_PAD_L | NV_PAD_R);
    // left stick + Guide per controller (ABI 11)
    const int pads = nv_pad_count();
    for (int i = 0; i < pads && i < 4; i++) {
        nv_pad_state_t st;
        if (nv_pad_state(i, &st, sizeof st) <= 0) continue;
        if (st.lx < -16000) in->keys |= JOYPAD_LEFT;
        if (st.lx > 16000) in->keys |= JOYPAD_RIGHT;
        if (st.ly < -16000) in->keys |= JOYPAD_UP;
        if (st.ly > 16000) in->keys |= JOYPAD_DOWN;
        if (st.buttons & NV_PADB_GUIDE) menu_bits |= 1u << 31;
    }
    // menu from a pad opens on the press edge only
    if (menu_bits & ~g_pad_prev) in->menu = true;
    g_pad_prev = menu_bits;
}

// ---- pause menu ---------------------------------------------------------------------------------
enum { M_RESUME, M_RESET, M_ZOOM, M_PAL, M_EXIT, M_N };
static int menu_item_y(int i, int *h) {
    const int ih = g_scale == 4 ? 80 : 62, top = g_gy + (g_gh - (M_N * ih + 70)) / 2 + 70;
    *h = ih - 12;
    return top + i * ih;
}
static void draw_menu(int sel) {
    nv_gfx_rect(g_gx, g_gy, g_gw, g_gh, C_PANEL);
    int h;
    const int y0 = menu_item_y(0, &h);
    text_c(g_gx + g_gw / 2, y0 - 56, g_error ? g_error_msg : GB_TITLE, g_error ? C_AB_ON : C_DIM, 3);
    char b[48];
    for (int i = 0; i < M_N; i++) {
        const int y = menu_item_y(i, &h);
        const char *s = "";
        switch (i) {
        case M_RESUME: s = tr("RESUME", "CONTINUA"); break;
        case M_RESET: s = tr("RESET", "RICOMINCIA"); break;
        case M_ZOOM: snprintf(b, sizeof b, "ZOOM %dX", g_scale); s = b; break;
        case M_PAL: snprintf(b, sizeof b, "%s %s", tr("COLOURS", "COLORI"), k_pal_name[g_it][g_palette]);
                    s = b; break;
        case M_EXIT:
#ifdef GB_GENERIC
            s = tr("OTHER GAME", "ALTRO GIOCO");
#else
            s = tr("EXIT", "ESCI");
#endif
            break;
        }
        nv_gfx_rect(g_gx + 40, y, g_gw - 80, h, i == sel ? C_HI : C_KEY);
        text_c(g_gx + g_gw / 2, y + h / 2 - 10, s, C_TEXT, 3);
    }
}
static int menu_hit(int x, int y) {
    for (int i = 0; i < M_N; i++) {
        int h;
        const int yy = menu_item_y(i, &h);
        if (x >= g_gx + 40 && x < g_gx + g_gw - 40 && y >= yy && y < yy + h) return i;
    }
    return -1;
}

// ---- messages -----------------------------------------------------------------------------------
static bool wait_ack(void) {   // until a tap, a pad button or back; 0 when the OS closes the app
    int prev_t = 1;
    uint32_t prev_p = 0xFFFFFFFFu;
    while (nv_gfx_present()) {
        if (nv_gfx_back()) return true;
        const int t = nv_touch_count();
        const uint32_t p = (uint32_t)nv_gfx_pad() & 0xFFF;
        if ((t && !prev_t) || (p & ~prev_p)) return true;
        prev_t = t; prev_p = p;
    }
    return false;
}
static bool message(const char *l1, const char *l2) {
    nv_gfx_persist(1);
    nv_gfx_clear(C_BG);
    text_c(CW / 2, 230, l1, C_TEXT, 4);
    if (l2) text_c(CW / 2, 300, l2, C_DIM, 3);
    text_c(CW / 2, 520, tr("TAP TO CONTINUE", "TOCCA PER CONTINUARE"), C_DIM, 2);
    return wait_ack();
}

// ---- one game session ---------------------------------------------------------------------------
// Returns 0 when the whole app must close (OS stop / exit), 1 to go back to the picker.
static int play(void) {
    if (g_rom_size < 0x150) return message(tr("INVALID ROM", "ROM NON VALIDA"), NULL);
    if (g_rom[0x143] == 0xC0)
        return message(tr("GAME BOY COLOR ONLY", "SOLO GAME BOY COLOR"),
                       tr("THIS GAME NEEDS A COLOR CONSOLE", "SERVE UNA CONSOLE A COLORI"));
    g_error = false;
    const enum gb_init_error_e ie = gb_init(&g_gb, rom_read, cart_ram_read, cart_ram_write, gb_error_cb, NULL);
    if (ie != GB_INIT_NO_ERROR)
        return message(ie == GB_INIT_CARTRIDGE_UNSUPPORTED ? tr("CARTRIDGE NOT SUPPORTED", "CARTUCCIA NON SUPPORTATA")
                                                           : tr("BAD ROM CHECKSUM", "CHECKSUM ROM ERRATO"),
                       NULL);
    size_t ss = 0;
    g_save_size = gb_get_save_size_s(&g_gb, &ss) == 0 && ss <= sizeof g_ram ? ss : 0;
    save_load();
    g_ram_dirty = false;
    gb_init_lcd(&g_gb, lcd_draw_line);
    minigb_apu_audio_init(&g_apu);
    {
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        if (tm) gb_set_rtc(&g_gb, tm);
    }
    {
        char name[20], b[96];
        gb_get_rom_name(&g_gb, name);
        snprintf(b, sizeof b, "rom '%s' %u KB, mbc %d, save %u B, audio %d Hz", name,
                 (unsigned)(g_rom_size / 1024), (int)g_gb.mbc, (unsigned)g_save_size, AUDIO_SAMPLE_RATE);
        nv_log(NV_LOG_INFO, b);
    }
    nv_gfx_persist(1);
    layout();
    set_palette(g_palette);
    memset(g_fb, 0, sizeof g_fb);
    draw_all();
    if (!g_audio) audio_try_open();
    pace_reset();

    bool paused = false, prev_menu = false;
    int sel = 0, rc = 0;
    uint8_t prev_keys = 0;
    int32_t stat_t = nv_millis(), stat_frames = 0, stat_shown = 0;
    while (nv_gfx_present()) {
        input_t in;
        read_input(&in);
        const bool back = nv_gfx_back() > 0;
        const bool menu_edge = (in.menu && !prev_menu) || back;
        prev_menu = in.menu;

        if (!paused && (menu_edge || g_error)) {
            paused = true; sel = 0;
            save_flush();
            draw_controls(0, true, false);
            draw_menu(sel);
            prev_keys = in.keys;
            continue;
        }
        if (paused) {
            if (back) {                       // back twice: leave the game
#ifdef GB_GENERIC
                rc = 1;
#endif
                break;
            }
            const uint8_t edge = (uint8_t)(in.keys & ~prev_keys);
            prev_keys = in.keys;
            int act = -1;
            if (edge & JOYPAD_UP) { sel = (sel + M_N - 1) % M_N; draw_menu(sel); }
            if (edge & JOYPAD_DOWN) { sel = (sel + 1) % M_N; draw_menu(sel); }
            if (edge & (JOYPAD_A | JOYPAD_START)) act = sel;
            if ((edge & JOYPAD_B) || (menu_edge && !back)) act = M_RESUME;
            if (in.tap) {
                const int h = menu_hit(in.tap_x, in.tap_y);
                if (h >= 0) act = h;
            }
            if (act == M_RESUME && g_error) act = -1;   // a crashed core can only reset/exit
            switch (act) {
            case M_RESUME:
                paused = false;
                draw_all();
                memset(g_prev, 0xFF, sizeof g_prev);
                blit_frame();
                pace_reset();
                break;
            case M_RESET:
                save_flush();
                gb_reset(&g_gb);
                minigb_apu_audio_init(&g_apu);
                g_error = false;
                paused = false;
                draw_all();
                pace_reset();
                break;
            case M_ZOOM:
                g_scale = g_scale == 4 ? 3 : 4;
                cfg_save();
                layout();
                draw_all();
                draw_controls(0, true, false);
                draw_menu(sel);
                break;
            case M_PAL:
                set_palette(g_palette + 1);
                cfg_save();
                draw_menu(sel);
                break;
            case M_EXIT:
#ifdef GB_GENERIC
                rc = 1;
#else
                rc = 0;
#endif
                goto out;
            default: break;
            }
            continue;
        }

        g_gb.direct.joypad = (uint8_t)~in.keys;
        if (in.keys != g_drawn_keys || in.menu != g_drawn_menu) draw_controls(in.keys, in.menu, false);

        const int n = frames_due();
        for (int i = 0; i < n && !g_error; i++) {
            g_gb.display.lcd_draw_line = i == n - 1 ? lcd_draw_line : NULL;   // frame skip
            gb_run_frame(&g_gb);
            audio_frame();
        }
        g_gb.display.lcd_draw_line = lcd_draw_line;
        if (n > 0) {
            if (!(g_gb.hram_io[IO_LCDC] & LCDC_ENABLE)) memset(g_fb, 8, sizeof g_fb);   // LCD off: blank
            blit_frame();
            stat_shown++;
        }
        stat_frames += n;
        if (!g_audio && nv_millis() - g_audio_retry > 3000) audio_try_open();
        if (g_ram_dirty && nv_millis() - g_ram_touch > 1500) save_flush();
        const int32_t now = nv_millis();
        if (now - stat_t >= 10000) {
            char b[128];
            snprintf(b, sizeof b, "%d.%d emu fps, %d.%d shown fps, audio %s backlog %d ms, dropped %d, padded %d",
                     (int)(stat_frames * 10000LL / (now - stat_t)) / 10, (int)(stat_frames * 10000LL / (now - stat_t)) % 10,
                     (int)(stat_shown * 10000LL / (now - stat_t)) / 10, (int)(stat_shown * 10000LL / (now - stat_t)) % 10,
                     g_audio ? "on" : "off", (int)(g_aud_bl / AUD_MS(1)), (int)g_aud_dropped, (int)g_aud_padded);
            g_aud_dropped = g_aud_padded = 0;
            nv_log(NV_LOG_INFO, b);
            stat_t = now; stat_frames = 0; stat_shown = 0;
        }
    }
out:
    save_flush();
    return rc;
}

// ---- generic app: ROM picker --------------------------------------------------------------------
#ifdef GB_GENERIC
#define ROM_DIR "/roms"
enum { MAX_ROMS = 200, NAME_LEN = 64, ROWS = 9, ROW_H = 50, LIST_Y = 110 };
static char g_names[MAX_ROMS][NAME_LEN];
static int g_nroms;

static bool has_ext(const char *n) {
    const char *d = strrchr(n, '.');
    if (!d) return false;
    char e[5] = {0};
    for (int i = 0; i < 4 && d[1 + i]; i++) e[i] = (char)(d[1 + i] | 0x20);
    return !strcmp(e, "gb") || !strcmp(e, "gbc") || !strcmp(e, "sgb");
}
static int cmp_names(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }
static void scan_roms(void) {
    g_nroms = 0;
    DIR *d = opendir(GB_FS_ROOT ROM_DIR);
    if (!d) { mkdir(GB_FS_ROOT ROM_DIR, 0777); return; }
    struct dirent *e;
    while ((e = readdir(d)) && g_nroms < MAX_ROMS) {
        if (e->d_name[0] == '.' || !has_ext(e->d_name) || strlen(e->d_name) >= NAME_LEN) continue;
        strcpy(g_names[g_nroms++], e->d_name);
    }
    closedir(d);
    qsort(g_names, (size_t)g_nroms, NAME_LEN, cmp_names);
}
static void shown_name(char *o, const char *n, int max) {   // font: A-Z 0-9 space - . : ! + etc.
    int i = 0;
    for (; n[i] && i < max; i++) o[i] = (n[i] == '_') ? ' ' : n[i];
    o[i] = 0;
    char *d = strrchr(o, '.');
    if (d && has_ext(n)) *d = 0;
}
static void draw_picker(int top, int sel) {
    nv_gfx_clear(C_BG);
    nv_gfx_text(40, 30, "GAME BOY", C_TEXT, 5);
    nv_gfx_text(40, 76, tr("GAMES IN /SDCARD/HOME/ROMS", "GIOCHI IN /SDCARD/HOME/ROMS"), C_DIM, 2);
    draw_key(CW - 90, 50, tr("EXIT", "ESCI"), false);
    if (!g_nroms) {
        text_c(CW / 2, 250, tr("NO GAMES FOUND", "NESSUN GIOCO TROVATO"), C_TEXT, 4);
        text_c(CW / 2, 320, tr("COPY .GB FILES TO /SDCARD/HOME/ROMS", "COPIA I FILE .GB IN /SDCARD/HOME/ROMS"), C_DIM, 2);
        text_c(CW / 2, 350, tr("WITH THE FILES APP OR THE WEB COMPANION", "CON L'APP FILE O IL WEB COMPANION"), C_DIM, 2);
        return;
    }
    char b[NAME_LEN];
    for (int r = 0; r < ROWS && top + r < g_nroms; r++) {
        const int y = LIST_Y + r * ROW_H;
        nv_gfx_rect(40, y, CW - 200, ROW_H - 8, top + r == sel ? C_HI : C_ZONE);
        shown_name(b, g_names[top + r], 46);
        nv_gfx_text(56, y + 11, b, C_TEXT, 3);
    }
    if (top > 0) draw_key(CW - 90, LIST_Y + 60, "UP", false);
    if (top + ROWS < g_nroms) draw_key(CW - 90, LIST_Y + ROWS * ROW_H - 60, "DOWN", false);
    char pg[32];
    snprintf(pg, sizeof pg, "%d/%d", sel + 1, g_nroms);
    text_c(CW - 90, LIST_Y + ROWS * ROW_H / 2 - 7, pg, C_DIM, 2);
}
// Returns the index of the chosen ROM, -1 to exit the app.
static int picker(void) {
    static int sel = 0;
    nv_gfx_persist(1);
    scan_roms();
    if (sel >= g_nroms) sel = 0;
    int top = sel - sel % ROWS;
    draw_picker(top, sel);
    uint8_t prev = 0xFF;
    int prev_t = 1;
    while (nv_gfx_present()) {
        if (nv_gfx_back()) return -1;
        const uint32_t p = (uint32_t)nv_gfx_pad();
        uint8_t k = 0;
        if (p & NV_PAD_UP) k |= 1;
        if (p & NV_PAD_DOWN) k |= 2;
        if (p & (NV_PAD_A | NV_PAD_START)) k |= 4;
        if (p & NV_PAD_B) k |= 8;
        const uint8_t e = (uint8_t)(k & ~prev);
        prev = k;
        int nsel = sel;
        if (e & 1) nsel--;
        if (e & 2) nsel++;
        if ((e & 4) && g_nroms) return sel;
        if (e & 8) return -1;
        int x, y;
        const int t = nv_touch(&x, &y);
        if (t && !prev_t) {
            if (in_rect(x, y, CW - 90, 50, KEY_W + 20, KEY_H + 30)) return -1;
            if (x < CW - 160 && y >= LIST_Y && y < LIST_Y + ROWS * ROW_H) {
                const int i = top + (y - LIST_Y) / ROW_H;
                if (i < g_nroms) return sel = i;
            }
            if (in_rect(x, y, CW - 90, LIST_Y + 60, KEY_W + 20, 100)) nsel = top - ROWS;
            if (in_rect(x, y, CW - 90, LIST_Y + ROWS * ROW_H - 60, KEY_W + 20, 100)) nsel = top + ROWS;
        }
        prev_t = t;
        if (nsel < 0) nsel = 0;
        if (nsel >= g_nroms) nsel = g_nroms - 1;
        if (nsel != sel && g_nroms) {
            sel = nsel;
            top = sel - sel % ROWS;
            draw_picker(top, sel);
        }
    }
    return -2;   // OS stop
}
static uint8_t *load_rom(const char *name, size_t *size) {
    char path[160];
    snprintf(path, sizeof path, GB_FS_ROOT ROM_DIR "/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = (n >= 0x150 && n <= 4L * 1024 * 1024) ? malloc((size_t)n) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    *size = b ? (size_t)n : 0;
    return b;
}
#endif

// ---- entry --------------------------------------------------------------------------------------
NV_EXPORT("run")
void run(void) {
    char lang[8] = {0};
    nv_lang(lang, sizeof lang);
    g_it = lang[0] == 'i' && lang[1] == 't';
    cfg_load();
    g_out = malloc((size_t)GW * 4 * GH * 4 * 2);
    if (!g_out) { message("OUT OF MEMORY", NULL); return; }
#ifdef GB_GENERIC
    for (;;) {
        const int i = picker();
        if (i < 0) break;
        size_t n = 0;
        uint8_t *rom = load_rom(g_names[i], &n);
        if (!rom) {
            if (!message(tr("CANNOT READ THE GAME", "IMPOSSIBILE LEGGERE IL GIOCO"),
                         tr("MAX 4 MB", "MASSIMO 4 MB"))) break;
            continue;
        }
        g_rom = rom; g_rom_size = n;
        snprintf(g_save_path, sizeof g_save_path, GB_FS_ROOT ROM_DIR "/%s", g_names[i]);
        char *dot = strrchr(g_save_path, '.');
        if (dot) strcpy(dot, ".sav");
        const int rc = play();
        free(rom);
        g_rom = NULL;
        if (!rc) break;
    }
#else
    g_rom = gb_rom_data;
    g_rom_size = GB_ROM_SIZE;
    snprintf(g_save_path, sizeof g_save_path, GB_FS_ROOT "/game.sav");
    play();
#endif
    if (g_audio) nv_audio_close();
    g_audio = 0;
    free(g_out);
    g_out = NULL;
}
