// Casa — Home Assistant dashboard for NucleoOS (ABI v12, permission "ha": the OS proxy holds the
// token, the app never sees it). One /api/template request returns every tile as a compact line
// (never the multi-MB /api/states): tiles by room, tap to toggle, brightness bar on lights, -/+ on
// thermostats, scenes and scripts run. Polls every 5 s; an action updates its tile at once.
#include "nucleo_sdk.h"

#define BG      NV_RGB(16, 18, 26)
#define PANEL   NV_RGB(34, 38, 52)
#define PANEL2  NV_RGB(52, 58, 78)
#define ON      NV_RGB(255, 184, 64)
#define ONDK    NV_RGB(150, 104, 30)
#define ACCENT  NV_RGB(84, 162, 255)
#define INK     NV_RGB(240, 242, 248)
#define DIM     NV_RGB(150, 156, 172)
#define DARK    NV_RGB(24, 22, 18)
#define RED     NV_RGB(232, 84, 92)

#define MAXT 72
typedef struct {
    char id[48], state[20], name[40], area[24], unit[12];
    int bri, target10, cur10;   // brightness %, thermostat target x10, current temperature x10
} Tile;

static Tile g_t[MAXT];
static int g_nt, W, H;
static char g_resp[16384];
static char g_areas[12][24];
static int g_na, g_area = -1, g_page;       // -1 = every room
static int g_h = -1, g_act = -1, g_next_poll, g_redraw = 2, g_status;
static char g_msg[96];

// ---- tiny libc (freestanding) -----------------------------------------------------------------
static int s_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int s_eq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void s_cpy(char *d, const char *s, int cap) { int i = 0; while (s[i] && i < cap - 1) { d[i] = s[i]; i++; } d[i] = 0; }
static int dom(const Tile *t, const char *d) { int n = s_len(d); for (int i = 0; i < n; i++) if (t->id[i] != d[i]) return 0; return t->id[n] == '.'; }
static int s_10(const char *s) {            // "21.5" -> 215, not a number -> -9999
    int neg = 0, v = 0, dec = -1, any = 0;
    if (*s == '-') { neg = 1; s++; }
    for (; *s; s++) {
        if (*s >= '0' && *s <= '9') { any = 1; if (dec < 0) v = v * 10 + (*s - '0'); else if (dec == 0) { v = v * 10 + (*s - '0'); dec = 1; } }
        else if (*s == '.' && dec < 0) dec = 0;
        else break;
    }
    if (!any) return -9999;
    if (dec < 1) v *= 10;
    return neg ? -v : v;
}
static void fmt10(char *o, int cap, int v) { nv_snprintf(o, cap, "%s%d.%d", v < 0 ? "-" : "", (v < 0 ? -v : v) / 10, (v < 0 ? -v : v) % 10); }

// ---- Home Assistant ----------------------------------------------------------------------------
static const char kTpl[] =
    "{\"template\":\"{% set ns = namespace(n=0) %}"
    "{% for s in states|selectattr('domain','in',['light','switch','fan','cover','climate','lock','media_player','scene','script','input_boolean','sensor','binary_sensor','vacuum']) %}"
    "{% set dc = s.attributes.device_class|default('', true) %}"
    "{% if s.domain not in ['sensor','binary_sensor'] or dc in ['temperature','humidity','power','door','window','motion','opening','garage_door'] %}"
    "{% set ns.n = ns.n + 1 %}{% if ns.n <= 72 %}"
    "{{ s.entity_id }}|{{ s.state }}|{{ s.name|replace('|',' ') }}|{{ area_name(s.entity_id) or '' }}|"
    "{{ ((s.attributes.brightness or 0)/2.55)|round|int }}|{{ s.attributes.temperature|default('', true) }}|"
    "{{ s.attributes.current_temperature|default('', true) }}|{{ s.attributes.unit_of_measurement|default('', true) }}\\n"
    "{% endif %}{% endif %}{% endfor %}\"}";

