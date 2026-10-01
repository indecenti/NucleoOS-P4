// nv_doom.c — NucleoOS front-end for doomgeneric: video, input, launcher and IWAD download.
//
// Video: Doom renders 320x200 8-bit into I_VideoBuffer; DG_DrawFrame turns it into RGB565 through
// a 256-entry palette LUT and stretches it to 320x240 (every 5th line twice), i.e. the 4:3 picture
// Doom was drawn for. The manifest's canvas_scale "fit" has the OS PPA scale that to 800x600 on the
// panel, so the app only ever moves 150 KB per frame.
// Input: USB/BLE keyboard as raw keys (ABI 13 nv_kbd_state: held usages + modifiers, diffed into
// Doom key events), USB mouse as relative motion (ABI 13 nv_mouse_read), every controller as
// analog sticks + buttons (ABI 11 nv_pad_state), touch for menus. The game loop is
// doomgeneric_Tick() per shown frame; Doom itself paces to 35 tics/s.
// Launcher: the engine's own tile lists the IWADs in /home/doom; a store game package running
// this module ("engine": "doom") downloads what its descriptor lists (see game_mode) and starts.
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#include "doomgeneric.h"
#include "doomkeys.h"
#include "doomstat.h"
#include "d_event.h"
#include "d_player.h"
#include "g_game.h"
#include "i_video.h"
#include "m_controls.h"
#include "sha1.h"

#include "nucleo_sdk.h"
#include "nv_doom.h"

enum { SW = 320, SH = 200, CW = 320, CH = 240 };
#ifndef PERF_LOG_MS
#define PERF_LOG_MS 60000   // one perf line a minute in the system log
#endif
#ifdef PERF_PROFILE
static int64_t us_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }
int64_t g_prof_draw, g_prof_mix, g_prof_present, g_prof_blit;
#endif

#ifdef NV_SIM   // PC harness: it chdirs into a fake filesystem and simulates the clock
#define FS "./"
#else
#define FS "/"
#endif

extern boolean chat_on;               // hu_stuff.c
extern boolean messageNeedsInput;     // m_menu.c: a Y/N prompt is up
extern int messageToPrint;

const char *nv_doom_datadir = FS;
static const char *s_homedir = NULL;  // "/" when the "home" permission maps /sdcard/home
static uint16_t s_out[CW * CH];
static uint16_t s_lut[256];
static int s_it;                      // Italian UI

static const char *tr(const char *en, const char *it) { return s_it ? it : en; }

// =================================================================================================
// Video
// =================================================================================================
static void build_lut(void) {
    for (int i = 0; i < 256; i++)
        s_lut[i] = (uint16_t)NV_RGB(colors[i].r, colors[i].g, colors[i].b);
}

void DG_Init(void) {}

// Keep the OS wedge watchdog (8 s without a frame = killed) happy through long loads: the w_wad.c
// patch calls this on every lump read; it presents only when the game loop has been silent for
// half a second, so it never adds frames during normal play.
static int32_t s_last_present;
unsigned int Z_ZoneSize(void);   // z_zone.c

void DG_Pulse(void) {
    const int32_t now = nv_millis();
    if (now - s_last_present < 500) return;
    s_last_present = now;
    nv_snd_pump();
    nv_gfx_present();
}

void DG_DrawFrame(void) {
#ifdef PERF_PROFILE
    const int64_t t0 = us_now();
#endif
    if (palette_changed) {
        build_lut();
        palette_changed = false;
    }
    // 200 -> 240 lines: source line y lands on output y + y/5; every 5th line is doubled
    const uint8_t *src = I_VideoBuffer;
    uint16_t *dst = s_out;
    for (int y = 0; y < SH; y++, src += SW) {
        uint32_t *d32 = (uint32_t *)dst;
        for (int x = 0; x < SW; x += 2) d32[x >> 1] = s_lut[src[x]] | (uint32_t)s_lut[src[x + 1]] << 16;
        dst += CW;
        if (y % 5 == 4) {
            memcpy(dst, dst - CW, CW * 2);
            dst += CW;
        }
    }
#ifdef PERF_PROFILE
    const int64_t tb = us_now();
    g_prof_draw += tb - t0;
#endif
    nv_gfx_blit_raw(s_out, (int32_t)sizeof s_out, 0, 0, CW, CH);
#ifdef PERF_PROFILE
    g_prof_blit += us_now() - tb;
    const int64_t t1 = us_now();
    nv_snd_pump();
    g_prof_mix += us_now() - t1;
#else
    nv_snd_pump();
#endif
}

static int32_t s_slept_ms;   // perf log: time Doom spent waiting for the next tic
void DG_SleepMs(uint32_t ms) {
    nv_snd_pump();
    s_slept_ms += (int32_t)ms;
#ifdef NV_SIM
    nv_sleep_ms((int32_t)ms);
#else
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
#endif
}

uint32_t DG_GetTicksMs(void) { return (uint32_t)nv_millis(); }

void DG_SetWindowTitle(const char *title) { (void)title; }

// =================================================================================================
// Input
// =================================================================================================
static void post(evtype_t type, int d1, int d2, int d3, int d4) {
    event_t ev;
    ev.type = type;
    ev.data1 = d1;
    ev.data2 = d2;
    ev.data3 = d3;
    ev.data4 = d4;
    D_PostEvent(&ev);
}
static void key(int down, int k, int ch) {
    if (k) post(down ? ev_keydown : ev_keyup, k, down ? ch : 0, 0, 0);
}

// ---- keyboard (HID usages, ABI 13) ---------------------------------------------------------------
// In play, WASD/E drive the game controls (the arrows keep working too); in menus, chat and save
// names every key is itself. data2 always carries the typed character, so cheats still work.
static int typing(void) { return menuactive || chat_on || messageToPrint; }

