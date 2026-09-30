// Casa — Home Assistant dashboard for NucleoOS (ABI v12, permission "ha").
//
// Home Assistant does the heavy lifting: one POST /api/template renders the entities we show as
// compact text lines ("domain|entity_id|state|name|area|brightness|unit|target|current"), so the app
// never parses the big /api/states JSON inside its 64 KB. The token stays in the OS (Settings > Home);
// nv_ha_req adds it host-side. Tap = toggle / activate, hold = details (brightness, thermostat,
// cover). Refresh every 5 s and right after every action.
#include "nucleo_sdk.h"

#define W 1024
#define H 600
#define MAX_ENT   64
#define MAX_AREA  10
#define RESP_CAP  14000

// ---------------------------------------------------------------- tiny libc
static int s_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int s_eq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void s_cpy(char *d, const char *s, int n) { int i = 0; for (; i < n - 1 && s[i]; i++) d[i] = s[i]; d[i] = 0; }
static int s_atoi(const char *s) { int v = 0, neg = 0; if (*s == '-') { neg = 1; s++; } while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); return neg ? -v : v; }
// "21.5" -> 215 (tenths)
static int s_tenths(const char *s) {
    int neg = 0, v = 0, f = 0, seen = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); seen = 1; }
    if (*s == '.' && s[1] >= '0' && s[1] <= '9') { f = s[1] - '0'; seen = 1; }
    if (!seen) return -9999;
    v = v * 10 + f;
    return neg ? -v : v;
}

// ---------------------------------------------------------------- colors / helpers
#define C_BG      NV_RGB(14, 17, 22)
#define C_BAR     NV_RGB(22, 27, 34)
#define C_TILE    NV_RGB(33, 39, 48)
#define C_TILE_LO NV_RGB(24, 29, 36)
#define C_TEXT    NV_RGB(230, 234, 240)
#define C_DIM     NV_RGB(140, 150, 165)
#define C_ACCENT  NV_RGB(99, 102, 241)
#define C_LIGHT   NV_RGB(250, 204, 21)
#define C_SWITCH  NV_RGB(56, 189, 248)
#define C_GREEN   NV_RGB(34, 197, 94)
#define C_RED     NV_RGB(239, 68, 68)
#define C_ORANGE  NV_RGB(249, 115, 22)

static void rrect(int x, int y, int w, int h, int col) {   // cheap rounded rect: 3 rects
    nv_gfx_rect(x + 6, y, w - 12, h, col);
    nv_gfx_rect(x, y + 6, 6, h - 12, col);
    nv_gfx_rect(x + w - 6, y + 6, 6, h - 12, col);
    nv_gfx_circle(x + 6, y + 6, 6, col); nv_gfx_circle(x + w - 7, y + 6, 6, col);
    nv_gfx_circle(x + 6, y + h - 7, 6, col); nv_gfx_circle(x + w - 7, y + h - 7, 6, col);
}

static void text_center(int cx, int y, const char *s, int col, int scale) {
    nv_gfx_text(cx - nv_gfx_text_width(s, scale) / 2, y, s, col, scale);
}

// Names -> what the 5x7 font can draw: uppercase ASCII, accents folded, others -> space.
static void fold(char *d, const char *s, int n) {
    int o = 0;
    for (int i = 0; s[i] && o < n - 1; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0xC3 && s[i + 1]) {              // Latin-1 supplement in UTF-8
            unsigned char c2 = (unsigned char)s[++i];
            const char *m = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTSaaaaaaaceeeeiiiidnooooo/ouuuuyty";
            c = (c2 >= 0x80 && c2 <= 0xBF) ? (unsigned char)m[c2 - 0x80] : ' ';
        } else if (c >= 0x80) { continue; }
        if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 32);
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '.' ||
              c == ':' || c == '%' || c == '/' || c == '+')) c = ' ';
        d[o++] = (char)c;
    }
    d[o] = 0;
}

// ---------------------------------------------------------------- model
enum { D_LIGHT, D_SWITCH, D_FAN, D_BOOL, D_COVER, D_CLIMATE, D_SCENE, D_SCRIPT, D_LOCK, D_SENSOR, D_BINARY, D_MEDIA, D_OTHER };
static const char *const kDom[] = {"light", "switch", "fan", "input_boolean", "cover", "climate", "scene",
                                   "script", "lock", "sensor", "binary_sensor", "media_player", 0};

