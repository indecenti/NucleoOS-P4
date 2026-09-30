// nvab.cpp — NucleoOS platform for Arduboy sketches (clean-room; part of ports/arduboy, BSD-3-Clause).
//
// The sketch's setup()/loop() run inside the app's run() export. Everything the Arduboy hardware
// did is done here:
//   display   the 128x64 1-bit SSD1306 page buffer becomes RGB565, scaled 5x/6x/7x on the 1024x600
//             canvas in persist mode (ABI 6): each display() blits only the rectangle of pixels
//             that changed, then presents. Palettes: white, amber, green, blue OLED, grey LCD.
//   input     multitouch on-screen D-pad (left) and A/B (right), USB/BLE pads and USB keyboard
//             (nv_gfx_pad + left stick via nv_pad_state, ABI 11).
//   time      millis()/delay() on nv_millis; the frame pacing is the sketch's own (nextFrame);
//             idle() naps 1 ms. Time spent in the pause menu is hidden from the sketch.
//   sound     up to 6 square-wave voices mixed into the ABI 10 PCM stream (48 kHz stereo). Audio is
//             a slave of the clock: ~220 ms of silence primes the sink's pre-roll, then each service
//             renders the elapsed milliseconds and drops them if > 100 ms are queued. Sequencers
//             (ArduboyTones, Playtune scores) tick once per generated millisecond.
//   EEPROM    1 KB image saved as "eeprom.bin" (nv_save) 1.5 s after the last write and on exit.
//   menu      MENU key / pad L,R,Guide,Select / OS back gesture: resume, zoom, colours, sound, exit.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nucleo_sdk.h"
#include "nvab.h"
#include "Arduino.h"
#include "EEPROM.h"
#include "SPI.h"

// generated per game (nvab_meta.cpp): the pause-menu title, and 1 = two-frame grey blending
extern "C" const char nvab_title[];
extern "C" const int nvab_blend;

#ifdef NV_SIM
extern "C" void nv_sim_frame(const uint8_t *buf);   // harness hook: the last shown 1-bit frame
#endif