static int hid_char(int u, int shift) {
    static const char row_num[] = "1234567890";
    static const char row_num_sh[] = "!@#$%^&*()";
    if (u >= 0x04 && u <= 0x1d) return (shift ? 'A' : 'a') + (u - 0x04);
    if (u >= 0x1e && u <= 0x27) return (shift ? row_num_sh : row_num)[u - 0x1e];
    switch (u) {
    case 0x2c: return ' ';
    case 0x2d: return shift ? '_' : '-';
    case 0x2e: return shift ? '+' : '=';
    case 0x2f: return shift ? '{' : '[';
    case 0x30: return shift ? '}' : ']';
    case 0x31: return shift ? '|' : '\\';
    case 0x33: return shift ? ':' : ';';
    case 0x34: return shift ? '"' : '\'';
    case 0x35: return shift ? '~' : '`';
    case 0x36: return shift ? '<' : ',';
    case 0x37: return shift ? '>' : '.';
    case 0x38: return shift ? '?' : '/';
    case 0x54: return '/';
    case 0x55: return '*';
    case 0x56: return '-';
    case 0x57: return '+';
    }
    if (u >= 0x59 && u <= 0x61) return '1' + (u - 0x59);
    if (u == 0x62) return '0';
    return 0;
}

static int hid_key(int u) {   // HID usage -> Doom key code (0 = none)
    if (u >= 0x04 && u <= 0x1d) {
        const int c = 'a' + (u - 0x04);
        if (!typing()) {
            switch (c) {
            case 'w': return KEY_UPARROW;
            case 's': return KEY_DOWNARROW;
            case 'a': return KEY_STRAFE_L;
            case 'd': return KEY_STRAFE_R;
            case 'e': return KEY_USE;
            }
        }
        return c;
    }
    if (u >= 0x3a && u <= 0x43) return KEY_F1 + (u - 0x3a);   // F1..F10
    switch (u) {
    case 0x28: case 0x58: return KEY_ENTER;
    case 0x29: return KEY_ESCAPE;
    case 0x2a: return KEY_BACKSPACE;
    case 0x2b: return KEY_TAB;
    case 0x2c: return typing() ? ' ' : KEY_USE;
    case 0x44: return KEY_F11;
    case 0x45: return KEY_F12;
    case 0x48: return KEY_PAUSE;
    case 0x49: return KEY_INS;
    case 0x4a: return KEY_HOME;
    case 0x4b: return KEY_PGUP;
    case 0x4c: return KEY_DEL;
    case 0x4d: return KEY_END;
    case 0x4e: return KEY_PGDN;
    case 0x4f: return KEY_RIGHTARROW;
    case 0x50: return KEY_LEFTARROW;
    case 0x51: return KEY_DOWNARROW;
    case 0x52: return KEY_UPARROW;
    case 0x2d: case 0x56: return KEY_MINUS;
    case 0x2e: case 0x57: return KEY_EQUALS;
    }
    return hid_char(u, 0);
}

static uint8_t s_kbd_prev[6], s_kbd_nprev, s_mod_prev;
static int s_kbd_code[256];           // Doom key sent at press time, so release matches it

static void poll_keyboard(void) {
    uint8_t b[7];
    const int n = nv_kbd_state(b, sizeof b);
    if (n < 0) {                      // no keyboard: release whatever was held
        if (s_kbd_nprev || s_mod_prev) memset(b, 0, sizeof b);
        else return;
    }
    const int cnt = n < 0 ? 0 : n > 6 ? 6 : n;
    const uint8_t mod = b[0];
    const uint8_t *u = b + 1;
    const int shift = (mod & 0x22) != 0;
    // modifiers: Ctrl = fire, Shift = run, Alt = strafe
    static const struct { uint8_t mask; int key; } mods[] = {
        {0x11, KEY_FIRE}, {0x22, KEY_RSHIFT}, {0x44, KEY_RALT}};
    for (unsigned i = 0; i < sizeof mods / sizeof mods[0]; i++) {
        const int was = (s_mod_prev & mods[i].mask) != 0, is = (mod & mods[i].mask) != 0;
        if (was != is) key(is, mods[i].key, 0);
    }
    s_mod_prev = mod;
    // releases
    for (int i = 0; i < s_kbd_nprev; i++) {
        int still = 0;
        for (int j = 0; j < cnt; j++) still |= u[j] == s_kbd_prev[i];
        if (!still) key(0, s_kbd_code[s_kbd_prev[i]], 0);
    }
    // presses
    for (int j = 0; j < cnt; j++) {
        int was = 0;
        for (int i = 0; i < s_kbd_nprev; i++) was |= u[j] == s_kbd_prev[i];
        if (was || u[j] < 4) continue;              // 1..3 = rollover / error codes
        const int k = hid_key(u[j]);
        s_kbd_code[u[j]] = k;
        key(1, k, hid_char(u[j], shift) ? hid_char(u[j], shift) : k < 128 ? k : 0);
    }
    memcpy(s_kbd_prev, u, (size_t)cnt);
    s_kbd_nprev = (uint8_t)cnt;
}

// ---- mouse (relative, ABI 13) --------------------------------------------------------------------
static int s_wheel_btn;               // weapon-cycle "button" to release on the next poll

static void poll_mouse(void) {
    nv_mouse_t m;
    if (nv_mouse_read(&m, sizeof m) <= 0) return;
    // Doom mouse buttons: 0 left = fire, 1 right = use, 2 middle = strafe; the wheel is sent as
    // button 3 (up = next weapon) / 4 (down = previous) for one poll.
    int btn = (int)(m.buttons & 7);
    if (s_wheel_btn) s_wheel_btn = 0;
    else if (m.wheel > 0) s_wheel_btn = 1 << 3;
    else if (m.wheel < 0) s_wheel_btn = 1 << 4;
    btn |= s_wheel_btn;
    post(ev_mouse, btn, m.dx * 2, 0, 0);   // vertical motion ignored: no mouse-walk
}

// ---- controllers (ABI 11) ------------------------------------------------------------------------
// In play: left stick walk/strafe, right stick turn (analog, via DG_AnalogTiccmd), RT/A... see the
// table below; in menus the pad navigates like the arrow keys, A = Enter (Yes at a prompt), B = back.
static int s_an_fwd, s_an_side, s_an_turn;   // -32767..32767 after the dead zone
static uint32_t s_pad_prev;
static int s_menu_axis_prev;

