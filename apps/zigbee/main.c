// Zigbee — the Zigbee2MQTT network on the panel (ABI 12, permission "mqtt"; the OS holds the broker
// connection from Settings > Home). Devices appear as they report on zigbee2mqtt/<friendly name>;
// switches and lights toggle with zigbee2mqtt/<name>/set {"state":"TOGGLE"}; sensors show their
// values (temperature, humidity, contact, occupancy, power, battery).
#include "nucleo_sdk.h"

#define W 1024
#define H 600
#define MAX_DEV 48
#define BASE "zigbee2mqtt"

static int s_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void s_cpy(char *d, const char *s, int n) { int i = 0; for (; i < n - 1 && s[i]; i++) d[i] = s[i]; d[i] = 0; }
static int s_eq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int s_starts(const char *s, const char *p) { while (*p) if (*s++ != *p++) return 0; return 1; }

// Find "key": in a flat JSON object and copy its value (string contents or literal) into out.
static int jget(const char *j, int n, const char *key, char *out, int on) {
    const int kl = s_len(key);
    for (int i = 0; i + kl + 3 < n; i++) {
        if (j[i] != '"' || j[i + kl + 1] != '"') continue;
        int k = 0;
        while (k < kl && j[i + 1 + k] == key[k]) k++;
        if (k != kl) continue;
        int p = i + kl + 2;
        while (p < n && (j[p] == ' ' || j[p] == ':')) p++;
        int o = 0;
        if (p < n && j[p] == '"') {
            p++;
            while (p < n && j[p] != '"' && o < on - 1) out[o++] = j[p++];
        } else {
            while (p < n && j[p] != ',' && j[p] != '}' && j[p] != ' ' && o < on - 1) out[o++] = j[p++];
        }
        out[o] = 0;
        return 1;
    }
    return 0;
}

#define C_BG   NV_RGB(14, 17, 22)
#define C_BAR  NV_RGB(22, 27, 34)
#define C_TILE NV_RGB(33, 39, 48)
#define C_LO   NV_RGB(24, 29, 36)
#define C_TEXT NV_RGB(230, 234, 240)
#define C_DIM  NV_RGB(140, 150, 165)
#define C_ON   NV_RGB(120, 94, 16)
#define C_ALERT NV_RGB(120, 30, 30)
#define C_RED  NV_RGB(239, 68, 68)

static void rrect(int x, int y, int w, int h, int col) {
    nv_gfx_rect(x + 6, y, w - 12, h, col);
    nv_gfx_rect(x, y + 6, 6, h - 12, col);
    nv_gfx_rect(x + w - 6, y + 6, 6, h - 12, col);
    nv_gfx_circle(x + 6, y + 6, 6, col); nv_gfx_circle(x + w - 7, y + 6, 6, col);
    nv_gfx_circle(x + 6, y + h - 7, 6, col); nv_gfx_circle(x + w - 7, y + h - 7, 6, col);
}
static void text_center(int cx, int y, const char *s, int col, int sc) { nv_gfx_text(cx - nv_gfx_text_width(s, sc) / 2, y, s, col, sc); }
static void fold(char *d, const char *s, int n) {
    int o = 0;
    for (int i = 0; s[i] && o < n - 1; i++) {
        char c = s[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (c == '_') c = ' ';
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '.' || c == '/')) c = ' ';
        d[o++] = c;
    }
    d[o] = 0;
}

typedef struct {
    char name[48];        // friendly name as in the topic (publishing needs it verbatim)
    char state[8];        // "ON"/"OFF"/"" (switchable when non-empty)
    char line1[24], line2[24];
    uint8_t alert;        // contact open / occupancy / water leak
} Dev;

static Dev  g_dev[MAX_DEV];
static int  g_n = 0;
static char g_topic[128];
static char g_msg[8192];
static int  g_it = 0;
static int  g_page = 0;
#define T(en, it) (g_it ? (it) : (en))

static Dev *dev_for(const char *name) {
    for (int i = 0; i < g_n; i++) if (s_eq(g_dev[i].name, name)) return &g_dev[i];
    if (g_n >= MAX_DEV) return 0;
    Dev *d = &g_dev[g_n++];
    s_cpy(d->name, name, sizeof d->name);
    d->state[0] = d->line1[0] = d->line2[0] = 0;
    d->alert = 0;
    return d;
}

