// Dispositivi — control Shelly, WLED and Tasmota devices on the home network, no Home Assistant
// needed (ABI 13: nv_mdns_browse + nv_http_req, permission "lan").
//
// Discovery: mDNS _shelly._tcp (Gen2+), _wled._tcp, _http._tcp (Shelly Gen1 "shelly*" hosts and
// Tasmota "tasmota*" hosts). Each device is polled for its on/off state; tap toggles it:
//   Shelly Gen2  GET /rpc/Switch.GetStatus?id=0 ("output":true) / GET /rpc/Switch.Toggle?id=0
//   Shelly Gen1  GET /relay/0 ("ison":true)                      / GET /relay/0?turn=toggle
//   WLED         GET /json/state ("on":true)                     / POST /json/state {"on":"t"}
//   Tasmota      GET /cm?cmnd=Power ("POWER":"ON")               / GET /cm?cmnd=Power%20TOGGLE
#include "nucleo_sdk.h"

#define W 1024
#define H 600
#define MAX_DEV 24

static int s_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int s_eq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void s_cpy(char *d, const char *s, int n) { int i = 0; for (; i < n - 1 && s[i]; i++) d[i] = s[i]; d[i] = 0; }
static int starts(const char *s, const char *p) { while (*p) { char a = *s++, b = *p++; if (a >= 'A' && a <= 'Z') a += 32; if (a != b) return 0; } return 1; }
static int has(const char *hay, int n, const char *needle) {
    int nl = s_len(needle);
    for (int i = 0; i + nl <= n; i++) { int k = 0; while (k < nl && hay[i + k] == needle[k]) k++; if (k == nl) return 1; }
    return 0;
}

#define C_BG     NV_RGB(14, 17, 22)
#define C_BAR    NV_RGB(22, 27, 34)
#define C_TILE   NV_RGB(33, 39, 48)
#define C_LO     NV_RGB(24, 29, 36)
#define C_TEXT   NV_RGB(230, 234, 240)
#define C_DIM    NV_RGB(140, 150, 165)
#define C_ON     NV_RGB(25, 110, 70)
#define C_ACCENT NV_RGB(99, 102, 241)
#define C_RED    NV_RGB(239, 68, 68)

static void rrect(int x, int y, int w, int h, int col) {
    nv_gfx_rect(x + 6, y, w - 12, h, col);
    nv_gfx_rect(x, y + 6, 6, h - 12, col);
    nv_gfx_rect(x + w - 6, y + 6, 6, h - 12, col);
    nv_gfx_circle(x + 6, y + 6, 6, col); nv_gfx_circle(x + w - 7, y + 6, 6, col);
    nv_gfx_circle(x + 6, y + h - 7, 6, col); nv_gfx_circle(x + w - 7, y + h - 7, 6, col);
}
static void text_center(int cx, int y, const char *s, int col, int sc) { nv_gfx_text(cx - nv_gfx_text_width(s, sc) / 2, y, s, col, sc); }

enum { K_SHELLY2, K_SHELLY1, K_WLED, K_TASMOTA };
static const char *const kKind[] = {"SHELLY", "SHELLY", "WLED", "TASMOTA"};

typedef struct {
    char name[28];
    char ip[16];
    uint8_t kind;
    int8_t on;          // -1 unknown, 0/1
    int8_t offline;
} Dev;

static Dev  g_dev[MAX_DEV];
static int  g_n = 0;
static char g_buf[6144];
static int  g_it = 0;
#define T(en, it) (g_it ? (it) : (en))

// mDNS instance name -> font-safe label
static void fold(char *d, const char *s, int n) {
    int o = 0;
    for (int i = 0; s[i] && o < n - 1; i++) {
        char c = s[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '.')) c = ' ';
        d[o++] = c;
    }
    d[o] = 0;
}

static void add_dev(const char *inst, const char *host, const char *ip, int kind) {
    if (!ip[0] || g_n >= MAX_DEV) return;
    for (int i = 0; i < g_n; i++) if (s_eq(g_dev[i].ip, ip)) return;   // same device twice
    Dev *d = &g_dev[g_n++];
    fold(d->name, inst[0] ? inst : host, sizeof d->name);
    s_cpy(d->ip, ip, sizeof d->ip);
    d->kind = (uint8_t)kind;
    d->on = -1;
    d->offline = 0;
}