static int dead(int v) {
    const int D = 7000;
    if (v > -D && v < D) return 0;
    v = v > 0 ? (v - D) * 32767 / (32767 - D) : (v + D) * 32767 / (32767 - D);
    return v > 32767 ? 32767 : v < -32767 ? -32767 : v;
}

static void pad_key(uint32_t now, uint32_t prev, uint32_t bit, int k, int ch) {
    if ((now ^ prev) & bit) key((now & bit) != 0, k, ch);
}

static void release_pad_keys(uint32_t held, int menu) {
    static const int menu_keys[] = {KEY_UPARROW, KEY_DOWNARROW, KEY_LEFTARROW, KEY_RIGHTARROW,
                                    KEY_ENTER, 'y', KEY_BACKSPACE, KEY_ESCAPE};
    static const int game_keys[] = {KEY_FIRE, KEY_USE, KEY_RSHIFT, KEY_UPARROW, KEY_DOWNARROW,
                                    KEY_LEFTARROW, KEY_RIGHTARROW, KEY_ESCAPE, KEY_TAB};
    if (!held) return;
    const int *k = menu ? menu_keys : game_keys;
    const int n = menu ? (int)(sizeof menu_keys / sizeof *menu_keys) : (int)(sizeof game_keys / sizeof *game_keys);
    for (int i = 0; i < n; i++) key(0, k[i], 0);
    if (!menu) post(ev_joystick, 0, 0, 0, 0);
}

static void poll_pads(void) {
    uint32_t btn = 0;
    int lx = 0, ly = 0, rx = 0;
    const int n = nv_pad_count();
    for (int i = 0; i < n && i < 4; i++) {
        nv_pad_state_t st;
        if (nv_pad_state(i, &st, sizeof st) <= 0) continue;
        btn |= st.buttons;
        if (abs(st.lx) > abs(lx)) lx = st.lx;
        if (abs(st.ly) > abs(ly)) ly = st.ly;
        if (abs(st.rx) > abs(rx)) rx = st.rx;
    }
    if (n == 0 && !s_pad_prev) { s_an_fwd = s_an_side = s_an_turn = 0; return; }
    lx = dead(lx); ly = dead(ly); rx = dead(rx);
    static int s_was_typing;
    if (typing() != s_was_typing) {       // menu opened/closed: let go of everything held
        release_pad_keys(s_pad_prev, s_was_typing);
        s_pad_prev = 0;
        s_menu_axis_prev = 0;
        s_was_typing = typing();
    }
    const uint32_t prev = s_pad_prev;
    // left stick also as a d-pad for menus (edge-triggered)
    uint32_t stick = 0;
    if (ly < -20000) stick |= NV_PADB_UP;
    if (ly > 20000) stick |= NV_PADB_DOWN;
    if (lx < -20000) stick |= NV_PADB_LEFT;
    if (lx > 20000) stick |= NV_PADB_RIGHT;

    if (typing()) {
        s_an_fwd = s_an_side = s_an_turn = 0;
        const uint32_t nav = btn | stick;
        const uint32_t pnav = prev | (uint32_t)s_menu_axis_prev;
        pad_key(nav, pnav, NV_PADB_UP, KEY_UPARROW, 0);
        pad_key(nav, pnav, NV_PADB_DOWN, KEY_DOWNARROW, 0);
        pad_key(nav, pnav, NV_PADB_LEFT, KEY_LEFTARROW, 0);
        pad_key(nav, pnav, NV_PADB_RIGHT, KEY_RIGHTARROW, 0);
        if (messageNeedsInput) pad_key(btn, prev, NV_PADB_A, 'y', 'y');
        else pad_key(btn, prev, NV_PADB_A, KEY_ENTER, 0);
        pad_key(btn, prev, NV_PADB_B, KEY_BACKSPACE, 0);
        pad_key(btn, prev, NV_PADB_START, KEY_ESCAPE, 0);
        s_menu_axis_prev = (int)stick;
        s_pad_prev = btn;
        return;
    }
    s_menu_axis_prev = (int)stick;
    s_an_fwd = -ly;
    s_an_side = lx;
    s_an_turn = rx;
    // buttons -> keys (same bindings as the keyboard, so the menus/config stay in charge)
    pad_key(btn, prev, NV_PADB_RT | NV_PADB_X, KEY_FIRE, 0);
    pad_key(btn, prev, NV_PADB_A, KEY_USE, 0);
    pad_key(btn, prev, NV_PADB_LT | NV_PADB_LSTICK, KEY_RSHIFT, 0);
    pad_key(btn, prev, NV_PADB_UP, KEY_UPARROW, 0);
    pad_key(btn, prev, NV_PADB_DOWN, KEY_DOWNARROW, 0);
    pad_key(btn, prev, NV_PADB_LEFT, KEY_LEFTARROW, 0);
    pad_key(btn, prev, NV_PADB_RIGHT, KEY_RIGHTARROW, 0);
    pad_key(btn, prev, NV_PADB_START, KEY_ESCAPE, 0);
    pad_key(btn, prev, NV_PADB_BACK, KEY_TAB, 0);
    // weapon cycling: LB / Y = previous, RB / B = next (joystick buttons 4 / 5)
    int jb = 0;
    if (btn & (NV_PADB_LB | NV_PADB_Y)) jb |= 1 << 4;
    if (btn & (NV_PADB_RB | NV_PADB_B)) jb |= 1 << 5;
    post(ev_joystick, jb, 0, 0, 0);
    s_pad_prev = btn;
}

void DG_AnalogTiccmd(int *forward, int *side, short *angleturn) {
    // full tilt = running speed (forwardmove[1] = 0x32, sidemove[1] = 0x28); turn with a squared
    // curve so small deflections aim finely and full deflection is ~1.5x the fast key turn
    *forward += s_an_fwd * 0x32 / 32767;
    *side += s_an_side * 0x28 / 32767;
    const int t = (int)((int64_t)s_an_turn * abs(s_an_turn) / 32767);
    *angleturn = (short)(*angleturn - t * 1920 / 32767);
}