namespace nvab {

enum { CW = 1024, CH = 600, AW = 128, AH = 64 };

// ---- colours ----------------------------------------------------------------------------------
#define C_BG NV_RGB(20, 22, 28)
#define C_BEZEL NV_RGB(44, 47, 56)
#define C_ZONE NV_RGB(30, 33, 41)
#define C_KEY NV_RGB(70, 74, 88)
#define C_KEY_ON NV_RGB(140, 200, 160)
#define C_AB NV_RGB(60, 64, 78)
#define C_AB_ON NV_RGB(150, 205, 170)
#define C_TEXT NV_RGB(215, 218, 226)
#define C_DIM NV_RGB(130, 134, 146)
#define C_PANEL NV_RGB(14, 16, 22)
#define C_HI NV_RGB(64, 120, 90)

struct palette_t { uint32_t on, off; const char *en, *it; };
static const palette_t k_pal[] = {
    {0xEEF3FF, 0x05070C, "WHITE", "BIANCO"},
    {0xFFB02E, 0x120A02, "AMBER", "AMBRA"},
    {0x7CF29A, 0x03100A, "GREEN", "VERDE"},
    {0x6FD3FF, 0x020A14, "BLUE", "BLU"},
    {0x23291E, 0xAEB89A, "LCD", "LCD"},
};
enum { NPAL = sizeof k_pal / sizeof k_pal[0] };

// ---- state --------------------------------------------------------------------------------------
static bool g_booted;
static int g_it;                                // UI language: Italian
static int g_scale = 6, g_pal = 0;
static bool g_muted, g_grid = true;
static int g_gx, g_gy, g_gw, g_gh;
static uint16_t g_on, g_off, g_on_edge, g_mid, g_mid_edge;
static uint16_t *g_out;                         // scaled blit buffer (up to 896x448)
static uint8_t g_cur[AW * AH / 8];              // last frame the sketch painted
static uint8_t g_shown[AW * AH / 8];            // what the canvas shows (after invert/all-on)
static bool g_full = true;
static bool g_invert, g_all_on;
static int32_t g_t0, g_paused_ms;
static int32_t g_last_present, g_last_service, g_last_input;
static bool g_in_service;
static uint8_t g_keys;                          // BTN_* from the last input read
static bool g_menu_key, g_menu_req;
static uint32_t g_pad_prev;
static int g_touch_prev;
static bool g_drawn_menu;
static uint8_t g_drawn_keys = 0xFF;
static int32_t g_stat_t, g_stat_frames;

// control geometry
static bool g_ctrl_visible;
static int g_dpx, g_dpy, g_dps;                  // d-pad centre + size
static int g_ax, g_ay, g_bx, g_by, g_abr;        // A/B centres + radius
static int g_menux, g_menuy, g_menuw;            // MENU key
enum { KEY_H = 40 };

static const char *tr(const char *en, const char *it) { return g_it ? it : en; }
static void text_c(int cx, int y, const char *s, int col, int sc) {
    nv_gfx_text(cx - nv_gfx_text_width(s, sc) / 2, y, s, col, sc);
}
static int rgb565(uint32_t c) { return NV_RGB((c >> 16) & 255, (c >> 8) & 255, c & 255); }
static uint32_t mix(uint32_t a, uint32_t b, int t256) {
    uint32_t r = 0;
    for (int s = 0; s < 24; s += 8) {
        const int ca = (a >> s) & 255, cb = (b >> s) & 255;
        r |= (uint32_t)((ca * (256 - t256) + cb * t256) >> 8) << s;
    }
    return r;
}

// ---- settings -----------------------------------------------------------------------------------
struct cfg_t { uint32_t magic; int32_t scale, pal, muted, grid; };
#define CFG_MAGIC 0x4E564142u   // "NVAB"
static bool cfg_load() {
    cfg_t c;
    if (nv_load("ab.cfg", &c, sizeof c) != (int32_t)sizeof c || c.magic != CFG_MAGIC) return false;
    g_scale = (c.scale >= 5 && c.scale <= 7) ? c.scale : 6;
    g_pal = (c.pal >= 0 && c.pal < NPAL) ? c.pal : 0;
    g_muted = c.muted != 0;
    g_grid = c.grid != 0;
    return true;
}
static void cfg_save() {
    cfg_t c = {CFG_MAGIC, g_scale, g_pal, g_muted, g_grid};
    nv_save("ab.cfg", &c, sizeof c);
}

// ---- EEPROM -------------------------------------------------------------------------------------
static uint8_t g_ee[1024];
static bool g_ee_dirty;
static int32_t g_ee_touch;
static void ee_load() {
    if (nv_load("eeprom.bin", g_ee, sizeof g_ee) == (int32_t)sizeof g_ee) return;
    memset(g_ee, 0xFF, sizeof g_ee);
    memset(g_ee, 0, 16);   // Arduboy system area: version 0, no boot-logo flags, no unit name
    g_ee[2] = 1;           // audio on (EEPROM_AUDIO_ON_OFF / eepromAudioOnOff)
}
static void ee_flush() {
    if (!g_ee_dirty) return;
    g_ee_dirty = false;
    if (!nv_save("eeprom.bin", g_ee, sizeof g_ee)) nv_log(NV_LOG_WARN, "eeprom: save failed");
}
uint8_t ee_read(uint16_t a) { return g_ee[a & 1023]; }
void ee_write(uint16_t a, uint8_t v) {
    a &= 1023;
    if (g_ee[a] == v) return;
    g_ee[a] = v;
    g_ee_dirty = true;
    g_ee_touch = nv_millis();
}

// ---- audio --------------------------------------------------------------------------------------
enum { RATE = 48000, SPMS = RATE / 1000 };
#define AUD_MS(ms) (RATE * 4 / 1000 * (ms))      // bytes of 16-bit stereo
enum { AUD_PRIME = AUD_MS(220), AUD_HI = AUD_MS(100), AUD_HARD = AUD_MS(600) };
struct voice_t { uint32_t phase, inc; int16_t amp; bool on; };
static voice_t g_voice[V_COUNT];
static ticker_fn g_tickers[8];
static int g_nticker;
static bool g_audio, g_audio_wanted;
static int32_t g_audio_retry, g_gen_t;
static int32_t g_aud_prev = -1, g_aud_drain_t, g_aud_bl, g_aud_dropped;
static int16_t g_pcm[SPMS * 2 * 20];            // up to 20 ms per render chunk
static int32_t g_lp;                             // one-pole low-pass state

void voice_on(int v, float f, int vol) {
    if (v < 0 || v >= V_COUNT) return;
    if (f < 20.f || vol == VOL_OFF) { g_voice[v].on = false; return; }
    if (f > 12000.f) f = 12000.f;
    g_voice[v].inc = (uint32_t)(f * (4294967296.0f / RATE));
    g_voice[v].amp = vol == VOL_HIGH ? 3600 : 2400;
    g_voice[v].on = true;
    g_audio_wanted = true;
}
void voice_off(int v) { if (v >= 0 && v < V_COUNT) g_voice[v].on = false; }
bool voice_active(int v) { return v >= 0 && v < V_COUNT && g_voice[v].on; }
void add_ticker(ticker_fn f) {
    for (int i = 0; i < g_nticker; i++) if (g_tickers[i] == f) return;
    if (g_nticker < 8) g_tickers[g_nticker++] = f;
}

static void audio_write_or_close(const void *p, int32_t n) {
    if (g_audio && nv_audio_write(p, n) < 0) {
        g_audio = false;
        nv_log(NV_LOG_WARN, "audio stream lost: playing muted");
    }
}
static void audio_open() {
    g_audio_retry = nv_millis();
    g_audio = nv_audio_open(RATE, 2) == 1;
    if (!g_audio) { nv_log(NV_LOG_WARN, "audio stream busy: playing muted"); return; }
    static const int16_t zero[512] = {0};
    for (int left = AUD_PRIME; left > 0 && g_audio; left -= (int)sizeof zero)
        audio_write_or_close(zero, left < (int)sizeof zero ? left : (int)sizeof zero);
    g_aud_prev = -1;
    g_aud_drain_t = nv_millis();
}
static void render_ms(int16_t *out) {   // one millisecond: tickers, then 48 stereo frames
    for (int i = 0; i < g_nticker; i++) g_tickers[i]();
    for (int s = 0; s < SPMS; s++) {
        int32_t acc = 0;
        for (int v = 0; v < V_COUNT; v++) {
            voice_t &vo = g_voice[v];
            if (!vo.on) continue;
            vo.phase += vo.inc;
            acc += (vo.phase & 0x80000000u) ? vo.amp : -vo.amp;
        }
        if (acc > 7000) acc = 7000;
        if (acc < -7000) acc = -7000;
        g_lp += ((acc << 8) - g_lp) * 3 / 4;     // soften the edges a little
        const int16_t y = (int16_t)(g_lp >> 8);
        out[2 * s] = y;
        out[2 * s + 1] = y;
    }
}
static void audio_pump() {
    const int32_t now = nv_millis();
    int32_t ms = now - g_gen_t;
    if (ms <= 0) return;
    g_gen_t = now;
    if (ms > 100) ms = 100;                      // a long stall: don't burst
    if (g_audio_wanted && !g_audio && !g_muted && now - g_audio_retry > 3000) audio_open();
    bool write = g_audio && !g_muted;
    if (write) {
        const int32_t bl = nv_audio_backlog();
        if (bl < 0) { g_audio = false; write = false; }
        else {
            if (g_aud_prev < 0 || bl < g_aud_prev) g_aud_drain_t = now;
            const bool stalled = now - g_aud_drain_t > 150;
            g_aud_bl = bl;
            if (bl >= (stalled ? AUD_HARD : AUD_HI)) { write = false; g_aud_dropped++; }
            g_aud_prev = bl;
        }
    }
    while (ms > 0) {
        const int n = ms > 20 ? 20 : ms;
        for (int i = 0; i < n; i++) render_ms(g_pcm + i * SPMS * 2);
        if (write) {
            audio_write_or_close(g_pcm, n * SPMS * 4);
            g_aud_prev += n * SPMS * 4;
        }
        ms -= n;
    }
}

// ---- layout + static drawing --------------------------------------------------------------------
static void set_colors() {
    const palette_t &p = k_pal[g_pal];
    g_on = (uint16_t)rgb565(p.on);
    g_off = (uint16_t)rgb565(p.off);
    g_on_edge = (uint16_t)rgb565(mix(p.on, p.off, 90));
    g_mid = (uint16_t)rgb565(mix(p.on, p.off, 128));
    g_mid_edge = (uint16_t)rgb565(mix(p.on, p.off, 170));
}
static void layout() {
    g_gw = AW * g_scale;
    g_gh = AH * g_scale;
    g_gx = (CW - g_gw) / 2;
    g_gy = (CH - g_gh) / 2;
    g_ctrl_visible = g_scale < 7;
    const int side = g_ctrl_visible ? g_gx : 200;   // 7x: invisible zones of a 200 px column
    g_dps = side - 24 < 190 ? side - 24 : 190;
    g_dpx = side / 2;
    g_dpy = 330;
    g_abr = side / 2 - 12 < 44 ? side / 2 - 12 : 44;
    const int rc = CW - side / 2, off = side < 160 ? side / 7 : side / 5;
    g_ax = rc - off; g_ay = 360;                    // A lower left, B upper right (as on the Arduboy)
    g_bx = rc + off; g_by = 262;
    if (g_ctrl_visible) {
        g_menuw = side - 24 < 116 ? side - 24 : 116;
        g_menux = side / 2; g_menuy = 56;
    } else {
        g_menuw = 116;
        g_menux = 8 + 58; g_menuy = g_gy / 2;
    }
}
static void draw_key(int cx, int cy, int w, const char *label, bool on) {
    nv_gfx_rect(cx - w / 2, cy - KEY_H / 2, w, KEY_H, on ? C_KEY_ON : C_KEY);
    text_c(cx, cy - 7, label, on ? C_BG : C_TEXT, 2);
}
static void draw_dpad(uint8_t k) {
    const int cx = g_dpx, cy = g_dpy, L = g_dps, t = L / 3;
    nv_gfx_circle(cx, cy, L / 2 + 12, C_ZONE);
    nv_gfx_rect(cx - t / 2, cy - L / 2, t, L, C_KEY);
    nv_gfx_rect(cx - L / 2, cy - t / 2, L, t, C_KEY);
    if (k & BTN_UP) nv_gfx_rect(cx - t / 2, cy - L / 2, t, t, C_KEY_ON);
    if (k & BTN_DOWN) nv_gfx_rect(cx - t / 2, cy + L / 2 - t, t, t, C_KEY_ON);
    if (k & BTN_LEFT) nv_gfx_rect(cx - L / 2, cy - t / 2, t, t, C_KEY_ON);
    if (k & BTN_RIGHT) nv_gfx_rect(cx + L / 2 - t, cy - t / 2, t, t, C_KEY_ON);
    const int a = t / 4, o = L / 2 - t / 2;
    nv_gfx_tri(cx, cy - o - a, cx - a, cy - o + a / 2, cx + a, cy - o + a / 2, C_BG);
    nv_gfx_tri(cx, cy + o + a, cx - a, cy + o - a / 2, cx + a, cy + o - a / 2, C_BG);
    nv_gfx_tri(cx - o - a, cy, cx - o + a / 2, cy - a, cx - o + a / 2, cy + a, C_BG);
    nv_gfx_tri(cx + o + a, cy, cx + o - a / 2, cy - a, cx + o - a / 2, cy + a, C_BG);
}
static void draw_ab(int cx, int cy, const char *l, bool on) {
    nv_gfx_circle(cx, cy, g_abr, on ? C_AB_ON : C_AB);
    text_c(cx, cy - 10, l, on ? C_BG : C_TEXT, 3);
}
static void draw_controls(uint8_t k, bool menu, bool force) {
    if (!g_ctrl_visible) {
        if (force || menu != g_drawn_menu) draw_key(g_menux, g_menuy, g_menuw, "MENU", menu);
        g_drawn_keys = k; g_drawn_menu = menu;
        return;
    }
    const uint8_t ch = force ? 0xFF : (uint8_t)(k ^ g_drawn_keys);
    if (ch & (BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT)) draw_dpad(k);
    if (ch & BTN_A) draw_ab(g_ax, g_ay, "A", k & BTN_A);
    if (ch & BTN_B) draw_ab(g_bx, g_by, "B", k & BTN_B);
    if (force || menu != g_drawn_menu) draw_key(g_menux, g_menuy, g_menuw, "MENU", menu);
    g_drawn_keys = k;
    g_drawn_menu = menu;
}
static void draw_all() {
    nv_gfx_clear(C_BG);
    const int b = g_scale == 7 ? 6 : 10;
    nv_gfx_rect(g_gx - b, g_gy - b, g_gw + 2 * b, g_gh + 2 * b, C_BEZEL);
    draw_controls(0, false, true);
    g_full = true;
}

// ---- video --------------------------------------------------------------------------------------
static uint8_t g_last_eff[AW * AH / 8];         // blend mode: the previous painted frame
static uint8_t g_shown_prev[AW * AH / 8];       // blend mode: its half of what the canvas shows
static void blit_changes(bool new_frame = false) {
    uint8_t eff[AW * AH / 8], prv[AW * AH / 8];
    for (int i = 0; i < AW * AH / 8; i++) eff[i] = g_all_on ? 0xFF : (uint8_t)(g_invert ? ~g_cur[i] : g_cur[i]);
    // Blend (games that flicker two frames for grey): a pixel lit in only one of the last two frames
    // is drawn half-bright, as the Arduboy's OLED persistence makes it look.
    if (nvab_blend) {
        memcpy(prv, new_frame ? g_last_eff : g_shown_prev, sizeof prv);
        if (new_frame) memcpy(g_last_eff, eff, sizeof eff);
    } else {
        memcpy(prv, eff, sizeof prv);
    }
    int x0 = AW, x1 = -1, p0 = 8, p1 = -1;
    for (int p = 0; p < 8; p++)
        for (int x = 0; x < AW; x++) {
            const int i = p * AW + x;
            if (g_full || eff[i] != g_shown[i] || prv[i] != g_shown_prev[i]) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (p < p0) p0 = p;
                if (p > p1) p1 = p;
            }
        }
    if (x1 < 0) return;
    // tighten to the rows that really changed inside the page band
    int y0 = p0 * 8, y1 = p1 * 8 + 7;
    if (!g_full) {
        while (y0 < y1) {
            const int p = y0 >> 3, m = 1 << (y0 & 7);
            bool ch = false;
            for (int x = x0; x <= x1 && !ch; x++) {
                const int i = p * AW + x;
                ch = (((eff[i] ^ g_shown[i]) | (prv[i] ^ g_shown_prev[i])) & m) != 0;
            }
            if (ch) break;
            y0++;
        }
        while (y1 > y0) {
            const int p = y1 >> 3, m = 1 << (y1 & 7);
            bool ch = false;
            for (int x = x0; x <= x1 && !ch; x++) {
                const int i = p * AW + x;
                ch = (((eff[i] ^ g_shown[i]) | (prv[i] ^ g_shown_prev[i])) & m) != 0;
            }
            if (ch) break;
            y1--;
        }
    }
    const int S = g_scale, w = (x1 - x0 + 1) * S;
    uint16_t *o = g_out;
    for (int y = y0; y <= y1; y++) {
        const int p = y >> 3, m = 1 << (y & 7);
        uint16_t *row = o, *edge = o + (S - 1) * w;
        for (int x = x0; x <= x1; x++) {
            const int lvl = ((eff[p * AW + x] & m) != 0) + ((prv[p * AW + x] & m) != 0);
            const uint16_t c = lvl == 2 ? g_on : lvl ? g_mid : g_off;
            const uint16_t ce = !g_grid || !lvl ? c : lvl == 2 ? g_on_edge : g_mid_edge;
            for (int k = 0; k < S - 1; k++) { row[k] = c; edge[k] = ce; }
            row[S - 1] = ce;
            edge[S - 1] = ce;
            row += S; edge += S;
        }
        for (int k = 1; k < S - 1; k++) memcpy(o + k * w, o, (size_t)w * 2);
        o += w * S;
    }
    const int rows = (y1 - y0 + 1) * S;
    nv_gfx_blit_raw(g_out, rows * w * 2, g_gx + x0 * S, g_gy + y0 * S, w, rows);
    memcpy(g_shown, eff, sizeof g_shown);
    memcpy(g_shown_prev, prv, sizeof g_shown_prev);
    g_full = false;
}