static void poll_start(void) {
    if (g_h >= 0) return;
    g_h = nv_ha_req("POST", "/api/template", kTpl, sizeof kTpl - 1);
    g_act = -1;
}

static void call(const char *domain, const char *service, const char *body) {
    char path[96];
    nv_snprintf(path, sizeof path, "/api/services/%s/%s", domain, service);
    int h = nv_ha_req("POST", path, body, s_len(body));
    if (h >= 0) { for (int t = 0; t < 4000 && nv_http_state(h) == 0; t += 20) nv_sleep_ms(20); nv_http_close(h); }
    g_next_poll = nv_millis() + 800;            // confirm from HA shortly
}

static void parse(int n) {
    g_resp[n] = 0;
    g_nt = 0;
    g_na = 0;
    for (char *p = g_resp; *p && g_nt < MAXT; ) {
        char *f[8]; int nf = 0;
        f[nf++] = p;
        while (*p && *p != '\n') { if (*p == '|' && nf < 8) { *p = 0; f[nf++] = p + 1; } p++; }
        if (*p) *p++ = 0;
        if (nf < 8 || !f[0][0]) continue;
        Tile *t = &g_t[g_nt++];
        s_cpy(t->id, f[0], sizeof t->id); s_cpy(t->state, f[1], sizeof t->state); s_cpy(t->name, f[2], sizeof t->name);
        s_cpy(t->area, f[3], sizeof t->area); t->bri = s_10(f[4]) / 10; t->target10 = s_10(f[5]); t->cur10 = s_10(f[6]);
        int u = 0;                                   // the font is ASCII: "°C" -> "C"
        for (const char *q = f[7]; *q && u < (int)sizeof t->unit - 1; q++) if ((unsigned char)*q < 0x80) t->unit[u++] = *q;
        t->unit[u] = 0;
        if (t->area[0]) {
            int k = 0;
            while (k < g_na && !s_eq(g_areas[k], t->area)) k++;
            if (k == g_na && g_na < 12) s_cpy(g_areas[g_na++], t->area, 24);
        }
    }
    if (g_area >= g_na) g_area = -1;
}

static void poll_step(void) {
    if (g_h < 0) { if (nv_millis() >= g_next_poll) poll_start(); return; }
    int st = nv_http_state(g_h);
    if (st == 0) return;
    if (st == 1 && nv_http_status(g_h) == 200) {
        int n = 0, r;
        while (n < (int)sizeof g_resp - 1 && (r = nv_http_read(g_h, g_resp + n, sizeof g_resp - 1 - n)) > 0) n += r;
        parse(n);
        g_status = 1;
    } else {
        g_status = -1;
        nv_snprintf(g_msg, sizeof g_msg, "Home Assistant non risponde (HTTP %d)", st == 1 ? nv_http_status(g_h) : st);
    }
    nv_http_close(g_h);
    g_h = -1;
    g_next_poll = nv_millis() + 5000;
    g_redraw = 2;
}

// ---- view --------------------------------------------------------------------------------------
#define COLS 4
#define ROWS 3
#define TW 238
#define TH 138
#define GX 16
#define GY 136
#define GAP 12

static int visible(int idx[MAXT]) {
    int n = 0;
    for (int i = 0; i < g_nt; i++) if (g_area < 0 || s_eq(g_t[i].area, g_areas[g_area])) idx[n++] = i;
    return n;
}

static int is_on(const Tile *t) {
    return s_eq(t->state, "on") || s_eq(t->state, "open") || s_eq(t->state, "playing") || s_eq(t->state, "unlocked") ||
           s_eq(t->state, "heat") || s_eq(t->state, "cool") || s_eq(t->state, "heat_cool") || s_eq(t->state, "cleaning");
}

static void text_fit(int x, int y, const char *s, int color, int scale, int maxw) {
    char b[44];
    s_cpy(b, s, sizeof b);
    int n = s_len(b);
    while (n > 1 && nv_gfx_text_width(b, scale) > maxw) { b[--n] = 0; if (n > 2) { b[n - 1] = '.'; } }
    nv_gfx_text(x, y, b, color, scale);
}