typedef struct {
    char    id[56];
    char    name[26];     // folded for the font
    char    state[16];
    char    unit[6];
    uint8_t dom, area;
    uint8_t bri;          // 0..255 (lights)
    int16_t target, current;   // tenths (climate)
} Ent;

static Ent  g_ent[MAX_ENT];
static int  g_n = 0;
static char g_area[MAX_AREA][20];
static int  g_nareas = 0;
static char g_resp[RESP_CAP];
static char g_lang[8] = "en";
static int  g_it = 0;

#define T(en, it) (g_it ? (it) : (en))

static int dom_of(const char *d) { for (int i = 0; kDom[i]; i++) if (s_eq(kDom[i], d)) return i; return D_OTHER; }

// Split one "a|b|c" line into fields (in place).
static int split(char *line, char **f, int max) {
    int n = 0;
    f[n++] = line;
    for (char *p = line; *p && n < max; p++)
        if (*p == '|') { *p = 0; f[n++] = p + 1; }
    return n;
}

static int area_index(const char *a) {
    if (!a[0]) return 0xff;
    char folded[20];
    fold(folded, a, sizeof folded);
    for (int i = 0; i < g_nareas; i++) if (s_eq(g_area[i], folded)) return i;
    if (g_nareas >= MAX_AREA) return 0xff;
    s_cpy(g_area[g_nareas], folded, sizeof g_area[0]);
    return g_nareas++;
}

static void parse(int len) {
    g_resp[len] = 0;
    g_n = 0;
    g_nareas = 0;
    char *line = g_resp;
    while (*line && g_n < MAX_ENT) {
        char *nl = line;
        while (*nl && *nl != '\n') nl++;
        const int last = !*nl;
        *nl = 0;
        char *f[9];
        if (split(line, f, 9) >= 9) {
            Ent *e = &g_ent[g_n];
            e->dom = (uint8_t)dom_of(f[0]);
            s_cpy(e->id, f[1], sizeof e->id);
            s_cpy(e->state, f[2], sizeof e->state);
            fold(e->name, f[3], sizeof e->name);
            e->area = (uint8_t)area_index(f[4]);
            e->bri = (uint8_t)s_atoi(f[5]);
            fold(e->unit, f[6], sizeof e->unit);
            e->target = (int16_t)s_tenths(f[7]);
            e->current = (int16_t)s_tenths(f[8]);
            if (e->id[0]) g_n++;
        }
        if (last) break;
        line = nl + 1;
    }
}

// The template: what we show, grouped by area, sorted by name, at most 60 entities.
static const char kTemplate[] =
    "{\"template\":\""
    "{%- set ns = namespace(n=0) -%}"
    "{%- for s in states|sort(attribute='name') if ns.n < 60 and ("
    " s.domain in ['light','switch','fan','input_boolean','cover','climate','scene','script','lock','media_player']"
    " or (s.domain == 'sensor' and s.attributes.device_class in ['temperature','humidity','power','energy','battery'])"
    " or (s.domain == 'binary_sensor' and s.attributes.device_class in ['door','window','motion','occupancy','opening'])) -%}"
    "{%- set ns.n = ns.n + 1 -%}"
    "{{ s.domain }}|{{ s.entity_id }}|{{ s.state }}|{{ (s.name or s.entity_id)|replace('|','/') }}|"
    "{{ (area_name(s.entity_id) or '')|replace('|','/') }}|{{ s.attributes.brightness|default(0,true)|int }}|"
    "{{ s.attributes.unit_of_measurement|default('',true) }}|{{ s.attributes.temperature|default('',true) }}|"
    "{{ s.attributes.current_temperature|default('',true) }}\\n"
    "{%- endfor -%}\"}";

// ---------------------------------------------------------------- networking
static int g_req = -1;           // template request in flight
static int g_act = -1;           // service call in flight
static int g_err = 0;            // last error (NV_NET_E_* or HTTP status >= 400)
static int g_loaded = 0;
static int g_next_poll = 0;

static void poll_start(void) {
    if (g_req >= 0) return;
    g_req = nv_ha_req("POST", "/api/template", kTemplate, sizeof kTemplate - 1);
    if (g_req < 0) { g_err = g_req; g_req = -1; }
}