// Parse browse lines "instance|host|ip|port|txt" for one service.
static void parse_browse(int n, int svc) {
    g_buf[n] = 0;
    char *line = g_buf;
    while (*line) {
        char *f[5] = {line, 0, 0, 0, 0};
        int k = 1;
        char *p = line;
        for (; *p && *p != '\n'; p++) if (*p == '|' && k < 5) { *p = 0; f[k++] = p + 1; }
        const int last = !*p;
        *p = 0;
        if (k >= 3) {
            if (svc == 0) add_dev(f[0], f[1], f[2], K_SHELLY2);
            else if (svc == 1) add_dev(f[0], f[1], f[2], K_WLED);
            else if (starts(f[1], "shelly")) add_dev(f[0], f[1], f[2], K_SHELLY1);
            else if (starts(f[1], "tasmota")) add_dev(f[0], f[1], f[2], K_TASMOTA);
        }
        if (last) break;
        line = p + 1;
    }
}

// ---------------------------------------------------------------- request queue (one at a time)
enum { Q_BROWSE, Q_STATE, Q_TOGGLE };
static int g_h = -1, g_qkind = 0, g_qarg = 0;
static int g_browse_step = 0;       // 0..2 services, 3 = done
static int g_poll_idx = 0, g_next_poll = 0;
static int g_scanning = 1;

static int start_state(int i) {
    char spec[160];
    const Dev *d = &g_dev[i];
    switch (d->kind) {
    case K_SHELLY2: nv_snprintf(spec, sizeof spec, "{\"url\":\"http://%s/rpc/Switch.GetStatus?id=0\",\"timeout\":3000}", d->ip); break;
    case K_SHELLY1: nv_snprintf(spec, sizeof spec, "{\"url\":\"http://%s/relay/0\",\"timeout\":3000}", d->ip); break;
    case K_WLED:    nv_snprintf(spec, sizeof spec, "{\"url\":\"http://%s/json/state\",\"timeout\":3000}", d->ip); break;
    default:        nv_snprintf(spec, sizeof spec, "{\"url\":\"http://%s/cm?cmnd=Power\",\"timeout\":3000}", d->ip); break;
    }
    return nv_http_req(spec, 0, 0);
}

static int start_toggle(int i) {
    char spec[180];
    const Dev *d = &g_dev[i];
    switch (d->kind) {
    case K_SHELLY2: nv_snprintf(spec, sizeof spec, "{\"url\":\"http://%s/rpc/Switch.Toggle?id=0\",\"timeout\":3000}", d->ip); break;
    case K_SHELLY1: nv_snprintf(spec, sizeof spec, "{\"url\":\"http://%s/relay/0?turn=toggle\",\"timeout\":3000}", d->ip); break;
    case K_WLED:
        nv_snprintf(spec, sizeof spec, "{\"url\":\"http://%s/json/state\",\"method\":\"POST\",\"headers\":{\"Content-Type\":\"application/json\"},\"timeout\":3000}", d->ip);
        return nv_http_req(spec, "{\"on\":\"t\"}", 10);
    default: nv_snprintf(spec, sizeof spec, "{\"url\":\"http://%s/cm?cmnd=Power%%20TOGGLE\",\"timeout\":3000}", d->ip); break;
    }
    return nv_http_req(spec, 0, 0);
}

static int g_pending_toggle = -1;
static int g_err = 0;

static int net_step(int now) {
    int changed = 0;
    if (g_h >= 0) {
        const int st = nv_http_state(g_h);
        if (st == 0) return 0;
        int n = 0, r;
        if (st == 1) while ((r = nv_http_read(g_h, g_buf + n, sizeof g_buf - 1 - n)) > 0) n += r;
        g_buf[n] = 0;
        if (g_qkind == Q_BROWSE) {
            if (st == 1) parse_browse(n, g_browse_step); else g_err = st;
            g_browse_step++;
            if (g_browse_step >= 3) g_scanning = 0;
        } else if (g_qkind == Q_STATE) {
            Dev *d = &g_dev[g_qarg];
            if (st == 1 && nv_http_status(g_h) == 200) {
                d->offline = 0;
                d->on = (int8_t)(has(g_buf, n, "\"output\":true") || has(g_buf, n, "\"ison\":true") ||
                                 has(g_buf, n, "\"on\":true") || has(g_buf, n, "\"POWER\":\"ON\""));
            } else d->offline = 1;
        } else {
            g_next_poll = now + 300;                // read back the real state soon
            g_poll_idx = g_qarg;
        }
        nv_http_close(g_h);
        g_h = -1;
        changed = 1;
    }
    // next job: browse > toggle > state poll
    if (g_browse_step < 3) {
        static const char *const svc[3][2] = {{"_shelly", "_tcp"}, {"_wled", "_tcp"}, {"_http", "_tcp"}};
        g_h = nv_mdns_browse(svc[g_browse_step][0], svc[g_browse_step][1]);
        g_qkind = Q_BROWSE;
        if (g_h < 0) { g_err = g_h; g_h = -1; g_browse_step = 3; g_scanning = 0; changed = 1; }
    } else if (g_pending_toggle >= 0) {
        g_qarg = g_pending_toggle;
        g_pending_toggle = -1;
        g_h = start_toggle(g_qarg);
        g_qkind = Q_TOGGLE;
        if (g_h < 0) { g_err = g_h; g_h = -1; }
    } else if (g_n && now >= g_next_poll) {
        g_qarg = g_poll_idx % g_n;
        g_poll_idx = (g_poll_idx + 1) % g_n;
        g_h = start_state(g_qarg);
        g_qkind = Q_STATE;
        if (g_h < 0) { g_err = g_h; g_h = -1; }
        g_next_poll = now + (g_poll_idx == 0 ? 4000 : 150);   // sweep, then rest
    }
    return changed;
}