static void tile(int x, int y, const Tile *t) {
    const int on = is_on(t);
    const int act = !(dom(t, "sensor") || dom(t, "binary_sensor"));
    nv_gfx_rect(x, y, TW, TH, on ? ON : act ? PANEL : PANEL2);
    const int ink = on ? DARK : INK, dim = on ? ONDK : DIM;
    text_fit(x + 14, y + 14, t->name, ink, 2, TW - 28);
    char v[32];
    if (dom(t, "climate")) {
        char a[12] = "--", b[12] = "--";
        if (t->cur10 > -9999) fmt10(a, sizeof a, t->cur10);
        if (t->target10 > -9999) fmt10(b, sizeof b, t->target10);
        nv_snprintf(v, sizeof v, "%s > %s", a, b);
        text_fit(x + 14, y + 50, v, ink, 4, TW - 28);
        nv_gfx_text(x + 14, y + TH - 34, "-", ink, 4);
        nv_gfx_text(x + TW - 34, y + TH - 34, "+", ink, 4);
    } else if (dom(t, "sensor")) {
        nv_snprintf(v, sizeof v, "%s %s", t->state, t->unit);
        text_fit(x + 14, y + 50, v, ink, 4, TW - 28);
    } else if (dom(t, "scene") || dom(t, "script")) {
        text_fit(x + 14, y + 56, "AVVIA", ACCENT, 3, TW - 28);
    } else {
        const char *s = s_eq(t->state, "on") ? "ACCESO" : s_eq(t->state, "off") ? "SPENTO" : s_eq(t->state, "open") ? "APERTO" :
                        s_eq(t->state, "closed") ? "CHIUSO" : s_eq(t->state, "unavailable") ? "NON DISP." : t->state;
        nv_snprintf(v, sizeof v, (dom(t, "light") && on && t->bri > 0) ? "%s %d%%" : "%s", s, t->bri);
        text_fit(x + 14, y + 56, v, ink, 3, TW - 28);
    }
    if (t->area[0] && g_area < 0) text_fit(x + 14, y + TH - 28, t->area, dim, 2, TW - 28);
    if (dom(t, "light") && on) {                 // brightness bar: tap it to set
        nv_gfx_rect(x + 14, y + TH - 10, TW - 28, 6, ONDK);
        nv_gfx_rect(x + 14, y + TH - 10, (TW - 28) * (t->bri > 0 ? t->bri : 100) / 100, 6, DARK);
    }
}

static void draw(void) {
    nv_gfx_clear(BG);
    nv_gfx_text(20, 20, "Casa", INK, 4);
    if (g_status < 0 || !nv_ha_available()) {
        nv_gfx_text_center(260, nv_ha_available() ? g_msg : "Collega Home Assistant in Impostazioni > Casa", DIM, 3);
        return;
    }
    if (g_status == 0) { nv_gfx_text_center(280, "Carico la casa...", DIM, 3); return; }
    // rooms: "Tutto" + each area
    int x = 160;
    for (int a = -1; a < g_na && x < W - 120; a++) {
        const char *lb = a < 0 ? "Tutto" : g_areas[a];
        int w = nv_gfx_text_width(lb, 2) + 28;
        nv_gfx_rect(x, 18, w, 40, a == g_area ? ACCENT : PANEL);
        nv_gfx_text(x + 14, 30, lb, a == g_area ? DARK : INK, 2);
        x += w + 8;
    }
    int idx[MAXT];
    const int n = visible(idx), per = COLS * ROWS, pages = (n + per - 1) / per;
    if (g_page >= pages) g_page = pages > 0 ? pages - 1 : 0;
    for (int k = 0; k < per && g_page * per + k < n; k++)
        tile(GX + (k % COLS) * (TW + GAP), GY - 56 + (k / COLS) * (TH + GAP), &g_t[idx[g_page * per + k]]);
    if (pages > 1) {
        char p[16];
        nv_snprintf(p, sizeof p, "%d/%d", g_page + 1, pages);
        nv_gfx_text(W - 150, H - 36, "<", INK, 3);
        nv_gfx_text(W - 110, H - 32, p, DIM, 2);
        nv_gfx_text(W - 40, H - 36, ">", INK, 3);
    }
    if (!n) nv_gfx_text_center(300, "Nessun dispositivo qui", DIM, 3);
}