// One device report: pick what a tile shows.
static void on_report(const char *name, int n) {
    Dev *d = dev_for(name);
    if (!d) return;
    char v[24];
    if (jget(g_msg, n, "state", v, sizeof v)) s_cpy(d->state, v, sizeof d->state);
    d->line1[0] = d->line2[0] = 0;
    d->alert = 0;
    if (jget(g_msg, n, "temperature", v, sizeof v)) nv_snprintf(d->line1, sizeof d->line1, "%s C", v);
    if (jget(g_msg, n, "humidity", v, sizeof v)) nv_snprintf(d->line2, sizeof d->line2, "%s %%", v);
    if (jget(g_msg, n, "power", v, sizeof v) && !d->line1[0]) nv_snprintf(d->line1, sizeof d->line1, "%s W", v);
    if (jget(g_msg, n, "contact", v, sizeof v)) {
        const int closed = s_eq(v, "true");
        s_cpy(d->line1, closed ? T("CLOSED", "CHIUSO") : T("OPEN", "APERTO"), sizeof d->line1);
        d->alert = !closed;
    }
    if (jget(g_msg, n, "occupancy", v, sizeof v)) {
        const int occ = s_eq(v, "true");
        s_cpy(d->line1, occ ? T("MOTION", "MOVIMENTO") : T("CLEAR", "LIBERO"), sizeof d->line1);
        d->alert = (uint8_t)occ;
    }
    if (jget(g_msg, n, "water_leak", v, sizeof v) && s_eq(v, "true")) { s_cpy(d->line1, T("LEAK!", "PERDITA!"), sizeof d->line1); d->alert = 1; }
    if (jget(g_msg, n, "battery", v, sizeof v) && !d->line2[0]) nv_snprintf(d->line2, sizeof d->line2, "%s %s%%", T("BATTERY", "BATTERIA"), v);
}

// ---------------------------------------------------------------- UI
#define GX 16
#define GY 84
#define COLS 4
#define ROWS 3
#define TW 237
#define TH 158
#define GAP 12

static int g_status = 0;   // mqtt_sub result

static void draw(void) {
    nv_gfx_clear(C_BG);
    nv_gfx_rect(0, 0, W, 70, C_BAR);
    nv_gfx_text(20, 20, "ZIGBEE", C_TEXT, 4);
    char cnt[40];
    nv_snprintf(cnt, sizeof cnt, "%d %s", g_n, T("DEVICES", "DISPOSITIVI"));
    nv_gfx_text(200, 28, cnt, C_DIM, 2);
    const int pages = (g_n + COLS * ROWS - 1) / (COLS * ROWS);
    if (pages > 1) {
        rrect(W - 136, 15, 56, 40, C_TILE); nv_gfx_text(W - 118, 28, "<", C_TEXT, 2);
        rrect(W - 72, 15, 56, 40, C_TILE);  nv_gfx_text(W - 54, 28, ">", C_TEXT, 2);
    }
    if (g_status < 0) {
        text_center(W / 2, 230, g_status == -9 ? T("MQTT IS NOT SET UP", "MQTT NON CONFIGURATO") : T("MQTT ERROR", "ERRORE MQTT"), C_TEXT, 3);
        text_center(W / 2, 290, T("SETTINGS > HOME: THE BROKER ZIGBEE2MQTT USES", "IMPOSTAZIONI > CASA: IL BROKER USATO DA ZIGBEE2MQTT"), C_DIM, 2);
        return;
    }
    if (!g_n) {
        text_center(W / 2, 250, T("WAITING FOR ZIGBEE2MQTT...", "IN ATTESA DI ZIGBEE2MQTT..."), C_TEXT, 3);
        text_center(W / 2, 300, T("DEVICES SHOW UP WHEN THEY REPORT", "I DISPOSITIVI COMPAIONO QUANDO INVIANO DATI"), C_DIM, 2);
        return;
    }
    for (int s = 0; s < COLS * ROWS; s++) {
        const int i = g_page * COLS * ROWS + s;
        if (i >= g_n) break;
        const Dev *d = &g_dev[i];
        const int x = GX + (s % COLS) * (TW + GAP), y = GY + (s / COLS) * (TH + GAP);
        const int on = s_eq(d->state, "ON");
        rrect(x, y + 4, TW, TH, C_LO);
        rrect(x, y, TW, TH, d->alert ? C_ALERT : on ? C_ON : C_TILE);
        char nm[20];
        fold(nm, d->name, sizeof nm);
        nv_gfx_text(x + 16, y + 18, nm, C_TEXT, 2);
        if (d->state[0]) nv_gfx_text(x + 16, y + 60, on ? T("ON", "ACCESO") : T("OFF", "SPENTO"), on ? C_TEXT : C_DIM, 3);
        if (d->line1[0]) nv_gfx_text(x + 16, d->state[0] ? y + 100 : y + 64, d->line1, C_TEXT, d->state[0] ? 2 : 3);
        if (d->line2[0]) nv_gfx_text(x + 16, y + TH - 30, d->line2, C_DIM, 2);
    }
}