// ---- input --------------------------------------------------------------------------------------
static bool in_rect(int x, int y, int cx, int cy, int w, int h) {
    return x >= cx - w / 2 && x < cx + w / 2 && y >= cy - h / 2 && y < cy + h / 2;
}
static int dist2(int x, int y, int cx, int cy) { return (x - cx) * (x - cx) + (y - cy) * (y - cy); }
static bool g_tap;
static int g_tap_x, g_tap_y;

static void read_input() {
    g_last_input = nv_millis();
    uint8_t k = 0;
    bool menu = false;
    g_tap = false;
    const int n = nv_touch_count();
    for (int i = 0; i < n && i < 5; i++) {
        int x, y;
        if (!nv_touch_at(i, &x, &y)) continue;
        if (i == 0 && g_touch_prev == 0) { g_tap = true; g_tap_x = x; g_tap_y = y; }
        const int side = g_ctrl_visible ? g_gx : 200;
        const int R = g_dps / 2 + 50;
        if (in_rect(x, y, g_menux, g_menuy, g_menuw + 30, KEY_H + 36)) {
            menu = true;
        } else if (x < side + 24 && dist2(x, y, g_dpx, g_dpy) <= R * R) {
            const int dx = x - g_dpx, dy = y - g_dpy, dead = g_dps / 10;
            const int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
            if (ax > dead && ax * 2 >= ay) k |= dx < 0 ? BTN_LEFT : BTN_RIGHT;
            if (ay > dead && ay * 2 >= ax) k |= dy < 0 ? BTN_UP : BTN_DOWN;
        } else if (x > CW - side - 24 && y > g_by - g_abr - 90 && y < g_ay + g_abr + 110) {
            k |= dist2(x, y, g_ax, g_ay) <= dist2(x, y, g_bx, g_by) ? BTN_A : BTN_B;
        }
    }
    g_touch_prev = n;
    const uint32_t p = (uint32_t)nv_gfx_pad();
    if (p & NV_PAD_UP) k |= BTN_UP;
    if (p & NV_PAD_DOWN) k |= BTN_DOWN;
    if (p & NV_PAD_LEFT) k |= BTN_LEFT;
    if (p & NV_PAD_RIGHT) k |= BTN_RIGHT;
    if (p & (NV_PAD_A | NV_PAD_X | NV_PAD_START)) k |= BTN_A;
    if (p & (NV_PAD_B | NV_PAD_Y)) k |= BTN_B;
    uint32_t mb = p & (NV_PAD_L | NV_PAD_R | NV_PAD_SELECT);
    const int pads = nv_pad_count();
    for (int i = 0; i < pads && i < 4; i++) {
        nv_pad_state_t st;
        if (nv_pad_state(i, &st, sizeof st) <= 0) continue;
        if (st.lx < -16000) k |= BTN_LEFT;
        if (st.lx > 16000) k |= BTN_RIGHT;
        if (st.ly < -16000) k |= BTN_UP;
        if (st.ly > 16000) k |= BTN_DOWN;
        if (st.buttons & NV_PADB_GUIDE) mb |= 1u << 31;
    }
    if (mb & ~g_pad_prev) g_menu_req = true;
    g_pad_prev = mb;
    if (menu && !g_menu_key) g_menu_req = true;
    g_menu_key = menu;
    g_keys = k;
}