// Returns 1 when new data arrived (redraw).
static int poll_step(int now) {
    int changed = 0;
    if (g_req >= 0) {
        const int st = nv_http_state(g_req);
        if (st == 1) {
            const int status = nv_http_status(g_req);
            int n = 0, r;
            while ((r = nv_http_read(g_req, g_resp + n, RESP_CAP - 1 - n)) > 0) n += r;
            if (status == 200) { parse(n); g_err = 0; g_loaded = 1; }
            else g_err = status;
            nv_http_close(g_req);
            g_req = -1;
            changed = 1;
            g_next_poll = now + 5000;
        } else if (st < 0) {
            g_err = st;
            nv_http_close(g_req);
            g_req = -1;
            changed = 1;
            g_next_poll = now + 5000;
        }
    }
    if (g_act >= 0) {
        const int st = nv_http_state(g_act);
        if (st != 0) {
            if (st < 0 || nv_http_status(g_act) >= 400) { g_err = st < 0 ? st : nv_http_status(g_act); changed = 1; }
            nv_http_close(g_act);
            g_act = -1;
            g_next_poll = now + 400;          // let HA apply it, then show the real state
        }
    }
    if (g_req < 0 && now >= g_next_poll) poll_start();
    return changed;
}

static void call(const char *domain, const char *service, const Ent *e, const char *extra) {
    char path[96], body[200];
    nv_snprintf(path, sizeof path, "/api/services/%s/%s", domain, service);
    nv_snprintf(body, sizeof body, "{\"entity_id\":\"%s\"%s}", e->id, extra ? extra : "");
    if (g_act >= 0) { nv_http_close(g_act); g_act = -1; }
    g_act = nv_ha_req("POST", path, body, (uint32_t)s_len(body));
    if (g_act < 0) { g_err = g_act; g_act = -1; }
    nv_gfx_tone(1200, 15);
}

static int is_on(const Ent *e) {
    return s_eq(e->state, "on") || s_eq(e->state, "open") || s_eq(e->state, "unlocked") ||
           s_eq(e->state, "playing") || s_eq(e->state, "heat") || s_eq(e->state, "cool") ||
           s_eq(e->state, "heat_cool") || s_eq(e->state, "auto");
}

// Tap: the obvious action; optimistic state flip so the tile reacts at once.
static void tap_action(Ent *e) {
    switch (e->dom) {
    case D_LIGHT: case D_SWITCH: case D_FAN: case D_BOOL:
        call(kDom[e->dom], "toggle", e, 0);
        s_cpy(e->state, is_on(e) ? "off" : "on", sizeof e->state);
        break;
    case D_COVER: call("cover", "toggle", e, 0); break;
    case D_SCENE: case D_SCRIPT: call(kDom[e->dom], "turn_on", e, 0); break;
    case D_MEDIA: call("media_player", "media_play_pause", e, 0); break;
    default: break;                              // sensors, locks and climate: hold for details
    }
}

// ---------------------------------------------------------------- UI state
enum { SCR_GRID, SCR_DETAIL };
static int g_scr = SCR_GRID;
static int g_sel_area = -1;       // -1 = all
static int g_page = 0;
static int g_detail = -1;         // entity index
static int g_redraw = 2;

#define GRID_X 16
#define GRID_Y 84
#define COLS 4
#define ROWS 3
#define TW 237
#define TH 158
#define GAP 12

static int visible(int i) { return g_sel_area < 0 || g_ent[i].area == g_sel_area; }
static int count_visible(void) { int n = 0; for (int i = 0; i < g_n; i++) n += visible(i); return n; }
// Entity index shown in grid slot `slot` of the current page, or -1.
static int slot_entity(int slot) {
    int want = g_page * COLS * ROWS + slot, k = 0;
    for (int i = 0; i < g_n; i++) if (visible(i)) { if (k == want) return i; k++; }
    return -1;
}

// Header chips: "All" + areas. Returns x-extent array filled for hit tests.
static int g_chip_x[MAX_AREA + 1], g_chip_w[MAX_AREA + 1], g_nchips = 0;