// ---- rumble on damage -----------------------------------------------------------------------------
static int s_last_damage;
static void rumble(void) {
    const player_t *p = &players[consoleplayer];
    if (gamestate != GS_LEVEL || !playeringame[consoleplayer]) { s_last_damage = 0; return; }
    if (p->damagecount > s_last_damage) {
        const int strength = p->damagecount > 40 ? 65535 : 20000 + p->damagecount * 1100;
        for (int i = 0; i < nv_pad_count() && i < 4; i++) nv_pad_rumble(i, strength, strength / 2, 160);
    }
    s_last_damage = p->damagecount;
}

// ---- touch: taps drive the menus; in play the screen halves are a minimal pad --------------------
static int s_touch_prev;
static void poll_touch(void) {
    int x, y;
    const int down = nv_touch(&x, &y);
    if (down && !s_touch_prev) {
        if (typing()) key(1, messageNeedsInput ? 'y' : KEY_ENTER, messageNeedsInput ? 'y' : 0);
        else if (gamestate != GS_LEVEL) key(1, KEY_ESCAPE, 0);
        else key(1, KEY_FIRE, 0);
    } else if (!down && s_touch_prev) {
        key(0, KEY_ENTER, 0);
        key(0, 'y', 0);
        key(0, KEY_ESCAPE, 0);
        key(0, KEY_FIRE, 0);
    }
    s_touch_prev = down;
}

int DG_GetKey(int *pressed, unsigned char *k) {
    // Called by I_GetEvent every tic until it returns 0: post everything directly instead.
    (void)pressed; (void)k;
    static uint32_t last;
    const uint32_t now = (uint32_t)nv_millis();
    if (now - last < 5) return 0;     // once per tic batch
    last = now;
    poll_keyboard();
    poll_mouse();
    poll_pads();
    poll_touch();
    if (nv_gfx_back() > 0) { key(1, KEY_ESCAPE, 0); key(0, KEY_ESCAPE, 0); }
    rumble();
    return 0;
}

// =================================================================================================
// Launcher + game data
// =================================================================================================
// Two ways in:
//   * as "doom" (the engine's own tile): pick an IWAD from /home/doom (the user's own doom.wad,
//     doom2.wad... and whatever the store games downloaded there);
//   * as a store game package (manifest "engine": "doom", NUCLEO_APP = its id): the game's
//     descriptor <id>.game on the store site lists the WADs it needs (shared in /home/doom, so
//     Freedoom is downloaded once for every game built on it) and the command line. Downloads are
//     1 MB ranges, resumable, SHA-1 checked; the descriptor is cached for offline starts.
#define STORE_DATA "https://indecenti.github.io/nucleoos-p4-store/data/doom/"

typedef struct { char name[40]; uint32_t size; char sha1[41]; } gfile_t;
typedef struct {
    char title[40];
    char iwad[40];
    char pwad[4][40];
    int npwad;
    char deh[2][40];
    int ndeh;
    gfile_t file[8];
    int nfile;
} game_t;

typedef struct { char path[160]; char title[40]; } entry_t;
static entry_t s_ent[24];
static int s_nent;
static char s_waddir[64];             // where WADs live: <home>/doom/ or <private>/doom/

static int file_size(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 ? (int)st.st_size : -1;
}
static int is_iwad(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    char h[4] = {0};
    const size_t n = fread(h, 1, 4, f);
    fclose(f);
    return n == 4 && !memcmp(h, "IWAD", 4);
}
static void upcase(char *d, const char *s, size_t cap) {   // + only glyphs the 5x7 font has
    size_t i = 0;
    for (; s[i] && i + 1 < cap; i++) {
        const char c = (char)toupper((unsigned char)s[i]);
        d[i] = (isalnum((unsigned char)c) || strchr(" -.:%/<>!+", c)) ? c : ' ';
    }
    d[i] = 0;
}
static int name_ok(const char *s) {   // descriptor file names: no paths, no surprises
    if (!*s || strlen(s) >= 40) return 0;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && *s != '.' && *s != '_' && *s != '-') return 0;
    return 1;
}

static void scan(void) {
    s_nent = 0;
    DIR *d = opendir(s_waddir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) && s_nent < (int)(sizeof s_ent / sizeof s_ent[0])) {
        const size_t l = strlen(de->d_name);
        if (l < 5 || strcasecmp(de->d_name + l - 4, ".wad")) continue;
        entry_t *e = &s_ent[s_nent];
        snprintf(e->path, sizeof e->path, "%s%s", s_waddir, de->d_name);
        if (!is_iwad(e->path)) continue;
        upcase(e->title, de->d_name, sizeof e->title);
        e->title[l - 4 < sizeof e->title ? l - 4 : sizeof e->title - 1] = 0;
        s_nent++;
    }
    closedir(d);
}

// ---- drawing (320x240 canvas, 5x7 font) ----------------------------------------------------------
#define C_BG    NV_RGB(20, 8, 6)
#define C_ROW   NV_RGB(52, 20, 14)
#define C_SEL   NV_RGB(170, 40, 20)
#define C_TXT   NV_RGB(240, 220, 200)
#define C_DIM   NV_RGB(160, 120, 100)
#define C_BAR   NV_RGB(230, 120, 30)
enum { ROW_Y = 58, ROW_H = 24 };

static void text_c(int y, const char *s, int col, int sc) {
    nv_gfx_text((CW - nv_gfx_text_width(s, sc)) / 2, y, s, col, sc);
}

