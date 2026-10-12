// kbd.c — a USB or Bluetooth keyboard (nv_kbd_state, ABI 14): presses from the held keys of each
// frame, auto-repeat, and the characters of the US or Italian layout (AltGr included).
#include "bib.h"

#include <string.h>

#include "nucleo_sdk.h"

static uint8_t s_prev[6];
static int s_prev_n;
static int s_rep_usage, s_rep_next;   // the key being held for auto-repeat

int kbd_present(void) {
    uint8_t b[8];
    return nv_kbd_state(b, sizeof b) >= 0;
}

// US layout: usages 0x04..0x38 without and with Shift.
static const char kUs[] = "abcdefghijklmnopqrstuvwxyz1234567890\n\x1b\b\t -=[]\\#;'`,./";
static const char kUsS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\n\x1b\b\t _+{}|~:\"~<>?";

static uint32_t italian(int u, int shift, int altgr) {
    if (altgr) {
        switch (u) {
        case 0x08: return 0x20AC;                  // € (AltGr+E)
        case 0x2F: return shift ? '{' : '[';
        case 0x30: return shift ? '}' : ']';
        case 0x33: return '@';
        case 0x34: return '#';
        case 0x35: return '`';
        case 0x2E: return '~';
        default: return 0;
        }
    }
    switch (u) {
    case 0x1F: return shift ? '"' : '2';
    case 0x20: return shift ? 0xA3 : '3';          // £
    case 0x23: return shift ? '&' : '6';
    case 0x24: return shift ? '/' : '7';
    case 0x25: return shift ? '(' : '8';
    case 0x26: return shift ? ')' : '9';
    case 0x27: return shift ? '=' : '0';
    case 0x2D: return shift ? '?' : '\'';
    case 0x2E: return shift ? '^' : 0xEC;          // ì
    case 0x2F: return shift ? 0xE9 : 0xE8;         // é è
    case 0x30: return shift ? '*' : '+';
    case 0x31: case 0x32: return shift ? 0xA7 : 0xF9;   // § ù
    case 0x33: return shift ? 0xE7 : 0xF2;         // ç ò
    case 0x34: return shift ? 0xB0 : 0xE0;         // ° à
    case 0x35: return shift ? '|' : '\\';
    case 0x36: return shift ? ';' : ',';
    case 0x37: return shift ? ':' : '.';
    case 0x38: return shift ? '_' : '-';
    case 0x64: return shift ? '>' : '<';
    default: return 0;
    }
}

static void decode(int u, int mods, int it, kev_t *e) {
    memset(e, 0, sizeof *e);
    e->mods = mods;
    const int shift = mods & M_SHIFT, altgr = mods & M_ALTGR;
    switch (u) {
    case 0x28: case 0x58: e->key = K_ENTER; return;
    case 0x29: e->key = K_ESC; return;
    case 0x2A: e->key = K_BACKSPACE; return;
    case 0x2B: e->key = K_TAB; return;
    case 0x2C: e->key = K_SPACE; e->ch = ' '; return;
    case 0x4A: e->key = K_HOME; return;
    case 0x4B: e->key = K_PGUP; return;
    case 0x4C: e->key = K_DELETE; return;
    case 0x4D: e->key = K_END; return;
    case 0x4E: e->key = K_PGDN; return;
    case 0x4F: e->key = K_RIGHT; return;
    case 0x50: e->key = K_LEFT; return;
    case 0x51: e->key = K_DOWN; return;
    case 0x52: e->key = K_UP; return;
    default: break;
    }
    if (u >= 0x3A && u <= 0x45) { e->key = K_F; e->fn = u - 0x3A + 1; return; }
    uint32_t ch = 0;
    if (it && u >= 0x04) ch = italian(u, shift, altgr);
    const int idx = u - 0x04;
    if (!ch && !altgr && idx >= 0 && idx < (int)sizeof kUs - 1 && idx < (int)sizeof kUsS - 1)
        ch = (unsigned char)(shift ? kUsS[idx] : kUs[idx]);
    if (!ch && u == 0x64) ch = shift ? '>' : '<';
    if (u >= 0x04 && u <= 0x1D && !altgr) ch = (uint32_t)((shift ? 'A' : 'a') + (u - 0x04));
    if (ch >= 0x20) { e->key = K_CHAR; e->ch = ch; }
}

static int repeatable(int u) {
    return u == 0x2A || u == 0x4C || (u >= 0x4F && u <= 0x52) || u == 0x4B || u == 0x4E ||
           (u >= 0x04 && u <= 0x27) || (u >= 0x2C && u <= 0x38);
}

int kbd_poll(kev_t *out, int max, int now_ms, int it) {
    uint8_t b[8] = {0};
    int n = nv_kbd_state(b, sizeof b);
    if (n < 0) { s_prev_n = 0; s_rep_usage = 0; return 0; }
    if (n > 6) n = 6;
    const uint8_t m = b[0];
    int mods = 0;
    if (m & 0x11) mods |= M_CTRL;
    if (m & 0x22) mods |= M_SHIFT;
    if (m & 0x04) mods |= M_ALT;
    if (m & 0x40) mods |= M_ALTGR;
    if (m & 0x88) mods |= M_GUI;
    int k = 0;
    for (int i = 0; i < n && k < max; i++) {
        const int u = b[1 + i];
        if (u <= 3) continue;   // rollover error / none
        int held = 0;
        for (int j = 0; j < s_prev_n; j++) held |= s_prev[j] == u;
        if (held) continue;
        decode(u, mods, it, &out[k]);
        if (out[k].key != K_NONE) k++;
        if (repeatable(u)) { s_rep_usage = u; s_rep_next = now_ms + 450; }
    }
    // auto-repeat of the last key pressed, while it is still held
    int still = 0;
    for (int i = 0; i < n; i++) still |= b[1 + i] == s_rep_usage;
    if (!still) s_rep_usage = 0;
    else if (now_ms - s_rep_next >= 0 && k < max) {
        decode(s_rep_usage, mods, it, &out[k]);
        if (out[k].key != K_NONE) k++;
        s_rep_next = now_ms + 38;
    }
    memcpy(s_prev, b + 1, (size_t)n);
    s_prev_n = n;
    return k;
}