// ---------------------------------------------------------------- drawing
static int tile_color(const Ent *e) {
    if (!is_on(e)) return C_TILE;
    switch (e->dom) {
    case D_LIGHT: return NV_RGB(120, 94, 16);
    case D_CLIMATE: return NV_RGB(120, 60, 20);
    case D_LOCK: return NV_RGB(120, 30, 30);
    case D_COVER: return NV_RGB(20, 84, 110);
    default: return NV_RGB(25, 78, 110);
    }
}

static void icon(const Ent *e, int x, int y, int col) {
    switch (e->dom) {
    case D_LIGHT:
        nv_gfx_circle(x + 16, y + 14, 12, col);
        nv_gfx_rect(x + 10, y + 26, 12, 8, col);
        nv_gfx_rect(x + 12, y + 35, 8, 3, col);
        break;
    case D_SWITCH: case D_BOOL: case D_FAN:
        rrect(x, y + 8, 40, 20, col);
        nv_gfx_circle(is_on(e) ? x + 29 : x + 11, y + 18, 7, C_TILE_LO);
        break;
    case D_COVER:
        for (int i = 0; i < 4; i++) nv_gfx_rect(x + 2, y + 4 + i * 8, 30, 5, col);
        break;
    case D_CLIMATE: case D_SENSOR:
        nv_gfx_rect(x + 12, y + 2, 8, 26, col);
        nv_gfx_circle(x + 16, y + 30, 8, col);
        break;
    case D_SCENE: case D_SCRIPT:
        nv_gfx_tri(x + 16, y + 2, x + 4, y + 34, x + 28, y + 34, col);
        nv_gfx_tri(x + 16, y + 38, x + 4, y + 12, x + 28, y + 12, col);
        break;
    case D_LOCK:
        nv_gfx_circle(x + 16, y + 12, 10, col);
        nv_gfx_circle(x + 16, y + 12, 5, C_TILE_LO);
        nv_gfx_rect(x + 4, y + 16, 24, 20, col);
        break;
    case D_BINARY:
        nv_gfx_rect(x + 6, y + 2, 22, 34, col);
        nv_gfx_rect(x + 10, y + 6, 14, 26, C_TILE_LO);
        break;
    default:
        nv_gfx_circle(x + 16, y + 18, 12, col);
        break;
    }
}

static void state_text(const Ent *e, char *out, int n) {
    if (e->dom == D_SENSOR) {
        char v[16];
        fold(v, e->state, sizeof v);
        nv_snprintf(out, (size_t)n, "%s %s", v, e->unit);
        return;
    }
    if (e->dom == D_CLIMATE) {
        if (e->current > -9999) nv_snprintf(out, (size_t)n, "%d.%d", e->current / 10, (e->current < 0 ? -e->current : e->current) % 10);
        else fold(out, e->state, n);
        return;
    }
    if (e->dom == D_LIGHT && is_on(e) && e->bri) { nv_snprintf(out, (size_t)n, "%d%%", (e->bri * 100 + 127) / 255); return; }
    if (s_eq(e->state, "on")) { s_cpy(out, e->dom == D_BINARY ? T("DETECTED", "RILEVATO") : T("ON", "ACCESO"), n); return; }
    if (s_eq(e->state, "off")) { s_cpy(out, e->dom == D_BINARY ? T("CLEAR", "LIBERO") : T("OFF", "SPENTO"), n); return; }
    if (s_eq(e->state, "open")) { s_cpy(out, T("OPEN", "APERTO"), n); return; }
    if (s_eq(e->state, "closed")) { s_cpy(out, T("CLOSED", "CHIUSO"), n); return; }
    if (s_eq(e->state, "locked")) { s_cpy(out, T("LOCKED", "CHIUSA"), n); return; }
    if (s_eq(e->state, "unlocked")) { s_cpy(out, T("UNLOCKED", "APERTA"), n); return; }
    if (s_eq(e->state, "unavailable")) { s_cpy(out, T("OFFLINE", "NON DISP."), n); return; }
    if (e->dom == D_SCENE || e->dom == D_SCRIPT) { s_cpy(out, T("RUN", "AVVIA"), n); return; }
    fold(out, e->state, n);
}