static void act(Tile *t, int lx, int ly) {
    char body[160];
    if (dom(t, "sensor") || dom(t, "binary_sensor") || dom(t, "lock")) return;   // locks: from HA, not one tap
    if (dom(t, "scene") || dom(t, "script")) {
        nv_snprintf(body, sizeof body, "{\"entity_id\":\"%s\"}", t->id);
        call(dom(t, "scene") ? "scene" : "script", "turn_on", body);
        return;
    }
    if (dom(t, "climate")) {
        if (t->target10 <= -9999 || (lx > 70 && lx < TW - 70)) return;
        t->target10 += lx <= 70 ? -5 : 5;
        char v[12];
        fmt10(v, sizeof v, t->target10);
        nv_snprintf(body, sizeof body, "{\"entity_id\":\"%s\",\"temperature\":%s}", t->id, v);
        call("climate", "set_temperature", body);
        return;
    }
    if (dom(t, "light") && is_on(t) && ly > TH - 34 && lx > 14 && lx < TW - 14) {   // brightness bar
        int pct = (lx - 14) * 100 / (TW - 28);
        if (pct < 1) pct = 1;
        t->bri = pct;
        nv_snprintf(body, sizeof body, "{\"entity_id\":\"%s\",\"brightness_pct\":%d}", t->id, pct);
        call("light", "turn_on", body);
        return;
    }
    s_cpy(t->state, is_on(t) ? "off" : "on", sizeof t->state);                      // optimistic
    nv_snprintf(body, sizeof body, "{\"entity_id\":\"%s\"}", t->id);
    call("homeassistant", "toggle", body);
}

static void tap(int x, int y) {
    if (y < 64) {                                  // room chips
        int cx = 160;
        for (int a = -1; a < g_na; a++) {
            const char *lb = a < 0 ? "Tutto" : g_areas[a];
            int w = nv_gfx_text_width(lb, 2) + 28;
            if (x >= cx && x < cx + w) { g_area = a; g_page = 0; return; }
            cx += w + 8;
        }
        return;
    }
    if (y > H - 50 && x > W - 160) { if (x < W - 80) { if (g_page > 0) g_page--; } else g_page++; return; }
    int idx[MAXT];
    const int n = visible(idx);
    const int col = (x - GX) / (TW + GAP), row = (y - (GY - 56)) / (TH + GAP);
    const int lx = (x - GX) % (TW + GAP), ly = (y - (GY - 56)) % (TH + GAP);
    if (col < 0 || col >= COLS || row < 0 || row >= ROWS || lx >= TW || ly >= TH) return;
    const int k = g_page * COLS * ROWS + row * COLS + col;
    if (k < n) act(&g_t[idx[k]], lx, ly);
}

NV_EXPORT("run")
void run(void) {
    W = nv_gfx_width();
    H = nv_gfx_height();
    if (nv_ha_available()) poll_start();
    int prev = 0, sx = 0, sy = 0;
    while (nv_gfx_present()) {
        if (nv_gfx_back()) break;
        int x, y, down = nv_touch(&x, &y);
        if (down && !prev) { sx = x; sy = y; }
        if (!down && prev) {
            const int dx = x - sx;
            if (dx > 120 && (y - sy) * (y - sy) < dx * dx) { if (g_page > 0) g_page--; }          // swipe right
            else if (dx < -120 && (y - sy) * (y - sy) < dx * dx) g_page++;                          // swipe left
            else tap(x, y);
            g_redraw = 2;
        }
        prev = down;
        if (nv_ha_available()) poll_step();
        if (g_redraw > 0) { draw(); g_redraw--; }
    }
    if (g_h >= 0) nv_http_close(g_h);
}