static void draw_menu(int sel) {
    nv_gfx_clear(C_BG);
    text_c(10, "DOOM", C_BAR, 4);
    if (!s_nent) {
        text_c(70, tr("NO GAMES YET", "ANCORA NESSUN GIOCO"), C_TXT, 2);
        text_c(104, tr("GET FREEDOOM OR OTHER DOOM GAMES", "SCARICA FREEDOOM O ALTRI GIOCHI"), C_DIM, 1);
        text_c(118, tr("FROM THE STORE, OR COPY YOUR OWN", "DALLO STORE, O COPIA I TUOI"), C_DIM, 1);
        text_c(132, tr("WAD FILES TO /HOME/DOOM", "FILE WAD IN /HOME/DOOM"), C_DIM, 1);
        return;
    }
    text_c(42, tr("CHOOSE A GAME", "SCEGLI IL GIOCO"), C_DIM, 1);
    for (int i = 0; i < s_nent; i++) {
        const int y = ROW_Y + i * ROW_H;
        if (y + ROW_H > CH - 22) break;
        nv_gfx_rect(16, y, CW - 32, ROW_H - 4, i == sel ? C_SEL : C_ROW);
        nv_gfx_text(24, y + 3, s_ent[i].title, C_TXT, 2 - (strlen(s_ent[i].title) > 21));
    }
    text_c(CH - 16, tr("YOUR WADS: /HOME/DOOM", "I TUOI WAD: /HOME/DOOM"), C_DIM, 1);
}

static void draw_message(const char *title, const char *l1, const char *l2) {
    nv_gfx_clear(C_BG);
    text_c(70, title, C_TXT, 2);
    if (l1) text_c(104, l1, C_DIM, 1);
    if (l2) text_c(118, l2, C_DIM, 1);
    text_c(CH - 16, tr("BACK: EXIT", "INDIETRO: ESCI"), C_DIM, 1);
}

static void draw_progress(const char *title, const char *what, int done, int total) {
    nv_gfx_clear(C_BG);
    text_c(60, title, C_TXT, 2);
    text_c(90, what, C_DIM, 1);
    nv_gfx_rect(30, 120, CW - 60, 16, C_ROW);
    const int w = total > 0 ? (int)((int64_t)(CW - 64) * done / total) : 0;
    nv_gfx_rect(32, 122, w, 12, C_BAR);
    char b[48];
    snprintf(b, sizeof b, "%d / %d MB", done >> 20, (total + (1 << 20) - 1) >> 20);
    text_c(146, b, C_TXT, 1);
    text_c(CH - 16, tr("BACK: CANCEL - RESUMES LATER", "INDIETRO: ANNULLA - RIPRENDE DOPO"), C_DIM, 1);
}

// Show a message until back or a tap; always returns 0 (= close the app).
static int wait_message(const char *title, const char *l1, const char *l2) {
    int redraw = 2, prev = 1;
    while (nv_gfx_present()) {
        if (nv_gfx_back() > 0) return 0;
        int x, y;
        const int t = nv_touch(&x, &y);
        if (t && !prev) return 0;
        prev = t;
        if (redraw > 0) { draw_message(title, l1, l2); redraw--; }
    }
    return 0;
}

// Doom's I_Error (patched): log the message and keep it on screen until dismissed, split into
// lines the 5x7 font can show (upper case, 50 chars a line).
void DG_FatalError(const char *msg) {
    char b[160];
    snprintf(b, sizeof b, "doom: fatal: %s", msg);
    nv_log(NV_LOG_ERROR, b);
    nv_snd_shutdown();
    char l[3][52] = {{0}};
    size_t n = strlen(msg), o = 0;
    for (int i = 0; i < 3 && o < n; i++) {
        size_t k = n - o < 50 ? n - o : 50;
        if (o + k < n) {   // break at a blank when possible
            size_t j = k;
            while (j > 20 && msg[o + j] != ' ') j--;
            if (j > 20) k = j;
        }
        char tmp[52];
        memcpy(tmp, msg + o, k);
        tmp[k] = 0;
        upcase(l[i], tmp, sizeof l[i]);
        o += k;
        while (msg[o] == ' ') o++;
    }
    int prev = 1, redraw = 2;
    while (nv_gfx_present()) {
        if (nv_gfx_back() > 0) break;
        int x, y;
        const int t = nv_touch(&x, &y);
        if (t && !prev) break;
        prev = t;
        if (redraw > 0) {
            nv_gfx_clear(C_BG);
            text_c(50, tr("DOOM ERROR", "ERRORE DI DOOM"), C_TXT, 2);
            for (int i = 0; i < 3; i++) text_c(90 + i * 14, l[i], C_DIM, 1);
            text_c(CH - 16, tr("TAP TO CLOSE", "TOCCA PER CHIUDERE"), C_DIM, 1);
            redraw--;
        }
    }
}

// ---- download ------------------------------------------------------------------------------------
static uint8_t s_buf[64 * 1024];

static int sha1_file(const char *path, const char *want, const char *title, int base, int total) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    sha1_context_t ctx;
    SHA1_Init(&ctx);
    int done = 0, last = -1;
    size_t n;
    while ((n = fread(s_buf, 1, sizeof s_buf, f)) > 0) {
        SHA1_Update(&ctx, s_buf, n);
        done += (int)n;
        if ((done >> 20) != last) {
            last = done >> 20;
            draw_progress(title, tr("CHECKING...", "VERIFICA..."), base + done, total);
            if (!nv_gfx_present()) { fclose(f); return 0; }
        }
    }
    fclose(f);
    sha1_digest_t dg;
    SHA1_Final(dg, &ctx);
    char hex[41];
    for (int i = 0; i < 20; i++) snprintf(hex + 2 * i, 3, "%02x", dg[i]);
    return !strcmp(hex, want);
}