// ---- pause menu ---------------------------------------------------------------------------------
enum { M_RESUME, M_ZOOM, M_PAL, M_SOUND, M_EXIT, M_N };
static int menu_item_y(int i, int *h) {
    const int ih = g_gh >= 384 ? 64 : 54, top = g_gy + (g_gh - (M_N * ih + 56)) / 2 + 56;
    *h = ih - 10;
    return top + i * ih;
}
static void draw_menu(int sel) {
    nv_gfx_rect(g_gx, g_gy, g_gw, g_gh, C_PANEL);
    int h;
    const int y0 = menu_item_y(0, &h);
    text_c(g_gx + g_gw / 2, y0 - 44, nvab_title, C_DIM, 3);
    char b[48];
    for (int i = 0; i < M_N; i++) {
        const int y = menu_item_y(i, &h);
        const char *s = "";
        switch (i) {
        case M_RESUME: s = tr("RESUME", "CONTINUA"); break;
        case M_ZOOM: snprintf(b, sizeof b, "ZOOM %dX", g_scale); s = b; break;
        case M_PAL: snprintf(b, sizeof b, "%s %s", tr("COLOURS", "COLORI"), g_it ? k_pal[g_pal].it : k_pal[g_pal].en);
                    s = b; break;
        case M_SOUND: snprintf(b, sizeof b, "%s %s", tr("SOUND", "AUDIO"), g_muted ? tr("OFF", "NO") : tr("ON", "SI"));
                      s = b; break;
        case M_EXIT: s = tr("EXIT", "ESCI"); break;
        }
        nv_gfx_rect(g_gx + 60, y, g_gw - 120, h, i == sel ? C_HI : C_KEY);
        text_c(g_gx + g_gw / 2, y + h / 2 - 10, s, C_TEXT, 3);
    }
}
static int menu_hit(int x, int y) {
    for (int i = 0; i < M_N; i++) {
        int h;
        const int yy = menu_item_y(i, &h);
        if (x >= g_gx + 60 && x < g_gx + g_gw - 60 && y >= yy && y < yy + h) return i;
    }
    return -1;
}
static void pause_menu() {
    const int32_t t_in = nv_millis();
    ee_flush();
    int sel = 0;
    draw_controls(0, true, false);
    draw_menu(sel);
    uint8_t prev = 0xFF;
    bool prev_menu = true;
    for (;;) {
        if (!nv_gfx_present()) quit();
        read_input();
        const bool back = nv_gfx_back() > 0;
        if (back) quit();                               // back twice: leave the game
        const uint8_t e = (uint8_t)(g_keys & ~prev);
        prev = g_keys;
        int act = -1;
        if (e & BTN_UP) { sel = (sel + M_N - 1) % M_N; draw_menu(sel); }
        if (e & BTN_DOWN) { sel = (sel + 1) % M_N; draw_menu(sel); }
        if (e & BTN_A) act = sel;
        if (e & BTN_B) act = M_RESUME;
        if (g_menu_req && !prev_menu) act = M_RESUME;
        prev_menu = g_menu_req;
        g_menu_req = false;
        if (g_tap) { const int hit = menu_hit(g_tap_x, g_tap_y); if (hit >= 0) act = hit; }
        if (act == M_RESUME) break;
        if (act == M_EXIT) quit();
        if (act == M_ZOOM) {
            g_scale = g_scale == 7 ? 5 : g_scale + 1;
            cfg_save(); layout(); draw_all(); draw_controls(0, true, false); draw_menu(sel);
        }
        if (act == M_PAL) { g_pal = (g_pal + 1) % NPAL; set_colors(); cfg_save(); draw_menu(sel); }
        if (act == M_SOUND) {
            g_muted = !g_muted;
            if (g_muted && g_audio) { nv_audio_close(); g_audio = false; }
            g_audio_retry = 0;
            cfg_save(); draw_menu(sel);
        }
    }
    draw_all();
    blit_changes();
    g_menu_req = false;
    // wait for the resume finger/button to lift so it doesn't reach the game
    for (int i = 0; i < 30; i++) {
        if (!nv_gfx_present()) quit();
        read_input();
        if (!g_keys) break;
    }
    g_menu_req = false;
    const int32_t now = nv_millis();
    g_paused_ms += now - t_in;
    g_gen_t = now;
    g_last_present = now;
}