static void draw_tile(int slot, int i) {
    const int cx = slot % COLS, cy = slot / COLS;
    const int x = GRID_X + cx * (TW + GAP), y = GRID_Y + cy * (TH + GAP);
    const Ent *e = &g_ent[i];
    const int bg = tile_color(e);
    rrect(x, y + 4, TW, TH, C_TILE_LO);
    rrect(x, y, TW, TH, bg);
    const int on = is_on(e);
    const int ic = on ? (e->dom == D_LIGHT ? C_LIGHT : C_TEXT) : C_DIM;
    icon(e, x + 16, y + 14, ic);
    // name: up to 2 lines of 18 chars at scale 2
    char l1[20], l2[20];
    int n = s_len(e->name), cut = n <= 18 ? n : 18;
    if (n > 18) for (int k = 18; k > 8; k--) if (e->name[k] == ' ') { cut = k; break; }
    s_cpy(l1, e->name, cut + 1);
    s_cpy(l2, n > cut ? e->name + cut + (e->name[cut] == ' ') : "", 19);
    nv_gfx_text(x + 16, y + 62, l1, C_TEXT, 2);
    if (l2[0]) nv_gfx_text(x + 16, y + 82, l2, C_TEXT, 2);
    char st[24];
    state_text(e, st, sizeof st);
    nv_gfx_text(x + 16, y + TH - 36, st, on ? C_TEXT : C_DIM, 3);
}

static void draw_header(void) {
    nv_gfx_rect(0, 0, W, 70, C_BAR);
    nv_gfx_text(20, 20, T("HOME", "CASA"), C_TEXT, 4);
    int x = 150;
    g_nchips = 0;
    for (int a = -1; a < g_nareas && g_nchips <= MAX_AREA; a++) {
        const char *lab = a < 0 ? T("ALL", "TUTTE") : g_area[a];
        const int w = nv_gfx_text_width(lab, 2) + 28;
        if (x + w > W - 150) break;
        const int sel = (a == g_sel_area);
        rrect(x, 15, w, 40, sel ? C_ACCENT : C_TILE);
        nv_gfx_text(x + 14, 28, lab, C_TEXT, 2);
        g_chip_x[g_nchips] = x; g_chip_w[g_nchips] = w; g_nchips++;
        x += w + 8;
    }
    // pager
    const int pages = (count_visible() + COLS * ROWS - 1) / (COLS * ROWS);
    if (pages > 1) {
        rrect(W - 136, 15, 56, 40, C_TILE); nv_gfx_text(W - 118, 28, "<", C_TEXT, 2);
        rrect(W - 72, 15, 56, 40, C_TILE);  nv_gfx_text(W - 54, 28, ">", C_TEXT, 2);
    }
    if (g_err) {
        char m[40];
        nv_snprintf(m, sizeof m, "%s %d", T("ERROR", "ERRORE"), g_err);
        nv_gfx_text(W - 140 - nv_gfx_text_width(m, 2) - (pages > 1 ? 0 : -130), 28, m, C_RED, 2);
    }
}

static void draw_grid(void) {
    nv_gfx_clear(C_BG);
    draw_header();
    if (!g_loaded) {
        text_center(W / 2, 280, g_err ? T("CANNOT REACH HOME ASSISTANT", "HOME ASSISTANT NON RAGGIUNGIBILE")
                                      : T("LOADING...", "CARICAMENTO..."), C_DIM, 3);
        return;
    }
    if (!g_n) { text_center(W / 2, 280, T("NO DEVICES", "NESSUN DISPOSITIVO"), C_DIM, 3); return; }
    for (int s = 0; s < COLS * ROWS; s++) {
        const int i = slot_entity(s);
        if (i >= 0) draw_tile(s, i);
    }
}

static void draw_not_configured(void) {
    nv_gfx_clear(C_BG);
    text_center(W / 2, 170, T("HOME ASSISTANT IS NOT SET UP", "HOME ASSISTANT NON CONFIGURATO"), C_TEXT, 3);
    text_center(W / 2, 240, T("SETTINGS > HOME: URL AND ACCESS TOKEN", "IMPOSTAZIONI > CASA: INDIRIZZO E TOKEN"), C_DIM, 2);
    text_center(W / 2, 280, T("OR PASTE THEM FROM A PC: HTTP://NUCLEOV2.LOCAL", "O INCOLLALI DA PC: HTTP://NUCLEOV2.LOCAL"), C_DIM, 2);
    text_center(W / 2, 360, T("HOME ASSISTANT: PROFILE > SECURITY > LONG-LIVED TOKEN", "HOME ASSISTANT: PROFILO > SICUREZZA > TOKEN A LUNGA DURATA"), C_DIM, 2);
}