// One HTTP GET (a byte range when len > 0) into buf; returns bytes read, <0 on failure
// (-100 = the user backed out).
static int http_get(const char *url, int from, int len, uint8_t *buf, int cap, const char *title,
                    const char *what, int done, int total) {
    char spec[400];
    if (len > 0)
        snprintf(spec, sizeof spec, "{\"url\":\"%s\",\"headers\":{\"Range\":\"bytes=%d-%d\"},"
                 "\"timeout\":30000,\"max\":%d}", url, from, from + len - 1, 1 << 20);
    else
        snprintf(spec, sizeof spec, "{\"url\":\"%s\",\"timeout\":15000,\"max\":%d}", url, cap);
    const int h = nv_http_req(spec, NULL, 0);
    if (h < 0) return h;
    int st = 0;
    while (st == 0) {
        if (title) draw_progress(title, what, done, total);
        if (!nv_gfx_present() || nv_gfx_back() > 0) { nv_http_close(h); return -100; }
        st = nv_http_state(h);
    }
    const int status = st == 1 ? nv_http_status(h) : 0;
    int got = -1;
    if (st == 1 && (len > 0 ? status == 206 : status == 200)) {
        int n;
        got = 0;
        while (got < cap && (n = nv_http_read(h, buf + got, (uint32_t)(cap - got))) > 0) got += n;
    }
    nv_http_close(h);
    return got;
}

// Firmware before 1.1.139: WAMR's WASI path lookup malloc'd a buffer as big as the file for every
// open/stat, so files larger than the largest free PSRAM block (Freedoom: 28 MB) fail with ENOMEM.
// Nothing an app can do about it: ask for the update instead of failing in a loop.
static int s_old_os;
static int update_message(void) {
    return wait_message(tr("UPDATE NUCLEOOS", "AGGIORNA NUCLEOOS"),
                        tr("THIS GAME NEEDS SYSTEM 1.1.139 OR LATER", "SERVE IL SISTEMA 1.1.139 O SUCCESSIVO"),
                        tr("SETTINGS - UPDATE - CHECK", "IMPOSTAZIONI - AGGIORNAMENTO - CONTROLLA"));
}

// Fetch one WAD into dst (resumable .part, whole ranges only). 1 = complete and verified.
enum { DL_CHUNK = 960 * 1024 };
static int download(const gfile_t *g, const char *dst, const char *title, int base, int total) {
    char part[180], url[200];
    snprintf(part, sizeof part, "%s.part", dst);
    snprintf(url, sizeof url, STORE_DATA "%s", g->name);
    int off = file_size(part);
    if (off < 0 || off > (int)g->size) { remove(part); off = 0; }
    uint8_t *chunk = malloc(DL_CHUNK);
    if (!chunk) return 0;
    int fails = 0, ret = 0;
    const char *what = tr("DOWNLOADING FROM THE STORE...", "DOWNLOAD DALLO STORE...");
    while (off < (int)g->size) {
        const int len = (int)g->size - off < DL_CHUNK ? (int)g->size - off : DL_CHUNK;
        const int got = http_get(url, off, len, chunk, len, title, what, base + off, total);
        if (got == -100) goto out;
        if (got == len) {
            // Positioned writes in 64 KB slices (no O_APPEND), then trust the file's real size:
            // a short write resumes from what actually reached the card on the next range.
            FILE *f = fopen(part, off ? "r+b" : "wb");
            int wrote = 0, err = 0;
            if (f && fseek(f, off, SEEK_SET) == 0) {
                while (wrote < len) {
                    const size_t n = (size_t)(len - wrote) < 65536 ? (size_t)(len - wrote) : 65536;
                    if (fwrite(chunk + wrote, 1, n, f) != n) { err = errno ? errno : EIO; break; }
                    wrote += (int)n;
                }
                if (fclose(f) != 0 && !err) err = errno ? errno : EIO;
            } else {
                err = errno ? errno : ENOENT;
                if (f) fclose(f);
            }
            const int now = file_size(part);
            if (err || now != off + len) {
                char b[112];
                snprintf(b, sizeof b, "doom: write at %d failed: %s (file now %d)", off, strerror(err), now);
                nv_log(NV_LOG_ERROR, b);
                if (err == ENOMEM) { s_old_os = 1; goto out; }   // firmware < 1.1.139, see below
                if (now >= 0 && now < off + len) off = now - now % 512;   // keep what is sound
                if (++fails > 6) goto out;
                continue;
            }
            off += len;
            fails = 0;
            continue;
        }
        char b[96];
        snprintf(b, sizeof b, "doom: %s range at %d failed (%d)", g->name, off, got);
        nv_log(NV_LOG_WARN, b);
        if (++fails > 6) goto out;
        const int32_t t0 = nv_millis();   // back off (Wi-Fi drops), keep the screen alive
        while (nv_millis() - t0 < 1500 * fails) {
            draw_progress(title, tr("NETWORK ERROR, RETRYING...", "ERRORE DI RETE, RIPROVO..."),
                          base + off, total);
            if (!nv_gfx_present() || nv_gfx_back() > 0) goto out;
        }
    }
    free(chunk);
    chunk = NULL;
    if (!sha1_file(part, g->sha1, title, base, total)) {
        remove(part);
        nv_log(NV_LOG_ERROR, "doom: download checksum mismatch, removed");
        goto out;
    }
    remove(dst);
    if (rename(part, dst) != 0) { nv_log(NV_LOG_ERROR, "doom: rename failed"); goto out; }
    ret = 1;
out:
    free(chunk);
    return ret;
}