// ---- service: presents, input, audio, saves -----------------------------------------------------
static void service(bool painted) {
    if (g_in_service) return;
    g_in_service = true;
    audio_pump();
    read_input();
    if (nv_gfx_back() > 0) g_menu_req = true;
    if (g_menu_req) { g_menu_req = false; pause_menu(); }
    if (g_keys != g_drawn_keys || g_menu_key != g_drawn_menu) draw_controls(g_keys, g_menu_key, false);
    if (g_ee_dirty && nv_millis() - g_ee_touch > 1500) ee_flush();
    if (!nv_gfx_present()) quit();
    const int32_t now = nv_millis();
    g_last_present = g_last_service = now;
    if (painted) g_stat_frames++;
    if (now - g_stat_t >= 10000) {
        char b[128];
        snprintf(b, sizeof b, "%d fps, audio %s backlog %d ms, dropped %d",
                 (int)(g_stat_frames * 1000 / (now - g_stat_t)), g_audio ? "on" : "off",
                 (int)(g_aud_bl / AUD_MS(1)), (int)g_aud_dropped);
        nv_log(NV_LOG_INFO, b);
        g_stat_t = now; g_stat_frames = 0; g_aud_dropped = 0;
    }
    g_in_service = false;
    (void)painted;
}
static void service_if_due() {
    if (!g_in_service && nv_millis() - g_last_present >= 33) service(false);
}