// Detail overlay: brightness slider (lights), target temperature (climate), cover buttons, lock.
#define DX 212
#define DY 110
#define DW 600
#define DH 380
static void draw_detail(void) {
    const Ent *e = &g_ent[g_detail];
    nv_gfx_clear(C_BG);
    rrect(DX, DY, DW, DH, C_BAR);
    nv_gfx_text(DX + 30, DY + 26, e->name, C_TEXT, 3);
    char st[24];
    state_text(e, st, sizeof st);
    nv_gfx_text(DX + 30, DY + 66, st, C_DIM, 2);
    rrect(DX + DW - 70, DY + 16, 54, 44, C_TILE); nv_gfx_text(DX + DW - 52, DY + 30, "X", C_TEXT, 2);
    if (e->dom == D_LIGHT) {
        const int pct = is_on(e) ? (e->bri * 100 + 127) / 255 : 0;
        rrect(DX + 40, DY + 150, DW - 80, 70, C_TILE_LO);
        if (pct) rrect(DX + 40, DY + 150, (DW - 80) * pct / 100 < 14 ? 14 : (DW - 80) * pct / 100, 70, C_LIGHT);
        text_center(DX + DW / 2, DY + 176, T("TOUCH TO SET BRIGHTNESS", "TOCCA PER LA LUMINOSITA"), C_TEXT, 2);
        rrect(DX + 40, DY + 260, 250, 70, C_TILE); text_center(DX + 165, DY + 285, T("OFF", "SPEGNI"), C_TEXT, 3);
        rrect(DX + DW - 290, DY + 260, 250, 70, C_TILE); text_center(DX + DW - 165, DY + 285, T("ON", "ACCENDI"), C_TEXT, 3);
    } else if (e->dom == D_CLIMATE) {
        char t[16];
        if (e->target > -9999) nv_snprintf(t, sizeof t, "%d.%d", e->target / 10, e->target % 10);
        else s_cpy(t, "--", sizeof t);
        text_center(DX + DW / 2, DY + 170, t, C_ORANGE, 8);
        rrect(DX + 40, DY + 260, 200, 80, C_TILE); text_center(DX + 140, DY + 285, "-", C_TEXT, 5);
        rrect(DX + DW - 240, DY + 260, 200, 80, C_TILE); text_center(DX + DW - 140, DY + 285, "+", C_TEXT, 5);
    } else if (e->dom == D_COVER) {
        const char *lab[3] = {T("OPEN", "APRI"), T("STOP", "STOP"), T("CLOSE", "CHIUDI")};
        for (int b = 0; b < 3; b++) {
            rrect(DX + 40 + b * 180, DY + 200, 160, 90, C_TILE);
            text_center(DX + 120 + b * 180, DY + 235, lab[b], C_TEXT, 3);
        }
    } else if (e->dom == D_LOCK) {
        rrect(DX + 40, DY + 200, 250, 90, C_TILE); text_center(DX + 165, DY + 235, T("LOCK", "CHIUDI"), C_TEXT, 3);
        rrect(DX + DW - 290, DY + 200, 250, 90, C_RED); text_center(DX + DW - 165, DY + 235, T("UNLOCK", "APRI"), C_TEXT, 3);
    } else {
        text_center(DX + DW / 2, DY + 200, st, C_TEXT, 5);
    }
}

static int in(int x, int y, int rx, int ry, int rw, int rh) { return x >= rx && x < rx + rw && y >= ry && y < ry + rh; }