// ---- game descriptor ------------------------------------------------------------------------------
// <id>.game, one directive per line:
//   title <text>                   shown while downloading
//   file <name> <bytes> <sha1>     a file the game needs in the WAD folder
//   iwad <name>                    the base game (one of the files, or already there)
//   pwad <name>                    added on top, in order (up to 4)
//   deh <name>                     DeHackEd patch file applied after the WADs (up to 2)
static int parse_game(const char *txt, game_t *g) {
    memset(g, 0, sizeof *g);
    const char *p = txt;
    while (*p) {
        char line[160];
        size_t n = strcspn(p, "\r\n");
        const size_t adv = n;
        if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, p, n);
        line[n] = 0;
        p += adv;
        p += strspn(p, "\r\n");
        char a[48] = {0}, b[48] = {0};
        unsigned sz = 0;
        if (!strncmp(line, "title ", 6)) {
            snprintf(g->title, sizeof g->title, "%s", line + 6);
        } else if (sscanf(line, "file %47s %u %47s", a, &sz, b) == 3 && g->nfile < 8 && name_ok(a) &&
                   strlen(b) == 40) {
            gfile_t *f = &g->file[g->nfile++];
            snprintf(f->name, sizeof f->name, "%s", a);
            f->size = sz;
            snprintf(f->sha1, sizeof f->sha1, "%s", b);
        } else if (sscanf(line, "iwad %47s", a) == 1 && name_ok(a)) {
            snprintf(g->iwad, sizeof g->iwad, "%s", a);
        } else if (sscanf(line, "pwad %47s", a) == 1 && name_ok(a) && g->npwad < 4) {
            snprintf(g->pwad[g->npwad++], sizeof g->pwad[0], "%s", a);
        } else if (sscanf(line, "deh %47s", a) == 1 && name_ok(a) && g->ndeh < 2) {
            snprintf(g->deh[g->ndeh++], sizeof g->deh[0], "%s", a);
        }
    }
    return g->iwad[0] != 0;
}

static char s_argbuf[8][176];
static char *s_argv[16];

// Prepare a store game: descriptor (fresh from the store, else the cached copy), missing WADs.
// Returns argc for doomgeneric_Create, 0 = close the app.
static int game_mode(const char *id) {
    static char txt[4096];
    char url[160], cache[128];
    game_t g;
    snprintf(url, sizeof url, STORE_DATA "%s.game", id);
    snprintf(cache, sizeof cache, "%sgame.txt", nv_doom_datadir);
    draw_message(tr("LOADING...", "CARICAMENTO..."), NULL, NULL);
    nv_gfx_present();
    int n = http_get(url, 0, 0, (uint8_t *)txt, (int)sizeof txt - 1, NULL, NULL, 0, 0);
    if (n == -100) return 0;
    if (n > 0) {
        txt[n] = 0;
        if (parse_game(txt, &g)) {
            FILE *f = fopen(cache, "wb");
            if (f) { fwrite(txt, 1, (size_t)n, f); fclose(f); }
        } else {
            n = -1;
        }
    }
    if (n <= 0) {   // offline (or a bad answer): the copy from the last successful start
        FILE *f = fopen(cache, "rb");
        n = f ? (int)fread(txt, 1, sizeof txt - 1, f) : 0;
        if (f) fclose(f);
        txt[n > 0 ? n : 0] = 0;
        if (n <= 0 || !parse_game(txt, &g))
            return wait_message(tr("NO CONNECTION", "NESSUNA CONNESSIONE"),
                                tr("THE FIRST START DOWNLOADS THE GAME", "IL PRIMO AVVIO SCARICA IL GIOCO"),
                                tr("CONNECT TO WI-FI AND RETRY", "CONNETTITI AL WI-FI E RIPROVA"));
    }
    // what is missing
    int total = 0, need = 0;
    for (int i = 0; i < g.nfile; i++) {
        char p[176];
        snprintf(p, sizeof p, "%s%s", s_waddir, g.file[i].name);
        if (file_size(p) != (int)g.file[i].size) { total += (int)g.file[i].size; need++; }
    }
    if (need) {
        char title[40];
        upcase(title, g.title[0] ? g.title : id, sizeof title);
        int done = 0;
        for (int i = 0; i < g.nfile; i++) {
            char p[176];
            snprintf(p, sizeof p, "%s%s", s_waddir, g.file[i].name);
            if (file_size(p) == (int)g.file[i].size) continue;
            if (!download(&g.file[i], p, title, done, total))
                return s_old_os ? update_message() : wait_message(tr("DOWNLOAD STOPPED", "DOWNLOAD INTERROTTO"),
                                    tr("OPEN THE GAME AGAIN TO RESUME", "RIAPRI IL GIOCO PER RIPRENDERE"), NULL);
            done += (int)g.file[i].size;
        }
    }
    int argc = 0;
    s_argv[argc++] = "doom";
    s_argv[argc++] = "-iwad";
    snprintf(s_argbuf[0], sizeof s_argbuf[0], "%s%s", s_waddir, g.iwad);
    s_argv[argc++] = s_argbuf[0];
    if (file_size(s_argbuf[0]) <= 0)
        return errno == ENOMEM ? update_message()
                               : wait_message(tr("MISSING GAME DATA", "DATI DEL GIOCO MANCANTI"), g.iwad, NULL);
    if (g.npwad) {
        s_argv[argc++] = "-merge";   // sprites / flats of the PWADs merged into the IWAD's
        for (int i = 0; i < g.npwad; i++) {
            snprintf(s_argbuf[1 + i], sizeof s_argbuf[0], "%s%s", s_waddir, g.pwad[i]);
            s_argv[argc++] = s_argbuf[1 + i];
        }
    }
    if (g.ndeh) {
        s_argv[argc++] = "-deh";
        for (int i = 0; i < g.ndeh; i++) {
            snprintf(s_argbuf[5 + i], sizeof s_argbuf[0], "%s%s", s_waddir, g.deh[i]);
            s_argv[argc++] = s_argbuf[5 + i];
        }
    }
    s_argv[argc] = NULL;
    return argc;
}

// ---- engine tile: IWAD picker ---------------------------------------------------------------------
typedef struct { uint32_t magic; char last[160]; } cfg_t;
#define CFG_MAGIC 0x444f4f31u   // "DOO1"