// ---- public -------------------------------------------------------------------------------------
void boot() {
    if (g_booted) return;
    g_booted = true;
    char lang[8] = {0};
    nv_lang(lang, sizeof lang);
    g_it = lang[0] == 'i' && lang[1] == 't';
    if (!cfg_load()) {
        const uint32_t p = (uint32_t)nv_gfx_pad();
        g_scale = (p & (NV_PAD_GAMEPAD | NV_PAD_KEYBOARD)) ? 7 : 6;
    }
    ee_load();
    random_seed((uint32_t)nv_rand() ^ ((uint32_t)nv_millis() << 16));
    g_out = (uint16_t *)malloc((size_t)AW * 7 * AH * 7 * 2);
    if (!g_out) { nv_log(NV_LOG_ERROR, "out of memory"); exit(1); }
    nv_gfx_persist(1);
    set_colors();
    layout();
    draw_all();
    blit_changes();
    g_t0 = nv_millis();
    g_gen_t = g_last_present = g_last_service = g_stat_t = g_t0;
    g_audio_retry = g_t0 - 5000;
}
void paint(const uint8_t *buf) {
    boot();
    memcpy(g_cur, buf, sizeof g_cur);
#ifdef NV_SIM
    nv_sim_frame(g_cur);
#endif
    blit_changes(true);
    service(true);
}
void lcd_command(uint8_t c) {
    switch (c) {
    case 0xA7: set_invert(true); break;
    case 0xA6: set_invert(false); break;
    case 0xA5: set_all_on(true); break;
    case 0xA4: set_all_on(false); break;
    default: break;
    }
}
void set_invert(bool on) { if (g_invert != on) { g_invert = on; blit_changes(); } }
void set_all_on(bool on) { if (g_all_on != on) { g_all_on = on; blit_changes(); } }
void blank() {
    boot();
    memset(g_cur, 0, sizeof g_cur);
    blit_changes();
}
uint8_t buttons() {
    boot();
    const int32_t now = nv_millis();
    if (now - g_last_present >= 33) service(false);
    else if (now - g_last_input >= 4) read_input();
    return g_keys;
}
uint32_t millis() {
    boot();
    service_if_due();
    return (uint32_t)(nv_millis() - g_t0 - g_paused_ms);
}
uint32_t micros() { return millis() * 1000u; }
void delay(uint32_t ms) {
    boot();
    const uint32_t end = millis() + ms;
    for (;;) {
        const uint32_t now = millis();
        if ((int32_t)(end - now) <= 0) break;
        const uint32_t left = end - now;
        nv_sleep_ms(left > 8 ? 8 : (int32_t)left);
        service_if_due();
    }
}
void poll() { service_if_due(); }
void idle() {
    boot();
    nv_sleep_ms(1);
    service_if_due();
}
void quit() {
    ee_flush();
    if (g_audio) nv_audio_close();
    g_audio = false;
    exit(0);
}
static uint32_t g_rng = 0x2545F491u;
uint32_t random32() {
    uint32_t x = g_rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return g_rng = x;
}
void random_seed(uint32_t s) { g_rng = s ? s : 0x2545F491u; }

}  // namespace nvab