// ---------------------------------------------------------------- UI
#define GX 16
#define GY 84
#define COLS 4
#define TW 237
#define TH 150
#define GAP 12

static void draw(void) {
    nv_gfx_clear(C_BG);
    nv_gfx_rect(0, 0, W, 70, C_BAR);
    nv_gfx_text(20, 20, T("DEVICES", "DISPOSITIVI"), C_TEXT, 4);
    rrect(W - 190, 15, 170, 40, C_TILE);
    text_center(W - 105, 28, T("SEARCH", "CERCA"), C_TEXT, 2);
    if (g_scanning) nv_gfx_text(360, 28, T("SEARCHING THE NETWORK...", "RICERCA IN RETE..."), C_DIM, 2);
    else if (g_err) { char m[32]; nv_snprintf(m, sizeof m, "%s %d", T("ERROR", "ERRORE"), g_err); nv_gfx_text(360, 28, m, C_RED, 2); }
    if (!g_n && !g_scanning) {
        text_center(W / 2, 250, T("NO DEVICES FOUND", "NESSUN DISPOSITIVO TROVATO"), C_TEXT, 3);
        text_center(W / 2, 300, T("SHELLY, WLED AND TASMOTA ON THIS WI-FI ARE FOUND AUTOMATICALLY",
                                  "SHELLY, WLED E TASMOTA SU QUESTO WI-FI VENGONO TROVATI DA SOLI"), C_DIM, 2);
        return;
    }
    for (int i = 0; i < g_n && i < 12; i++) {
        const Dev *d = &g_dev[i];
        const int x = GX + (i % COLS) * (TW + GAP), y = GY + (i / COLS) * (TH + GAP);
        const int bg = d->offline ? C_LO : d->on == 1 ? C_ON : C_TILE;
        rrect(x, y + 4, TW, TH, C_LO);
        rrect(x, y, TW, TH, bg);
        nv_gfx_text(x + 16, y + 16, kKind[d->kind], C_DIM, 2);
        char l[20];
        s_cpy(l, d->name, 19);
        nv_gfx_text(x + 16, y + 50, l, C_TEXT, 2);
        nv_gfx_text(x + 16, y + 72, d->ip, C_DIM, 1);
        const char *st = d->offline ? T("OFFLINE", "NON RAGGIUNGIBILE") : d->on < 0 ? "..." : d->on ? T("ON", "ACCESO") : T("OFF", "SPENTO");
        nv_gfx_text(x + 16, y + TH - 40, st, d->on == 1 ? C_TEXT : C_DIM, 3);
    }
}

static int in(int x, int y, int rx, int ry, int rw, int rh) { return x >= rx && x < rx + rw && y >= ry && y < ry + rh; }

NV_EXPORT("run")
void run(void) {
    char lang[8] = "en";
    nv_lang(lang, sizeof lang);
    g_it = lang[0] == 'i' && lang[1] == 't';
    int redraw = 2, prev = 0, dx = 0, dy = 0;
    while (nv_gfx_present()) {
        const int now = nv_millis();
        if (nv_gfx_back()) break;
        if (net_step(now)) redraw = 2;
        int x, y;
        const int down = nv_touch(&x, &y);
        if (down && !prev) { dx = x; dy = y; }
        if (!down && prev) {
            if (in(dx, dy, W - 190, 15, 170, 40) && !g_scanning) {    // search again
                g_n = 0; g_browse_step = 0; g_scanning = 1; g_err = 0;
            }
            for (int i = 0; i < g_n && i < 12; i++)
                if (in(dx, dy, GX + (i % COLS) * (TW + GAP), GY + (i / COLS) * (TH + GAP), TW, TH) && !g_dev[i].offline) {
                    g_pending_toggle = i;
                    if (g_dev[i].on >= 0) g_dev[i].on = (int8_t)!g_dev[i].on;   // optimistic
                    nv_gfx_tone(1200, 15);
                }
            redraw = 2;
        }
        prev = down;
        if (redraw > 0) { draw(); redraw--; }
    }
    if (g_h >= 0) nv_http_close(g_h);
}