static int launcher(void) {
    cfg_t cfg = {0};
    if (nv_load("launcher.cfg", &cfg, sizeof cfg) != (int32_t)sizeof cfg || cfg.magic != CFG_MAGIC)
        memset(&cfg, 0, sizeof cfg);
    scan();
    int sel = 0;
    for (int i = 0; i < s_nent; i++)
        if (!strcmp(s_ent[i].path, cfg.last)) sel = i;
    int redraw = 2, prev_touch = 1;
    uint32_t prev_pad = 0;
    while (nv_gfx_present()) {
        if (nv_gfx_back() > 0) return 0;
        int x, y, go = 0;
        const int down = nv_touch(&x, &y);
        if (down && !prev_touch && s_nent) {
            const int i = (y - ROW_Y) / ROW_H;
            if (y >= ROW_Y && i < s_nent) { if (i == sel) go = 1; sel = i; redraw = 2; }
        }
        prev_touch = down;
        const uint32_t p = (uint32_t)nv_gfx_pad(), edge = p & ~prev_pad;
        prev_pad = p;
        if (s_nent) {
            if (edge & NV_PAD_UP) { sel = (sel + s_nent - 1) % s_nent; redraw = 2; }
            if (edge & NV_PAD_DOWN) { sel = (sel + 1) % s_nent; redraw = 2; }
            if (edge & (NV_PAD_A | NV_PAD_START)) go = 1;
        }
        if (go) {
            cfg.magic = CFG_MAGIC;
            snprintf(cfg.last, sizeof cfg.last, "%s", s_ent[sel].path);
            nv_save("launcher.cfg", &cfg, sizeof cfg);
            snprintf(s_argbuf[0], sizeof s_argbuf[0], "%s", s_ent[sel].path);
            s_argv[0] = "doom";
            s_argv[1] = "-iwad";
            s_argv[2] = s_argbuf[0];
            s_argv[3] = NULL;
            return 3;
        }
        if (redraw > 0) { draw_menu(sel); redraw--; }
    }
    return 0;
}

// =================================================================================================
// Entry
// =================================================================================================
static void defaults(void) {
    // Modern mouse layout (config file values win once saved): left fire, right use, middle
    // strafe, wheel = weapon cycle; pads: joystick button 4/5 = previous/next weapon.
    mousebfire = 0;
    mousebuse = 1;
    mousebstrafe = 2;
    mousebforward = -1;
    mousebnextweapon = 3;
    mousebprevweapon = 4;
    joybprevweapon = 4;
    joybnextweapon = 5;
    joybfire = joybstrafe = joybuse = joybspeed = -1;
    // big community maps make saves past the DOS 180 KB buffer; this port has no such limit
    vanilla_savegame_limit = 0;
    vanilla_demo_limit = 0;
}

NV_EXPORT("run")
void run(void) {
    char lang[8] = {0};
    nv_lang(lang, sizeof lang);
    s_it = lang[0] == 'i' && lang[1] == 't';
    // "home" maps /sdcard/home to "/" and the private folder to /appdata; without it the private
    // folder is "/". WADs are shared through /home/doom when possible.
    struct stat st;
    if (stat(FS "appdata", &st) == 0 && S_ISDIR(st.st_mode)) {
        nv_doom_datadir = FS "appdata/";
        s_homedir = FS;
    }
    snprintf(s_waddir, sizeof s_waddir, "%sdoom/", s_homedir ? s_homedir : nv_doom_datadir);
    mkdir(s_waddir, 0777);

    const char *id = getenv("NUCLEO_APP");
    const int argc = (id && *id && strcmp(id, "doom")) ? game_mode(id) : launcher();
    if (argc <= 0) return;
#ifdef NV_SIM   // harness: extra command line (e.g. -warp 20) from DOOM_EXTRA
    int n = argc;
    static char extra[128];
    if (getenv("DOOM_EXTRA")) {
        snprintf(extra, sizeof extra, "%s", getenv("DOOM_EXTRA"));
        for (char *t = strtok(extra, " "); t && n < 15; t = strtok(NULL, " ")) s_argv[n++] = t;
        s_argv[n] = NULL;
    }
    int nargs = n;
#else
    int nargs = argc;
#endif
    nv_gfx_clear(0);
    text_c(110, tr("LOADING...", "CARICAMENTO..."), C_DIM, 2);
    nv_gfx_present();
    nv_gfx_clear(0);   // the other buffer too: DG_Pulse may present it during the startup load
    text_c(110, tr("LOADING...", "CARICAMENTO..."), C_DIM, 2);
    s_last_present = nv_millis();

    defaults();
    nv_snd_setup();
    // Memory adapts in i_system.c (patch_sources.py): the zone is the biggest of 8..4 MB that
    // still leaves 2.5 MB free in the linear memory this run got (9 MB, up to 12 when it can grow).
    doomgeneric_Create(nargs, s_argv);
    {
        char b[96];
        snprintf(b, sizeof b, "doom: zone %u KB", Z_ZoneSize() / 1024);
        nv_log(NV_LOG_INFO, b);
    }
    s_last_present = nv_millis();
    int32_t perf_t0 = nv_millis(), perf_frames = 0;
    s_slept_ms = 0;
    for (;;) {
#ifdef PERF_PROFILE
        const int64_t tp = us_now();
        const int alive = nv_gfx_present();
        g_prof_present += us_now() - tp;
        if (!alive) break;
#else
        if (!nv_gfx_present()) break;
#endif
        s_last_present = nv_millis();
        doomgeneric_Tick();
        perf_frames++;
        const int32_t span = nv_millis() - perf_t0;
        if (span >= PERF_LOG_MS) {   // fps and the busy time per frame (tic waits excluded)
            char b[96];
            snprintf(b, sizeof b, "doom: %d fps, %d ms/frame busy, idle %d%%",
                     perf_frames * 1000 / span, (span - s_slept_ms) / (perf_frames ? perf_frames : 1),
                     s_slept_ms * 100 / span);
            nv_log(NV_LOG_INFO, b);
#ifdef PERF_PROFILE
            const int f = perf_frames ? perf_frames : 1;
            snprintf(b, sizeof b, "doom: per frame us: convert %d, blit %d, mix %d, present %d",
                     (int)(g_prof_draw / f), (int)(g_prof_blit / f), (int)(g_prof_mix / f), (int)(g_prof_present / f));
            nv_log(NV_LOG_INFO, b);
            g_prof_draw = g_prof_mix = g_prof_present = g_prof_blit = 0;
#endif
            perf_t0 = nv_millis();
            perf_frames = 0;
            s_slept_ms = 0;
        }
    }
    nv_snd_shutdown();
}