// ---- Arduino core functions -----------------------------------------------------------------------
// avr-libc heap bounds, read by "free memory" helpers some sketches carry
extern "C" {
char __heap_start;
char *__brkval;
}
volatile uint8_t nvab_reg8[80];
volatile uint16_t nvab_reg16[16];
EEPROMClass EEPROM;
NvabSerial Serial;
NvabSerial Serial1;
SPIClass SPI;

extern "C" {
void nvab_idle(void) { nvab::idle(); }
unsigned long millis(void) { return nvab::millis(); }
unsigned long micros(void) { return nvab::micros(); }
void delay(unsigned long ms) { nvab::delay((uint32_t)ms); }
void delayMicroseconds(unsigned int us) { if (us >= 1000) nvab::delay(us / 1000); }
void yield(void) { nvab::idle(); }
void pinMode(uint8_t, uint8_t) {}
void digitalWrite(uint8_t, uint8_t) {}
int digitalRead(uint8_t) { return HIGH; }
int analogRead(uint8_t) { return (int)(nvab::random32() & 1023); }
void analogWrite(uint8_t, int) {}
void analogReference(uint8_t) {}
unsigned long pulseIn(uint8_t, uint8_t, unsigned long) { return 0; }
void shiftOut(uint8_t, uint8_t, uint8_t, uint8_t) {}
uint8_t shiftIn(uint8_t, uint8_t, uint8_t) { return 0; }
void attachInterrupt(uint8_t, void (*)(void), int) {}
void detachInterrupt(uint8_t) {}

static unsigned long s_tone_left;       // ms left of tone(pin, f, d), 0 = until noTone
static void tone_tick(void) {
    if (s_tone_left && --s_tone_left == 0) nvab::voice_off(nvab::V_TONE);
}
void tone(uint8_t, unsigned int frequency, unsigned long duration) {
    nvab::add_ticker(tone_tick);
    s_tone_left = duration;
    nvab::voice_on(nvab::V_TONE, (float)frequency, nvab::VOL_NORMAL);
}
void noTone(uint8_t) { s_tone_left = 0; nvab::voice_off(nvab::V_TONE); }

static char *num_to(unsigned long u, bool neg, char *str, int base) {
    char b[40];
    char *p = b + sizeof b - 1;
    *p = 0;
    if (base < 2 || base > 36) base = 10;
    do { const int d = (int)(u % (unsigned)base); *--p = (char)(d < 10 ? '0' + d : 'a' + d - 10); u /= (unsigned)base; } while (u);
    if (neg) *--p = '-';
    strcpy(str, p);
    return str;
}
char *itoa(int v, char *s, int base) { return num_to(v < 0 && base == 10 ? (unsigned long)(-(long)v) : (unsigned long)(unsigned)v, v < 0 && base == 10, s, base); }
char *utoa(unsigned v, char *s, int base) { return num_to(v, false, s, base); }
char *ltoa(long v, char *s, int base) { return num_to(v < 0 && base == 10 ? (unsigned long)(-v) : (unsigned long)v, v < 0 && base == 10, s, base); }
char *ultoa(unsigned long v, char *s, int base) { return num_to(v, false, s, base); }
char *dtostrf(double val, signed char width, unsigned char prec, char *s) {
    char fmt[16];
    snprintf(fmt, sizeof fmt, "%%%d.%df", (int)width, (int)prec);
    sprintf(s, fmt, val);
    return s;
}

// EEPROM (avr-libc style)
uint8_t nvab_ee_read(uint16_t a) { return nvab::ee_read(a); }
void nvab_ee_write(uint16_t a, uint8_t v) { nvab::ee_write(a, v); }
static uint16_t ee_addr(const void *p) { return (uint16_t)((uintptr_t)p & 1023); }
uint8_t eeprom_read_byte(const uint8_t *p) { return nvab::ee_read(ee_addr(p)); }
void eeprom_read_block(void *dst, const void *src, size_t n) {
    for (size_t i = 0; i < n; i++) ((uint8_t *)dst)[i] = nvab::ee_read((uint16_t)(ee_addr(src) + i));
}
uint16_t eeprom_read_word(const uint16_t *p) { uint16_t v; eeprom_read_block(&v, p, 2); return v; }
uint32_t eeprom_read_dword(const uint32_t *p) { uint32_t v; eeprom_read_block(&v, p, 4); return v; }
float eeprom_read_float(const float *p) { float v; eeprom_read_block(&v, p, 4); return v; }
void eeprom_write_block(const void *src, void *dst, size_t n) {
    for (size_t i = 0; i < n; i++) nvab::ee_write((uint16_t)(ee_addr(dst) + i), ((const uint8_t *)src)[i]);
}
void eeprom_write_byte(uint8_t *p, uint8_t v) { nvab::ee_write(ee_addr(p), v); }
void eeprom_write_word(uint16_t *p, uint16_t v) { eeprom_write_block(&v, p, 2); }
void eeprom_write_dword(uint32_t *p, uint32_t v) { eeprom_write_block(&v, p, 4); }
void eeprom_write_float(float *p, float v) { eeprom_write_block(&v, p, 4); }
void eeprom_update_byte(uint8_t *p, uint8_t v) { eeprom_write_byte(p, v); }
void eeprom_update_word(uint16_t *p, uint16_t v) { eeprom_write_word(p, v); }
void eeprom_update_dword(uint32_t *p, uint32_t v) { eeprom_write_dword(p, v); }
void eeprom_update_float(float *p, float v) { eeprom_write_float(p, v); }
void eeprom_update_block(const void *src, void *dst, size_t n) { eeprom_write_block(src, dst, n); }
}