static void detail_tap(int x, int y) {
    Ent *e = &g_ent[g_detail];
    if (in(x, y, DX + DW - 70, DY + 16, 54, 44) || !in(x, y, DX, DY, DW, DH)) { g_scr = SCR_GRID; return; }
    char extra[48];
    if (e->dom == D_LIGHT) {
        if (in(x, y, DX + 40, DY + 150, DW - 80, 70)) {
            int pct = (x - DX - 40) * 100 / (DW - 80);
            pct = pct < 1 ? 1 : pct > 100 ? 100 : pct;
            nv_snprintf(extra, sizeof extra, ",\"brightness_pct\":%d", pct);
            call("light", "turn_on", e, extra);
            e->bri = (uint8_t)(pct * 255 / 100);
            s_cpy(e->state, "on", sizeof e->state);
        } else if (in(x, y, DX + 40, DY + 260, 250, 70)) {
            call("light", "turn_off", e, 0); s_cpy(e->state, "off", sizeof e->state);
        } else if (in(x, y, DX + DW - 290, DY + 260, 250, 70)) {
            call("light", "turn_on", e, 0); s_cpy(e->state, "on", sizeof e->state);
        }
    } else if (e->dom == D_CLIMATE && e->target > -9999) {
        int d = in(x, y, DX + 40, DY + 260, 200, 80) ? -5 : in(x, y, DX + DW - 240, DY + 260, 200, 80) ? 5 : 0;
        if (d) {
            e->target = (int16_t)(e->target + d);
            nv_snprintf(extra, sizeof extra, ",\"temperature\":%d.%d", e->target / 10, e->target % 10);
            call("climate", "set_temperature", e, extra);
        }
    } else if (e->dom == D_COVER) {
        static const char *const svc[3] = {"open_cover", "stop_cover", "close_cover"};
        for (int b = 0; b < 3; b++)
            if (in(x, y, DX + 40 + b * 180, DY + 200, 160, 90)) call("cover", svc[b], e, 0);
    } else if (e->dom == D_LOCK) {
        if (in(x, y, DX + 40, DY + 200, 250, 90)) call("lock", "lock", e, 0);
        else if (in(x, y, DX + DW - 290, DY + 200, 250, 90)) call("lock", "unlock", e, 0);
    }
}

static void grid_tap(int x, int y, int hold) {
    if (y < 70) {
        for (int c = 0; c < g_nchips; c++)
            if (x >= g_chip_x[c] && x < g_chip_x[c] + g_chip_w[c]) { g_sel_area = c - 1; g_page = 0; return; }
        const int pages = (count_visible() + COLS * ROWS - 1) / (COLS * ROWS);
        if (pages > 1 && x >= W - 136 && x < W - 80) g_page = (g_page + pages - 1) % pages;
        if (pages > 1 && x >= W - 72) g_page = (g_page + 1) % pages;
        return;
    }
    for (int s = 0; s < COLS * ROWS; s++) {
        const int cx = s % COLS, cy = s / COLS;
        if (!in(x, y, GRID_X + cx * (TW + GAP), GRID_Y + cy * (TH + GAP), TW, TH)) continue;
        const int i = slot_entity(s);
        if (i < 0) return;
        const int d = g_ent[i].dom;
        if (hold || d == D_CLIMATE || d == D_LOCK || d == D_SENSOR || d == D_BINARY) { g_detail = i; g_scr = SCR_DETAIL; }
        else tap_action(&g_ent[i]);
        return;
    }
}

NV_EXPORT("run")
void run(void) {
    nv_lang(g_lang, sizeof g_lang);
    g_it = g_lang[0] == 'i' && g_lang[1] == 't';
    const int configured = nv_ha_available();
    if (configured) poll_start();
    int prev = 0, down_t = 0, down_x = 0, down_y = 0, held = 0;
    while (nv_gfx_present()) {
        const int now = nv_millis();
        if (nv_gfx_back()) {
            if (g_scr == SCR_DETAIL) { g_scr = SCR_GRID; g_redraw = 2; }
            else break;
        }
        if (configured && poll_step(now)) g_redraw = 2;
        int x, y;
        const int down = nv_touch(&x, &y);
        if (down && !prev) { down_t = now; down_x = x; down_y = y; held = 0; }
        if (down && !held && now - down_t > 600 && g_scr == SCR_GRID) {   // long press = details
            held = 1;
            grid_tap(down_x, down_y, 1);
            g_redraw = 2;
        }
        if (!down && prev && !held && configured) {
            if (g_scr == SCR_GRID) grid_tap(down_x, down_y, 0);
            else detail_tap(down_x, down_y);
            g_redraw = 2;
        }
        prev = down;
        if (g_redraw > 0) {
            if (!configured) draw_not_configured();
            else if (g_scr == SCR_DETAIL && g_detail >= 0 && g_detail < g_n) draw_detail();
            else { g_scr = SCR_GRID; draw_grid(); }
            g_redraw--;
        }
    }
    if (g_req >= 0) nv_http_close(g_req);
    if (g_act >= 0) nv_http_close(g_act);
}