NV_EXPORT("run")
void run(void) {
    char lang[8] = "en";
    nv_lang(lang, sizeof lang);
    g_it = lang[0] == 'i' && lang[1] == 't';
    g_status = nv_mqtt_sub(BASE "/#");
    int redraw = 2, prev = 0, dx = 0, dy = 0, dirty_at = 0;
    while (nv_gfx_present()) {
        const int now = nv_millis();
        if (nv_gfx_back()) break;
        int got = 0, n;
        while (got < 20 && (n = nv_mqtt_recv(g_topic, sizeof g_topic, g_msg, sizeof g_msg - 1)) >= 0) {
            got++;
            if (n > (int)sizeof g_msg - 1) n = sizeof g_msg - 1;
            g_msg[n] = 0;
            if (!s_starts(g_topic, BASE "/")) continue;
            const char *name = g_topic + sizeof BASE;
            // skip the bridge and command/echo topics; availability is zigbee2mqtt/<name>/availability
            if (s_starts(name, "bridge")) continue;
            const int nl = s_len(name);
            if ((nl > 4 && s_eq(name + nl - 4, "/set")) || (nl > 4 && s_eq(name + nl - 4, "/get")) ||
                (nl > 13 && s_eq(name + nl - 13, "/availability"))) continue;
            if (n < 2 || g_msg[0] != '{') continue;
            on_report(name, n);
        }
        if (got && !dirty_at) dirty_at = now + 150;   // coalesce a burst of reports into one redraw
        if (dirty_at && now >= dirty_at) { dirty_at = 0; redraw = 2; }
        int x, y;
        const int down = nv_touch(&x, &y);
        if (down && !prev) { dx = x; dy = y; }
        if (!down && prev) {
            const int pages = (g_n + COLS * ROWS - 1) / (COLS * ROWS);
            if (dy < 70 && pages > 1) {
                if (dx >= W - 136 && dx < W - 80) g_page = (g_page + pages - 1) % pages;
                else if (dx >= W - 72) g_page = (g_page + 1) % pages;
            } else {
                for (int s = 0; s < COLS * ROWS; s++) {
                    const int i = g_page * COLS * ROWS + s;
                    if (i >= g_n) break;
                    const int tx = GX + (s % COLS) * (TW + GAP), ty = GY + (s / COLS) * (TH + GAP);
                    if (dx >= tx && dx < tx + TW && dy >= ty && dy < ty + TH && g_dev[i].state[0]) {
                        char t[128];
                        nv_snprintf(t, sizeof t, BASE "/%s/set", g_dev[i].name);
                        nv_mqtt_pub(t, "{\"state\":\"TOGGLE\"}", 18, 0);
                        s_cpy(g_dev[i].state, s_eq(g_dev[i].state, "ON") ? "OFF" : "ON", sizeof g_dev[i].state);
                        nv_gfx_tone(1200, 15);
                    }
                }
            }
            redraw = 2;
        }
        prev = down;
        if (redraw > 0) { draw(); redraw--; }
    }
}