long random(long howbig) { return howbig <= 0 ? 0 : (long)(nvab::random32() % (uint32_t)howbig); }
long random(long howsmall, long howbig) { return howsmall >= howbig ? howsmall : howsmall + random(howbig - howsmall); }
void randomSeed(unsigned long seed) { if (seed) nvab::random_seed((uint32_t)seed); }
long map(long x, long in_min, long in_max, long out_min, long out_max) {
    return in_max == in_min ? out_min : (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

// ---- Print --------------------------------------------------------------------------------------
size_t Print::write(const uint8_t *buf, size_t n) {
    size_t k = 0;
    while (n--) { if (!write(*buf++)) break; k++; }
    return k;
}
size_t Print::printNumber(unsigned long n, int base) {
    char b[40];
    char *p = b + sizeof b - 1;
    *p = 0;
    if (base < 2) base = 10;
    do { const int d = (int)(n % (unsigned)base); *--p = (char)(d < 10 ? '0' + d : 'A' + d - 10); n /= (unsigned)base; } while (n);
    return write(p);
}
size_t Print::printSigned(long n, int base) {
    if (base == 10 && n < 0) { size_t k = print('-'); return k + printNumber((unsigned long)(-n), 10); }
    return printNumber((unsigned long)n, base);
}
size_t Print::print(long long n, int base) {
    if (n >= -2147483647LL && n <= 2147483647LL) return printSigned((long)n, base);
    char b[32];
    snprintf(b, sizeof b, "%lld", n);
    return write(b);
}
size_t Print::print(unsigned long long n, int base) {
    if (n <= 0xFFFFFFFFull) return printNumber((unsigned long)n, base);
    char b[32];
    snprintf(b, sizeof b, "%llu", n);
    return write(b);
}
size_t Print::printFloat(double n, int digits) {
    if (n != n) return write("nan");
    if (n > 4294967040.0 || n < -4294967040.0) return write("ovf");
    size_t k = 0;
    if (n < 0.0) { k += print('-'); n = -n; }
    double r = 0.5;
    for (int i = 0; i < digits; i++) r /= 10.0;
    n += r;
    unsigned long ip = (unsigned long)n;
    double rem = n - (double)ip;
    k += printNumber(ip, 10);
    if (digits > 0) k += print('.');
    while (digits-- > 0) {
        rem *= 10.0;
        const unsigned d = (unsigned)rem;
        k += print((char)('0' + d));
        rem -= d;
    }
    return k;
}
size_t Print::print(const Printable &p) { return p.printTo(*this); }
String::String(double v, unsigned char decimals) {
    char b[48];
    snprintf(b, sizeof b, "%.*f", (int)decimals, v);
    set(b, strlen(b));
}

// Serial: collect a line, log it (at most ~20 lines/s so a chatty debug sketch can't flood the log)
size_t NvabSerial::write(uint8_t c) {
    static char line[120];
    static int n;
    static int32_t t, count;
    if (c == '\r') return 1;
    if (c != '\n' && n < (int)sizeof line - 1) { line[n++] = (char)c; return 1; }
    line[n] = 0;
    const int32_t now = nv_millis();
    if (now - t > 1000) { t = now; count = 0; }
    if (n && count++ < 20) nv_log(NV_LOG_INFO, line);
    n = 0;
    return 1;
}

// ---- C++ runtime bits (no libc++ linked) ------------------------------------------------------------
void *operator new(size_t n) { void *p = malloc(n ? n : 1); if (!p) abort(); return p; }
void *operator new[](size_t n) { return operator new(n); }
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }
extern "C" void __cxa_pure_virtual() { abort(); }

// ---- entry ------------------------------------------------------------------------------------------
extern "C" NV_EXPORT("run") void run(void) {
    nvab::boot();
    setup();
    for (;;) {
        loop();
        nvab::poll();
    }
}
