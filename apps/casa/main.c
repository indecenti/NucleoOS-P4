// Casa 1.1 â€” a Home Assistant dashboard for NucleoOS (ABI 15, permission "ha").
//
// Security: the OS proxy holds the token, the app never sees it (nv.ha_req / nv.ha_ws). Every
// service call comes from a fixed per-domain whitelist, entity ids are checked before they go into
// a request, and unlocking a lock or opening a garage, gate or door asks first.
//
// Data: one Jinja template, rendered by Home Assistant, returns every tile as a compact line
// (never the multi-MB /api/states). A WebSocket render_template subscription pushes it again when
// something changes; it iterates per domain, so Home Assistant rate-limits it to once a second. If
// the WebSocket is down the app polls the same template over HTTP every 5 s.
//
// Lettering and icons come from atlases baked by art/gen_assets.py (Montserrat Bold, MDI).
#include "nucleo_sdk.h"
#include "casa_assets.h"

// ---- colours ------------------------------------------------------------------------------------
#define C_BG     NV_RGB(13, 15, 21)
#define C_PANEL  NV_RGB(30, 34, 46)
#define C_PANEL2 NV_RGB(48, 54, 70)
#define C_INK    NV_RGB(240, 242, 248)
#define C_DIM    NV_RGB(150, 157, 174)
#define C_FAINT  NV_RGB(98, 104, 120)
#define C_AMBER  NV_RGB(255, 186, 73)
#define C_BLUE   NV_RGB(96, 170, 255)
#define C_GREEN  NV_RGB(76, 204, 146)
#define C_RED    NV_RGB(240, 98, 98)
#define C_ORANGE NV_RGB(255, 142, 70)
#define C_CYAN   NV_RGB(70, 200, 222)
#define C_FOCUS  NV_RGB(130, 190, 255)
#define C_BLACK  NV_RGB(0, 0, 0)

// ---- strings (en, it, es, fr, de) ---------------------------------------------------------------
enum {
    S_TITLE, S_ALL, S_ON, S_OFF, S_OPEN, S_CLOSED, S_OPENING, S_CLOSING, S_LOCKED, S_UNLOCKED, S_JAMMED,
    S_PLAYING, S_PAUSED, S_IDLE, S_HEAT, S_COOL, S_AUTO, S_CLEANING, S_DOCKED, S_RETURNING, S_UNAVAIL,
    S_RUN, S_DETECTED, S_CLEAR, S_WET, S_DRY, S_LIVE, S_POLL, S_OFFLINE, S_LOADING, S_NOCONF1, S_NOCONF2,
    S_UNREACH, S_AUTH, S_EMPTY, S_CANCEL, S_UNLOCK, S_OPEN_V, S_Q_UNLOCK, S_Q_OPEN, S_Q_NOTE, S_FAILED,
    S_N
};
static const char *const k_str[5][S_N] = {
    { "Home", "All", "On", "Off", "Open", "Closed", "Opening\xE2\x80\xA6", "Closing\xE2\x80\xA6", "Locked", "Unlocked",
      "Jammed", "Playing", "Paused", "Idle", "Heating", "Cooling", "Auto", "Cleaning", "Docked", "Returning",
      "Unavailable", "Run", "Detected", "Clear", "Wet", "Dry", "Live", "Every 5 s", "Offline",
      "Loading your home\xE2\x80\xA6", "Home Assistant is not set up",
      "Add its address and a token in Settings > Home", "Home Assistant does not answer",
      "The token was refused: create a new one in Home Assistant", "Nothing here", "Cancel", "Unlock", "Open",
      "Unlock %s?", "Open %s?", "Anyone nearby will be able to get in.", "Home Assistant refused the command" },
    { "Casa", "Tutto", "Acceso", "Spento", "Aperto", "Chiuso", "Apertura\xE2\x80\xA6", "Chiusura\xE2\x80\xA6", "Bloccata",
      "Sbloccata", "Inceppata", "In riproduzione", "In pausa", "Inattivo", "Riscalda", "Raffresca", "Auto",
      "Pulisce", "In base", "Rientra", "Non disponibile", "Avvia", "Rilevato", "Nessuno", "Bagnato", "Asciutto",
      "In diretta", "Ogni 5 s", "Non raggiungibile", "Carico la casa\xE2\x80\xA6", "Home Assistant non \xC3\xA8 configurato",
      "Inserisci indirizzo e token in Impostazioni > Casa", "Home Assistant non risponde",
      "Il token \xC3\xA8 stato rifiutato: creane uno nuovo in Home Assistant", "Niente qui", "Annulla", "Sblocca",
      "Apri", "Sbloccare %s?", "Aprire %s?", "Chiunque sia vicino potr\xC3\xA0 entrare.",
      "Home Assistant ha rifiutato il comando" },
    { "Casa", "Todo", "Encendido", "Apagado", "Abierto", "Cerrado", "Abriendo\xE2\x80\xA6", "Cerrando\xE2\x80\xA6",
      "Bloqueada", "Desbloqueada", "Atascada", "Reproduciendo", "En pausa", "Inactivo", "Calentando", "Enfriando",
      "Auto", "Limpiando", "En base", "Volviendo", "No disponible", "Activar", "Detectado", "Despejado", "Mojado",
      "Seco", "En directo", "Cada 5 s", "Sin conexi\xC3\xB3n", "Cargando tu casa\xE2\x80\xA6",
      "Home Assistant no est\xC3\xA1 configurado", "A\xC3\xB1" "ade direcci\xC3\xB3n y token en Ajustes > Home",
      "Home Assistant no responde", "El token fue rechazado: crea uno nuevo en Home Assistant", "Nada aqu\xC3\xAD",
      "Cancelar", "Desbloquear", "Abrir", "\xC2\xBF" "Desbloquear %s?", "\xC2\xBF" "Abrir %s?",
      "Cualquiera cerca podr\xC3\xA1 entrar.", "Home Assistant rechaz\xC3\xB3 la orden" },
    { "Maison", "Tout", "Allum\xC3\xA9", "\xC3\x89teint", "Ouvert", "Ferm\xC3\xA9", "Ouverture\xE2\x80\xA6",
      "Fermeture\xE2\x80\xA6", "Verrouill\xC3\xA9" "e", "D\xC3\xA9verrouill\xC3\xA9" "e", "Bloqu\xC3\xA9" "e", "Lecture", "En pause",
      "Inactif", "Chauffe", "Refroidit", "Auto", "Nettoie", "Sur la base", "Retour", "Indisponible", "Lancer",
      "D\xC3\xA9tect\xC3\xA9", "Rien", "Humide", "Sec", "En direct", "Toutes les 5 s", "Hors ligne",
      "Chargement de la maison\xE2\x80\xA6", "Home Assistant n\xE2\x80\x99" "est pas configur\xC3\xA9",
      "Ajoutez l\xE2\x80\x99" "adresse et un jeton dans R\xC3\xA9glages > Home", "Home Assistant ne r\xC3\xA9pond pas",
      "Le jeton a \xC3\xA9t\xC3\xA9 refus\xC3\xA9 : cr\xC3\xA9" "ez-en un nouveau dans Home Assistant", "Rien ici", "Annuler",
      "D\xC3\xA9verrouiller", "Ouvrir", "D\xC3\xA9verrouiller %s ?", "Ouvrir %s ?",
      "Toute personne \xC3\xA0 proximit\xC3\xA9 pourra entrer.", "Home Assistant a refus\xC3\xA9 la commande" },
    { "Zuhause", "Alle", "An", "Aus", "Offen", "Geschlossen", "\xC3\x96" "ffnet\xE2\x80\xA6", "Schlie\xC3\x9Ft\xE2\x80\xA6",
      "Verriegelt", "Entriegelt", "Blockiert", "Spielt", "Pausiert", "Bereit", "Heizt", "K\xC3\xBChlt", "Auto",
      "Reinigt", "In der Station", "Kehrt zur\xC3\xBC" "ck", "Nicht verf\xC3\xBC" "gbar", "Starten", "Erkannt", "Frei", "Nass",
      "Trocken", "Live", "Alle 5 s", "Offline", "Lade dein Zuhause\xE2\x80\xA6", "Home Assistant ist nicht eingerichtet",
      "Adresse und Token in Einstellungen > Home eintragen", "Home Assistant antwortet nicht",
      "Das Token wurde abgelehnt: erstelle in Home Assistant ein neues", "Hier ist nichts", "Abbrechen",
      "Entriegeln", "\xC3\x96" "ffnen", "%s entriegeln?", "%s \xC3\xB6" "ffnen?", "Dann kann jeder in der N\xC3\xA4he hinein.",
      "Home Assistant hat den Befehl abgelehnt" },
};
static int g_lang;
static const char *T(int s) { return k_str[g_lang][s]; }

// ---- tiny libc (freestanding) -------------------------------------------------------------------
static int s_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int s_eq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void s_cpy(char *d, const char *s, int cap) { int i = 0; while (s[i] && i < cap - 1) { d[i] = s[i]; i++; } d[i] = 0; }
static int s_cmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }
static int s_in(const char *s, const char *const *list) { for (; *list; list++) if (s_eq(s, *list)) return 1; return 0; }

#define NA (-32000)
// "21.5" -> 215, anything that is not a number -> NA
static int s_10(const char *s) {
    int neg = 0, v = 0, dec = -1, any = 0;
    if (*s == '-') { neg = 1; s++; }
    for (; *s; s++) {
        if (*s >= '0' && *s <= '9') {
            any = 1;
            if (dec < 0) { if (v < 300000) v = v * 10 + (*s - '0'); }
            else if (dec == 0) { v = v * 10 + (*s - '0'); dec = 1; }
        } else if (*s == '.' && dec < 0) dec = 0;
        else return NA;
    }
    if (!any) return NA;
    if (dec < 1) v *= 10;
    if (v > 30000) v = 30000;
    return neg ? -v : v;
}
// x10 value -> "21.5" (or "21,5"); whole numbers lose the ".0"
static void fmt10(char *o, int cap, int v) {
    const char sep = g_lang == 0 ? '.' : ',';
    const int a = v < 0 ? -v : v;
    if (a % 10) nv_snprintf(o, cap, "%s%d%c%d", v < 0 ? "-" : "", a / 10, sep, a % 10);
    else nv_snprintf(o, cap, "%s%d", v < 0 ? "-" : "", a / 10);
}

// ---- lettering ----------------------------------------------------------------------------------
enum { F_S, F_M, F_L };
static const Glyph *const k_font[3] = { k_fs, k_fm, k_fl };
static const char *const k_atlas[3][2] = { { "fsl", "fsd" }, { "fml", "fmd" }, { "fll", "fld" } };

// glyph index of the next UTF-8 character of *s, advancing *s
static int glyph_at(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    int c = *p++;
    if (c >= 0xC0 && c < 0xE0 && (*p & 0xC0) == 0x80) c = ((c & 31) << 6) | (*p++ & 63);
    else if (c >= 0xE0 && c < 0xF0 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80) {
        c = ((c & 15) << 12) | ((p[0] & 63) << 6) | (p[1] & 63);
        p += 2;
    } else if (c >= 0x80) { while ((*p & 0xC0) == 0x80) p++; c = '?'; }
    *s = (const char *)p;
    if (c >= 32 && c < 127) return c - 32;
    if (c >= 0xA0 && c <= 0xFF) return 95 + (c - 0xA0);
    for (int i = 0; i < (int)(sizeof k_extra / sizeof k_extra[0]); i++) if (k_extra[i] == c) return GLYPH_EXTRA + i;
    return '?' - 32;
}
static int text_w(const char *s, int f) {
    int pen = 0;
    while (*s) pen += k_font[f][glyph_at(&s)].adv;
    return (pen + 3) / 4;
}
// fam 0: light text on a dark panel, tint = text colour; fam 1: dark text, tint = the panel colour
static int text(int x, int y, const char *s, int f, int tint, int fam) {
    const Glyph *G = k_font[f];
    int pen = 0;
    while (*s) {
        const int gi = glyph_at(&s);
        const Glyph *g = &G[gi];
        if (g->w > 1) nv_gfx_sprite(k_atlas[f][fam], g->x, g->y, g->w, g->h, x + pen / 4 + g->ox, y + g->oy, g->w, g->h, tint);
        pen += g->adv;
    }
    return (pen + 3) / 4;
}
// s shortened with an ellipsis until it fits maxw
static void fit(char *o, int cap, const char *s, int f, int maxw) {
    s_cpy(o, s, cap);
    if (text_w(o, f) <= maxw) return;
    int n = s_len(o);
    while (n > 0) {
        do n--; while (n > 0 && ((unsigned char)o[n] & 0xC0) == 0x80);   // back one UTF-8 character
        if (n + 4 > cap) continue;
        o[n] = 0;
        char t[96];
        nv_snprintf(t, sizeof t, "%s\xE2\x80\xA6", o);
        if (text_w(t, f) <= maxw || n == 0) { s_cpy(o, t, cap); return; }
    }
}
static void text_fit(int x, int y, const char *s, int f, int tint, int fam, int maxw) {
    char b[96];
    fit(b, sizeof b, s, f, maxw);
    text(x, y, b, f, tint, fam);
}
static void text_right(int xr, int y, const char *s, int f, int tint, int fam) { text(xr - text_w(s, f), y, s, f, tint, fam); }
static void icon(int ic, int x, int y, int tint, int fam) {
    nv_gfx_sprite(fam ? "icd" : "icl", (ic % ICON_COLS) * (ICON_PX + 1), (ic / ICON_COLS) * (ICON_PX + 1), ICON_PX, ICON_PX,
                  x, y, ICON_PX, ICON_PX, tint);
}
// colour c with its channels at pct %
static int shade(int c, int pct) {
    int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    r = r * pct / 100; g = g * pct / 100; b = b * pct / 100;
    return (r << 11) | (g << 5) | b;
}

// ---- tiles --------------------------------------------------------------------------------------
enum { D_LIGHT, D_SWITCH, D_BOOL, D_FAN, D_COVER, D_CLIMATE, D_LOCK, D_MEDIA, D_VACUUM, D_SCENE, D_SCRIPT,
       D_SENSOR, D_BINARY, D_N };
static const char *const k_dom[D_N] = { "light", "switch", "input_boolean", "fan", "cover", "climate", "lock",
                                        "media_player", "vacuum", "scene", "script", "sensor", "binary_sensor" };
#define MAXT 72
typedef struct {
    char id[48], state[16], name[36], area[24], unit[8], dc[16];
    short bri, t10, cur10, lo10, hi10;    // brightness %, thermostat target / current / range, x10
    unsigned char dom;
} Tile;
static Tile g_t[MAXT];
static int g_nt;
static char g_areas[12][24];
static int g_na, g_area = -1;               // rooms; the one shown, -1 = all

// The template. Per-domain loops (states[d]) let Home Assistant rate-limit the live render to once
// a second instead of once a minute. Fields: id|state|name|area|brightness%|target|current|unit|
// device_class|low|high. '|' in names is replaced so a name can't shift the fields; a name that starts
// with its device's name loses it, as in Home Assistant's own cards ("NucleoOS eac253 Mute" -> "Mute").
#define TPL \
    "{% set ns = namespace(n=0) %}" \
    "{% for d in ['light','switch','input_boolean','fan','cover','climate','lock','media_player','vacuum','scene','script','sensor','binary_sensor'] %}" \
    "{% for s in states[d] %}" \
    "{% set a = s.attributes %}{% set dc = a.get('device_class') or '' %}" \
    "{% if ns.n < 72 and (d != 'sensor' or dc in ['temperature','humidity','power','illuminance','carbon_dioxide','pm25']) " \
    "and (d != 'binary_sensor' or dc in ['door','window','garage_door','opening','motion','occupancy','presence','moisture','smoke','gas','carbon_monoxide']) %}" \
    "{% set ns.n = ns.n + 1 %}" \
    "{% set dn = device_attr(s.entity_id, 'name_by_user') or device_attr(s.entity_id, 'name') or '' %}" \
    "{% set nm = s.name[dn|length + 1:] if dn and s.name.startswith(dn ~ ' ') and s.name|length > dn|length + 1 else s.name %}" \
    "{{ s.entity_id }}|{{ s.state }}|{{ nm|replace('|',' ') }}|{{ (area_name(s.entity_id) or '')|replace('|',' ') }}|" \
    "{{ ((a.get('brightness') or 0)/2.55)|round|int }}|{{ a.get('temperature', '') }}|" \
    "{{ a.get('current_temperature', '') }}|{{ a.get('unit_of_measurement') or '' }}|{{ dc }}|" \
    "{{ a.get('target_temp_low', '') }}|{{ a.get('target_temp_high', '') }}\\n" \
    "{% endif %}{% endfor %}{% endfor %}"
static const char kHttpBody[] = "{\"template\":\"" TPL "\"}";
static const char kWsSub[] = "{\"id\":1,\"type\":\"render_template\",\"template\":\"" TPL "\"}";

// an entity id as Home Assistant makes them: domain.object_id, [a-z0-9_] only
static int id_ok(const char *s) {
    int dots = 0, n = 0;
    for (; s[n]; n++) {
        const char c = s[n];
        if (c == '.') { if (n == 0 || dots++) return 0; }
        else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return 0;
    }
    return dots == 1 && n > 2 && s[n - 1] != '.';
}

// optimistic states, kept over refreshes until Home Assistant confirms (or 4 s pass)
typedef struct { char id[48], state[16]; short bri, t10, lo10, hi10; int until; } Pend;
static Pend g_pend[4];

static void parse(char *p) {
    g_nt = 0;
    g_na = 0;
    while (*p && g_nt < MAXT) {
        char *f[11];
        int nf = 0;
        f[nf++] = p;
        while (*p && *p != '\n') { if (*p == '|' && nf < 11) { *p = 0; f[nf++] = p + 1; } p++; }
        if (*p) *p++ = 0;
        if (nf < 11 || !id_ok(f[0])) continue;
        int d = 0;
        while (d < D_N) {
            const char *a = k_dom[d], *b = f[0];
            while (*a && *a == *b) { a++; b++; }
            if (!*a && *b == '.') break;
            d++;
        }
        if (d == D_N) continue;
        Tile *t = &g_t[g_nt++];
        t->dom = (unsigned char)d;
        s_cpy(t->id, f[0], sizeof t->id);
        s_cpy(t->state, f[1], sizeof t->state);
        s_cpy(t->name, f[2], sizeof t->name);
        s_cpy(t->area, f[3], sizeof t->area);
        const int b10 = s_10(f[4]);
        t->bri = (short)(b10 == NA ? 0 : b10 / 10);
        t->t10 = (short)s_10(f[5]);
        t->cur10 = (short)s_10(f[6]);
        s_cpy(t->unit, f[7], sizeof t->unit);
        s_cpy(t->dc, f[8], sizeof t->dc);
        t->lo10 = (short)s_10(f[9]);
        t->hi10 = (short)s_10(f[10]);
        if (t->area[0]) {
            int k = 0;
            while (k < g_na && !s_eq(g_areas[k], t->area)) k++;
            if (k == g_na && g_na < 12) s_cpy(g_areas[g_na++], t->area, sizeof g_areas[0]);
        }
    }
    for (int i = 1; i < g_na; i++)                 // rooms in alphabetical order
        for (int j = i; j > 0 && s_cmp(g_areas[j - 1], g_areas[j]) > 0; j--) {
            char tmp[24];
            s_cpy(tmp, g_areas[j], 24); s_cpy(g_areas[j], g_areas[j - 1], 24); s_cpy(g_areas[j - 1], tmp, 24);
        }
    const int now = nv_millis();
    for (int k = 0; k < 4; k++) {
        Pend *q = &g_pend[k];
        if (!q->id[0]) continue;
        if (now - q->until > 0) { q->id[0] = 0; continue; }
        for (int i = 0; i < g_nt; i++) if (s_eq(g_t[i].id, q->id)) {
            if (s_eq(g_t[i].state, q->state) && g_t[i].bri == q->bri && g_t[i].t10 == q->t10) { q->id[0] = 0; break; }
            s_cpy(g_t[i].state, q->state, sizeof g_t[i].state);
            g_t[i].bri = q->bri; g_t[i].t10 = q->t10; g_t[i].lo10 = q->lo10; g_t[i].hi10 = q->hi10;
        }
    }
}

// ---- connection ---------------------------------------------------------------------------------
enum { ST_LOADING, ST_OK, ST_NOCONF, ST_UNREACH, ST_AUTH };
static int g_status = ST_LOADING, g_err, g_auth_bad;   // g_auth_bad: the WebSocket said auth_invalid
static char g_buf[14336];                     // template text / WebSocket message
static unsigned g_sum;                        // checksum of the last rendered text
static int g_poll_h = -1, g_next_poll;
static int g_ws = -1, g_ws_sub, g_live, g_ws_retry;
static int g_redraw = 2, g_force_poll;

static unsigned checksum(const char *s) { unsigned h = 2166136261u; while (*s) h = (h ^ (unsigned char)*s++) * 16777619u; return h; }

static void got_text(void) {
    const unsigned h = checksum(g_buf);
    if (g_status != ST_OK) g_redraw = 2;
    g_status = ST_OK;
    g_auth_bad = 0;
    if (h == g_sum) return;
    g_sum = h;
    parse(g_buf);
    g_redraw = 2;
}

static void poll_step(void) {
    const int now = nv_millis();
    if (g_poll_h < 0) {
        if ((g_live && !g_force_poll) || now - g_next_poll < 0) return;
        g_force_poll = 0;
        g_poll_h = nv_ha_req("POST", "/api/template", kHttpBody, sizeof kHttpBody - 1);
        if (g_poll_h < 0) { g_status = g_poll_h == NV_NET_E_NOTCONF ? ST_NOCONF : ST_UNREACH; g_err = g_poll_h; g_next_poll = now + 5000; g_redraw = 2; }
        return;
    }
    const int st = nv_http_state(g_poll_h);
    if (st == 0) return;
    const int code = st == 1 ? nv_http_status(g_poll_h) : st;
    if (st == 1 && code == 200) {
        int n = 0, r;
        while (n < (int)sizeof g_buf - 1 && (r = nv_http_read(g_poll_h, g_buf + n, sizeof g_buf - 1 - n)) > 0) n += r;
        g_buf[n] = 0;
        got_text();
    } else {
        const int old = g_status;
        // older firmware reports a 401 as a connect error (-5): the WebSocket's verdict wins
        g_status = code == 401 || g_auth_bad ? ST_AUTH : code == NV_NET_E_NOTCONF ? ST_NOCONF : ST_UNREACH;
        g_err = code;
        if (old != g_status) g_redraw = 2;
    }
    nv_http_close(g_poll_h);
    g_poll_h = -1;
    g_next_poll = now + 5000;
}

// the string value of "key" in a JSON message, unescaped in place; returns it or 0
static char *json_str(char *m, const char *key) {
    char pat[24];
    nv_snprintf(pat, sizeof pat, "\"%s\":\"", key);
    char *p = m;
    const int pl = s_len(pat);
    for (; *p; p++) {
        int i = 0;
        while (i < pl && p[i] == pat[i]) i++;
        if (i == pl) break;
    }
    if (!*p) return 0;
    char *in = p + pl, *out = in, *start = in;
    while (*in && *in != '"') {
        if (*in != '\\') { *out++ = *in++; continue; }
        in++;
        const char e = *in ? *in++ : 0;
        if (e == 'n') *out++ = '\n';
        else if (e == 't') *out++ = ' ';
        else if (e == 'u') {
            int c = 0;
            for (int k = 0; k < 4 && *in; k++, in++) {
                const char h = *in;
                c = c * 16 + (h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : 0);
            }
            if (c >= 0xD800 && c < 0xE000) c = '?';
            if (c < 0x80) *out++ = (char)(c ? c : ' ');
            else if (c < 0x800) { *out++ = (char)(0xC0 | (c >> 6)); *out++ = (char)(0x80 | (c & 63)); }
            else { *out++ = (char)(0xE0 | (c >> 12)); *out++ = (char)(0x80 | ((c >> 6) & 63)); *out++ = (char)(0x80 | (c & 63)); }
        } else if (e) *out++ = e;                  // \" \\ \/
    }
    *out = 0;
    return start;
}

static int has(const char *m, const char *pat) {
    const int pl = s_len(pat);
    for (; *m; m++) {
        int i = 0;
        while (i < pl && m[i] == pat[i]) i++;
        if (i == pl) return 1;
    }
    return 0;
}

static void ws_drop(void) {
    if (g_ws >= 0) nv_ws_close(g_ws);
    g_ws = -1;
    g_ws_sub = 0;
    if (g_live) { g_live = 0; g_redraw = 2; }
    g_ws_retry = nv_millis() + 15000;
    g_next_poll = nv_millis();
}

static void ws_step(void) {
    const int now = nv_millis();
    if (g_ws < 0) {
        // only after the first HTTP answer: with Home Assistant unreachable, a WebSocket connect
        // queued in front of the poll would hold the "not reachable" message back for 8 s more
        if (g_status == ST_LOADING || g_status == ST_NOCONF || g_status == ST_AUTH || now - g_ws_retry < 0) return;
        g_ws = nv_ha_ws();
        if (g_ws < 0) { g_ws_retry = now + (g_ws == NV_NET_E_BUSY ? 2000 : 15000); g_ws = -1; return; }
        g_ws_sub = 0;
        return;
    }
    const int st = nv_ws_state(g_ws);
    if (st < 0) { ws_drop(); return; }
    if (st == 0) return;
    for (int k = 0; k < 4; k++) {
        const int n = nv_ws_recv(g_ws, g_buf, sizeof g_buf - 1);
        if (n < 0) { ws_drop(); return; }
        if (n == 0) return;
        if (n >= (int)sizeof g_buf - 1) { ws_drop(); g_ws_retry = now + 600000; return; }   // too big: HTTP only
        g_buf[n] = 0;
        if (has(g_buf, "\"auth_invalid\"")) {      // the token in Settings > Home was refused
            ws_drop();
            g_auth_bad = 1;
            g_status = ST_AUTH;
            g_redraw = 2;
            return;
        }
        if (!g_ws_sub) {                           // subscribe once the OS has authenticated
            if (has(g_buf, "\"auth_ok\"") && nv_ws_send(g_ws, kWsSub, sizeof kWsSub - 1, 0) >= 0) g_ws_sub = 1;
            continue;
        }
        if (has(g_buf, "\"success\":false")) { ws_drop(); g_ws_retry = now + 600000; return; }
        if (!has(g_buf, "\"type\":\"event\"")) continue;           // the subscription's result
        char *res = json_str(g_buf, "result");
        if (!res) { ws_drop(); g_ws_retry = now + 600000; return; }  // a template error: HTTP only
        char *d = g_buf;
        while ((*d++ = *res++) != 0) {}
        if (!g_live) { g_live = 1; g_redraw = 2; }
        got_text();
    }
}

// ---- actions ------------------------------------------------------------------------------------
static int g_act[4] = { -1, -1, -1, -1 };
static int g_toast_until, g_flash_until, g_flash_tile = -1;
static const char *g_toast;
static char g_last_id[48];
static int g_last_t;

static void toast(int s) { g_toast = T(s); g_toast_until = nv_millis() + 3000; g_redraw = 2; }

static void pend_set(const Tile *t) {
    int k = 0;
    while (k < 4 && g_pend[k].id[0] && !s_eq(g_pend[k].id, t->id)) k++;
    if (k == 4) k = 0;
    Pend *q = &g_pend[k];
    s_cpy(q->id, t->id, sizeof q->id);
    s_cpy(q->state, t->state, sizeof q->state);
    q->bri = t->bri; q->t10 = t->t10; q->lo10 = t->lo10; q->hi10 = t->hi10;
    q->until = nv_millis() + 4000;
}

// x10 -> "21.5" for JSON (always a dot)
static void fmtj(char *o, int cap, int v) {
    const int a = v < 0 ? -v : v;
    nv_snprintf(o, cap, "%s%d.%d", v < 0 ? "-" : "", a / 10, a % 10);
}

// POST /api/services/<the tile's domain>/<svc>. svc is always a literal from this file.
static int call(const Tile *t, const char *svc, const char *extra) {
    if (!id_ok(t->id)) return 0;
    int slot = 0;
    while (slot < 4 && g_act[slot] >= 0) slot++;
    if (slot == 4) return 0;                        // four commands in flight already
    char path[64], body[192];
    nv_snprintf(path, sizeof path, "/api/services/%s/%s", k_dom[t->dom], svc);
    nv_snprintf(body, sizeof body, "{\"entity_id\":\"%s\"%s}", t->id, extra ? extra : "");
    const int h = nv_ha_req("POST", path, body, s_len(body));
    if (h < 0) { toast(S_FAILED); return 0; }
    g_act[slot] = h;
    return 1;
}

static void act_step(void) {
    for (int k = 0; k < 4; k++) {
        if (g_act[k] < 0) continue;
        const int st = nv_http_state(g_act[k]);
        if (st == 0) continue;
        const int code = st == 1 ? nv_http_status(g_act[k]) : st;
        nv_http_close(g_act[k]);
        g_act[k] = -1;
        if (code < 200 || code >= 300) {
            toast(S_FAILED);
            for (int i = 0; i < 4; i++) g_pend[i].id[0] = 0;
            g_sum = 0;
            g_force_poll = 1;
            g_next_poll = nv_millis();
        }
        if (!g_live) g_next_poll = nv_millis() + 600;
    }
}

static int is_unavail(const Tile *t) { return s_eq(t->state, "unavailable") || s_eq(t->state, "unknown"); }
static int on_state(const char *s) {
    static const char *const on[] = { "on", "open", "opening", "playing", "unlocked", "cleaning", "returning", 0 };
    return s_in(s, on);
}

static void adjust(Tile *t, int dir) {
    char extra[96], a[12], b[12];
    if (t->dom == D_LIGHT) {
        int p = (on_state(t->state) && t->bri > 0 ? t->bri : 0) + dir * 10;
        if (p < 1) p = 1;
        if (p > 100) p = 100;
        t->bri = (short)p;
        s_cpy(t->state, "on", sizeof t->state);
        nv_snprintf(extra, sizeof extra, ",\"brightness_pct\":%d", p);
        if (call(t, "turn_on", extra)) pend_set(t);
    } else if (t->dom == D_CLIMATE && !s_eq(t->state, "off")) {
        if (t->t10 != NA) {
            t->t10 = (short)(t->t10 + dir * 5);
            fmtj(a, sizeof a, t->t10);
            nv_snprintf(extra, sizeof extra, ",\"temperature\":%s", a);
        } else if (t->lo10 != NA && t->hi10 != NA) {
            t->lo10 = (short)(t->lo10 + dir * 5);
            t->hi10 = (short)(t->hi10 + dir * 5);
            fmtj(a, sizeof a, t->lo10);
            fmtj(b, sizeof b, t->hi10);
            nv_snprintf(extra, sizeof extra, ",\"target_temp_low\":%s,\"target_temp_high\":%s", a, b);
        } else return;
        if (call(t, "set_temperature", extra)) pend_set(t);
    } else return;
    g_redraw = 2;
}

// confirmation sheet: unlocking, or opening a garage / gate / door
static int g_sheet = -1, g_sheet_open;      // tile index; 1 = open a cover, 0 = unlock

static int gate_like(const Tile *t) {
    static const char *const dcs[] = { "garage", "gate", "door", 0 };
    return s_in(t->dc, dcs);
}

#define TW 236
#define TH 128

static void do_tile(int i, int lx, int ly) {
    Tile *t = &g_t[i];
    if (is_unavail(t)) return;
    const int now = nv_millis();
    if (lx < 0 && s_eq(g_last_id, t->id) && now - g_last_t < 300) return;   // key bounce
    s_cpy(g_last_id, t->id, sizeof g_last_id);
    g_last_t = now;
    const int on = on_state(t->state);
    switch (t->dom) {
    case D_LIGHT:
        if (on && ly >= TH - 30 && lx >= 12 && lx < TW - 12) {        // the brightness bar
            int p = (lx - 16) * 100 / (TW - 32);
            if (p < 1) p = 1;
            if (p > 100) p = 100;
            t->bri = (short)p;
            char extra[40];
            nv_snprintf(extra, sizeof extra, ",\"brightness_pct\":%d", p);
            if (call(t, "turn_on", extra)) pend_set(t);
            break;
        }
        /* fall through */
    case D_SWITCH: case D_BOOL: case D_FAN:
        s_cpy(t->state, on ? "off" : "on", sizeof t->state);
        if (call(t, "toggle", 0)) pend_set(t);
        break;
    case D_COVER:
        if (s_eq(t->state, "opening") || s_eq(t->state, "closing")) call(t, "stop_cover", 0);
        else if (s_eq(t->state, "open")) { s_cpy(t->state, "closing", sizeof t->state); if (call(t, "close_cover", 0)) pend_set(t); }
        else if (gate_like(t)) { g_sheet = i; g_sheet_open = 1; }
        else { s_cpy(t->state, "opening", sizeof t->state); if (call(t, "open_cover", 0)) pend_set(t); }
        break;
    case D_CLIMATE:
        if (ly >= 84 && lx >= TW - 108) adjust(t, lx >= TW - 58 ? 1 : -1);   // the - and + buttons
        break;
    case D_LOCK:
        if (s_eq(t->state, "locked")) { g_sheet = i; g_sheet_open = 0; }
        else if (s_eq(t->state, "unlocked") || s_eq(t->state, "open")) {
            s_cpy(t->state, "locked", sizeof t->state);
            if (call(t, "lock", 0)) pend_set(t);
        }
        break;
    case D_MEDIA:
        if (s_eq(t->state, "off") || s_eq(t->state, "standby")) call(t, "turn_on", 0);
        else {
            s_cpy(t->state, s_eq(t->state, "playing") ? "paused" : "playing", sizeof t->state);
            if (call(t, "media_play_pause", 0)) pend_set(t);
        }
        break;
    case D_VACUUM:
        if (s_eq(t->state, "cleaning")) { s_cpy(t->state, "returning", sizeof t->state); if (call(t, "return_to_base", 0)) pend_set(t); }
        else { s_cpy(t->state, "cleaning", sizeof t->state); if (call(t, "start", 0)) pend_set(t); }
        break;
    case D_SCENE: case D_SCRIPT:
        if (call(t, "turn_on", 0)) { g_flash_tile = i; g_flash_until = now + 600; }
        break;
    default:
        break;                                       // sensors: nothing to do
    }
    g_redraw = 2;
}

static void sheet_confirm(void) {
    if (g_sheet < 0 || g_sheet >= g_nt) { g_sheet = -1; return; }
    Tile *t = &g_t[g_sheet];
    if (g_sheet_open && t->dom == D_COVER) { s_cpy(t->state, "opening", sizeof t->state); if (call(t, "open_cover", 0)) pend_set(t); }
    else if (!g_sheet_open && t->dom == D_LOCK) call(t, "unlock", 0);   // no optimistic state: wait for the lock
    g_sheet = -1;
    g_redraw = 2;
}

// ---- look ---------------------------------------------------------------------------------------
static int tile_colour(int i) {
    const Tile *t = &g_t[i];
    if (i == g_flash_tile && nv_millis() - g_flash_until < 0) return C_AMBER;
    if (is_unavail(t)) return C_PANEL;
    const char *s = t->state;
    switch (t->dom) {
    case D_LIGHT: case D_SWITCH: case D_BOOL: case D_FAN: return s_eq(s, "on") ? C_AMBER : C_PANEL;
    case D_COVER: return s_eq(s, "open") || s_eq(s, "opening") ? C_BLUE : C_PANEL;
    case D_CLIMATE: return s_eq(s, "heat") ? C_ORANGE : s_eq(s, "cool") ? C_BLUE : s_eq(s, "off") ? C_PANEL : C_GREEN;
    case D_LOCK: return s_eq(s, "locked") || s_eq(s, "locking") ? C_PANEL : C_RED;
    case D_MEDIA: return s_eq(s, "playing") ? C_GREEN : C_PANEL;
    case D_VACUUM: return s_eq(s, "cleaning") || s_eq(s, "returning") ? C_GREEN : C_PANEL;
    case D_BINARY: {
        if (!s_eq(s, "on")) return C_PANEL;
        static const char *const alarm[] = { "moisture", "smoke", "gas", "carbon_monoxide", 0 };
        static const char *const opening[] = { "door", "window", "garage_door", "opening", 0 };
        return s_in(t->dc, alarm) ? C_RED : s_in(t->dc, opening) ? C_BLUE : C_AMBER;
    }
    default: return C_PANEL;
    }
}

static int tile_icon(const Tile *t, int *tint) {
    const int on = s_eq(t->state, "on");
    *tint = C_DIM;
    switch (t->dom) {
    case D_LIGHT: return on ? IC_LIGHTBULB_ON : IC_LIGHTBULB_OUTLINE;
    case D_SWITCH: case D_BOOL: return on ? IC_TOGGLE_SWITCH_OUTLINE : IC_TOGGLE_SWITCH_OFF_OUTLINE;
    case D_FAN: return on ? IC_FAN : IC_FAN_OFF;
    case D_COVER: {
        const int open = !s_eq(t->state, "closed");
        if (s_eq(t->dc, "garage") || s_eq(t->dc, "gate")) return open ? IC_GARAGE_OPEN : IC_GARAGE;
        return open ? IC_WINDOW_SHUTTER_OPEN : IC_WINDOW_SHUTTER;
    }
    case D_CLIMATE: return IC_THERMOSTAT;
    case D_LOCK:
        if (s_eq(t->state, "locked")) { *tint = C_GREEN; return IC_LOCK; }
        return IC_LOCK_OPEN_VARIANT;
    case D_MEDIA: return s_eq(t->state, "playing") ? IC_SPEAKER_PLAY : IC_SPEAKER;
    case D_VACUUM: return IC_ROBOT_VACUUM;
    case D_SCENE: return IC_PALETTE;
    case D_SCRIPT: return IC_SCRIPT_TEXT_PLAY;
    case D_SENSOR:
        if (s_eq(t->dc, "temperature")) { *tint = C_ORANGE; return IC_THERMOMETER; }
        if (s_eq(t->dc, "humidity")) { *tint = C_BLUE; return IC_WATER_PERCENT; }
        if (s_eq(t->dc, "power")) { *tint = C_AMBER; return IC_FLASH; }
        if (s_eq(t->dc, "illuminance")) { *tint = C_AMBER; return IC_EYE; }
        *tint = C_CYAN;
        return IC_GAUGE;
    case D_BINARY:
        if (s_eq(t->dc, "moisture")) return on ? IC_WATER_ALERT : IC_WATER_OFF;
        if (s_eq(t->dc, "window")) return on ? IC_WINDOW_OPEN_VARIANT : IC_WINDOW_CLOSED_VARIANT;
        if (s_eq(t->dc, "door") || s_eq(t->dc, "garage_door") || s_eq(t->dc, "opening")) return on ? IC_DOOR_OPEN : IC_DOOR_CLOSED;
        if (s_eq(t->dc, "motion") || s_eq(t->dc, "occupancy") || s_eq(t->dc, "presence")) return on ? IC_MOTION_SENSOR : IC_MOTION_SENSOR_OFF;
        return IC_ALERT_CIRCLE_OUTLINE;
    default: return IC_EYE;
    }
}

static void tile_status(const Tile *t, char *o, int cap) {
    const char *s = t->state;
    o[0] = 0;
    if (is_unavail(t)) { s_cpy(o, T(S_UNAVAIL), cap); return; }
    int k = -1;
    switch (t->dom) {
    case D_LIGHT:
        if (s_eq(s, "on") && t->bri > 0) { nv_snprintf(o, cap, "%s \xC2\xB7 %d%%", T(S_ON), t->bri); return; }
        k = s_eq(s, "on") ? S_ON : S_OFF;
        break;
    case D_SWITCH: case D_BOOL: case D_FAN: k = s_eq(s, "on") ? S_ON : S_OFF; break;
    case D_COVER:
        k = s_eq(s, "open") ? S_OPEN : s_eq(s, "closed") ? S_CLOSED : s_eq(s, "opening") ? S_OPENING : s_eq(s, "closing") ? S_CLOSING : -1;
        break;
    case D_CLIMATE: {
        if (s_eq(s, "off")) { k = S_OFF; break; }
        const int m = s_eq(s, "heat") ? S_HEAT : s_eq(s, "cool") ? S_COOL : S_AUTO;
        char a[12], b[12];
        if (t->t10 != NA) { fmt10(a, sizeof a, t->t10); nv_snprintf(o, cap, "%s \xE2\x86\x92 %s\xC2\xB0", T(m), a); }
        else if (t->lo10 != NA && t->hi10 != NA) {
            fmt10(a, sizeof a, t->lo10); fmt10(b, sizeof b, t->hi10);
            nv_snprintf(o, cap, "%s %s\xE2\x80\x93%s\xC2\xB0", T(m), a, b);
        } else s_cpy(o, T(m), cap);
        return;
    }
    case D_LOCK: k = s_eq(s, "locked") ? S_LOCKED : s_eq(s, "unlocked") || s_eq(s, "open") ? S_UNLOCKED : s_eq(s, "jammed") ? S_JAMMED : -1; break;
    case D_MEDIA: k = s_eq(s, "playing") ? S_PLAYING : s_eq(s, "paused") ? S_PAUSED : s_eq(s, "off") ? S_OFF : s_eq(s, "idle") ? S_IDLE : s_eq(s, "on") ? S_ON : -1; break;
    case D_VACUUM: k = s_eq(s, "cleaning") ? S_CLEANING : s_eq(s, "docked") ? S_DOCKED : s_eq(s, "returning") ? S_RETURNING : s_eq(s, "idle") ? S_IDLE : s_eq(s, "paused") ? S_PAUSED : -1; break;
    case D_SCENE: case D_SCRIPT: k = S_RUN; break;
    case D_SENSOR: if (g_area < 0) s_cpy(o, t->area, cap); return;     // in a room's view the room goes without saying
    case D_BINARY: {
        const int on = s_eq(s, "on");
        static const char *const alarm[] = { "smoke", "gas", "carbon_monoxide", "motion", "occupancy", "presence", 0 };
        if (s_eq(t->dc, "moisture")) k = on ? S_WET : S_DRY;
        else if (s_in(t->dc, alarm)) k = on ? S_DETECTED : S_CLEAR;
        else k = on ? S_OPEN : S_CLOSED;
        break;
    }
    default: break;
    }
    s_cpy(o, k >= 0 ? T(k) : s, cap);
}

// ---- layout: every control, for drawing, taps and the keyboard ----------------------------------
enum { K_CHIP, K_MORE, K_TILE, K_PREV, K_NEXT, K_CANCEL, K_OK };
typedef struct { short x, y, w, h; unsigned char kind; short idx; } Ctl;
static Ctl g_ctl[40];
static int g_nc, W, H;
static int g_page, g_chip0, g_focus = -1, g_fkind = -1, g_fidx, g_kbd;
#define GX 19
#define GY 148
#define GAP 14
#define COLS 4
#define PER 12
#define PAGER_W 160

static int visible(int *idx) {
    int n = 0;
    for (int i = 0; i < g_nt; i++) if (g_area < 0 || s_eq(g_t[i].area, g_areas[g_area])) idx[n++] = i;
    return n;
}
static const char *chip_label(int c) { return c == 0 ? T(S_ALL) : g_areas[c - 1]; }
static int chip_w(int c) { return text_w(chip_label(c), F_S) + 36; }

static void add(int x, int y, int w, int h, int kind, int idx) {
    if (g_nc >= 40) return;
    Ctl *c = &g_ctl[g_nc++];
    c->x = (short)x; c->y = (short)y; c->w = (short)w; c->h = (short)h; c->kind = (unsigned char)kind; c->idx = (short)idx;
}

static int g_pages;
static void layout(void) {
    g_nc = 0;
    if (g_sheet >= 0) {
        add(264, 340, 236, 56, K_CANCEL, 0);
        add(524, 340, 236, 56, K_OK, 0);
    } else if (g_status == ST_OK) {
        int idx[MAXT];
        const int n = visible(idx);
        g_pages = (n + PER - 1) / PER;
        if (g_page >= g_pages) g_page = g_pages > 0 ? g_pages - 1 : 0;
        // the pager sits at the right of the rooms row: the bottom 24 px belong to the system's
        // edge gestures (home, recents), a control there would not get its taps
        const int pager = g_pages > 1 ? PAGER_W + 16 : 0;
        const int nchips = g_na + 1;
        if (g_chip0 >= nchips) g_chip0 = 0;
        int x = 24;
        for (int c = g_chip0; c < nchips; c++) {
            const int w = chip_w(c);
            const int room = W - 24 - pager - (c + 1 < nchips ? 56 : 0);   // keep space for the "more" chip
            if (x + w > room) { add(W - 24 - pager - 48, 90, 48, 40, K_MORE, c); break; }
            add(x, 90, w, 40, K_CHIP, c - 1);
            x += w + 10;
        }
        for (int k = 0; k < PER && g_page * PER + k < n; k++)
            add(GX + (k % COLS) * (TW + GAP), GY + (k / COLS) * (TH + GAP), TW, TH, K_TILE, idx[g_page * PER + k]);
        if (g_pages > 1) {
            add(W - 24 - PAGER_W, 90, 48, 40, K_PREV, 0);
            add(W - 24 - 48, 90, 48, 40, K_NEXT, 0);
        }
    }
    g_focus = -1;
    if (g_fkind >= 0)
        for (int i = 0; i < g_nc; i++) if (g_ctl[i].kind == g_fkind && g_ctl[i].idx == g_fidx) { g_focus = i; break; }
    if (g_focus < 0 && g_fkind >= 0 && g_nc > 0) {      // the focused control left: nearest by kind
        for (int i = 0; i < g_nc; i++) if (g_ctl[i].kind == g_fkind) { g_focus = i; break; }
        if (g_focus < 0) g_focus = 0;
        g_fkind = g_ctl[g_focus].kind;
        g_fidx = g_ctl[g_focus].idx;
    }
}

static void set_focus(int i) {
    if (i < 0 || i >= g_nc) return;
    g_focus = i;
    g_fkind = g_ctl[i].kind;
    g_fidx = g_ctl[i].idx;
    g_redraw = 2;
}

// ---- drawing ------------------------------------------------------------------------------------
static void ring(const Ctl *c, int r) {
    if (g_kbd && g_focus >= 0 && c == &g_ctl[g_focus]) nv_gfx_panel(c->x - 4, c->y - 4, c->w + 8, c->h + 8, r + 4, C_FOCUS, C_FOCUS, 255);
}

static void draw_tile(const Ctl *c) {
    const int i = c->idx, x = c->x, y = c->y;
    const Tile *t = &g_t[i];
    const int col = tile_colour(i);
    const int lit = col != C_PANEL, fam = lit ? 1 : 0;
    const int ink = lit ? col : is_unavail(t) ? C_FAINT : C_INK;
    const int dim = lit ? col : C_DIM;
    ring(c, 18);
    nv_gfx_panel(x, y, TW, TH, 18, col, col, 255);
    int itint;
    const int ic = tile_icon(t, &itint);
    icon(ic, x + 16, y + 14, lit ? col : is_unavail(t) ? C_FAINT : itint, fam);
    // top right: a value (sensors, thermostats) or the room
    char v[24];
    v[0] = 0;
    if (t->dom == D_SENSOR && !is_unavail(t)) { const int n = s_10(t->state); if (n != NA) fmt10(v, sizeof v, n); }
    if (t->dom == D_CLIMATE && t->cur10 != NA) { fmt10(v, sizeof v, t->cur10); }
    if (v[0]) {
        const char *u = t->dom == D_CLIMATE ? "\xC2\xB0" : t->unit;
        const int uw = u[0] ? text_w(u, F_S) + 3 : 0;
        text_right(x + TW - 16 - uw, y + 6, v, F_L, ink, fam);
        if (u[0]) text(x + TW - 16 - uw + 3, y + 14, u, F_S, dim, fam);
    } else if (t->area[0] && g_area < 0) {
        char a[40];
        fit(a, sizeof a, t->area, F_S, 130);
        text_right(x + TW - 16, y + 20, a, F_S, lit ? col : C_FAINT, fam);
    }
    text_fit(x + 16, y + 60, t->name, F_M, ink, fam, TW - 32);
    char st[48];
    tile_status(t, st, sizeof st);
    const int climate_btns = t->dom == D_CLIMATE && !s_eq(t->state, "off") && !is_unavail(t) && (t->t10 != NA || t->lo10 != NA);
    if (climate_btns && text_w(st, F_S) > TW - 32 - 100) {          // "Heating -> 21.5" too long: the target alone
        const char *p = st;
        while (*p && !((unsigned char)p[0] == 0xE2 && (unsigned char)p[1] == 0x86 && (unsigned char)p[2] == 0x92)) p++;   // U+2192
        if (!*p) { p = st; while (*p && *p != ' ') p++; while (*p == ' ') p++; }   // "Auto 21-24": the range
        if (*p) { char tmp[48]; s_cpy(tmp, p, sizeof tmp); s_cpy(st, tmp, sizeof st); }
    }
    text_fit(x + 16, y + (climate_btns ? 96 : 92), st, F_S, dim, fam, TW - 32 - (climate_btns ? 100 : 0));
    if (t->dom == D_LIGHT && s_eq(t->state, "on")) {
        const int bw = TW - 32, p = t->bri > 0 ? t->bri : 100;
        nv_gfx_panel(x + 16, y + TH - 12, bw, 5, 2, shade(col, 72), shade(col, 72), 255);
        nv_gfx_panel(x + 16, y + TH - 12, bw * p / 100, 5, 2, C_BLACK, C_BLACK, 255);
    }
    if (climate_btns) {
        const int bc = lit ? shade(col, 82) : C_PANEL2;
        nv_gfx_panel(x + TW - 104, y + 86, 44, 36, 18, bc, bc, 255);
        nv_gfx_panel(x + TW - 56, y + 86, 44, 36, 18, bc, bc, 255);
        icon(IC_MINUS, x + TW - 104 + 4, y + 86, lit ? bc : C_INK, fam);
        icon(IC_PLUS, x + TW - 56 + 4, y + 86, lit ? bc : C_INK, fam);
    }
}

static void text_center(int y, const char *s, int f, int tint, int maxw) {
    char b[160];
    fit(b, sizeof b, s, f, maxw);
    text((W - text_w(b, f)) / 2, y, b, f, tint, 0);
}

static void draw(void) {
    nv_gfx_clear(C_BG);
    icon(IC_HOME_ASSISTANT, 24, 24, C_CYAN, 0);
    text(70, 14, T(S_TITLE), F_L, C_INK, 0);
    // connection, top right
    const int sc = g_status != ST_OK ? C_RED : g_live ? C_GREEN : C_DIM;
    const char *sl = g_status != ST_OK && g_status != ST_LOADING ? T(S_OFFLINE) : g_live ? T(S_LIVE) : T(S_POLL);
    if (g_status != ST_LOADING) {
        const int w = text_w(sl, F_S);
        text(W - 24 - w, 30, sl, F_S, C_DIM, 0);
        nv_gfx_panel(W - 24 - w - 18, 36, 10, 10, 5, sc, sc, 255);
    }
    if (g_status != ST_OK) {
        if (g_status == ST_LOADING) { text_center(286, T(S_LOADING), F_M, C_DIM, 900); return; }
        const int ic = g_status == ST_UNREACH ? IC_LAN_DISCONNECT : IC_ALERT_CIRCLE_OUTLINE;
        icon(ic, (W - ICON_PX) / 2, 210, C_DIM, 0);
        const int l1 = g_status == ST_NOCONF ? S_NOCONF1 : g_status == ST_AUTH ? S_AUTH : S_UNREACH;
        text_center(262, T(l1), F_M, C_INK, 960);
        if (g_status == ST_NOCONF) text_center(302, T(S_NOCONF2), F_S, C_DIM, 960);
        else if (g_status == ST_UNREACH) {
            char d[32];
            if (g_err >= 100) nv_snprintf(d, sizeof d, "HTTP %d", g_err);
            else nv_snprintf(d, sizeof d, "(%d)", g_err);
            text_center(302, d, F_S, C_FAINT, 400);
        }
        return;
    }
    for (int i = 0; i < g_nc && g_sheet < 0; i++) {
        const Ctl *c = &g_ctl[i];
        if (c->kind == K_CHIP) {
            const int sel = c->idx == g_area;
            ring(c, 20);
            nv_gfx_panel(c->x, c->y, c->w, c->h, 20, sel ? C_BLUE : C_PANEL, sel ? C_BLUE : C_PANEL, 255);
            text(c->x + 18, c->y + 10, chip_label(c->idx + 1), F_S, sel ? C_BLUE : C_INK, sel);
        } else if (c->kind == K_MORE) {
            ring(c, 20);
            nv_gfx_panel(c->x, c->y, c->w, c->h, 20, C_PANEL, C_PANEL, 255);
            icon(IC_CHEVRON_RIGHT, c->x + 6, c->y + 2, C_INK, 0);
        } else if (c->kind == K_TILE) draw_tile(c);
        else if (c->kind == K_PREV || c->kind == K_NEXT) {
            ring(c, 18);
            nv_gfx_panel(c->x, c->y, c->w, c->h, 18, C_PANEL, C_PANEL, 255);
            const int dim = (c->kind == K_PREV && g_page == 0) || (c->kind == K_NEXT && g_page + 1 >= g_pages);
            icon(c->kind == K_PREV ? IC_CHEVRON_LEFT : IC_CHEVRON_RIGHT, c->x + 6, c->y + 2, dim ? C_FAINT : C_INK, 0);
        }
    }
    int idx[MAXT];
    if (!visible(idx)) text_center(300, T(S_EMPTY), F_M, C_DIM, 900);
    if (g_pages > 1 && g_sheet < 0) {                 // "2/4" between the pager's arrows
        char pg[12];
        nv_snprintf(pg, sizeof pg, "%d/%d", g_page + 1, g_pages);
        text(W - 24 - PAGER_W / 2 - text_w(pg, F_S) / 2, 100, pg, F_S, C_DIM, 0);
    }
    if (g_toast && nv_millis() - g_toast_until < 0) {
        const int w = text_w(g_toast, F_S) + 40;
        nv_gfx_panel((W - w) / 2, 548, w, 40, 20, C_PANEL2, C_PANEL2, 255);
        text((W - w) / 2 + 20, 558, g_toast, F_S, C_INK, 0);
    }
    if (g_sheet >= 0 && g_sheet < g_nt) {
        const Tile *t = &g_t[g_sheet];
        nv_gfx_panel(0, 0, W, H, 0, C_BLACK, C_BLACK, 170);
        nv_gfx_panel(232, 170, 560, 260, 24, C_PANEL2, C_PANEL2, 255);
        icon(g_sheet_open ? IC_GARAGE_OPEN : IC_LOCK_OPEN_VARIANT, 264, 206, C_RED, 0);
        char q[96], name[48];
        fit(name, sizeof name, t->name, F_M, 300);
        nv_snprintf(q, sizeof q, T(g_sheet_open ? S_Q_OPEN : S_Q_UNLOCK), name);
        text_fit(316, 206, q, F_M, C_INK, 0, 444);
        text_fit(316, 246, T(S_Q_NOTE), F_S, C_DIM, 0, 444);
        for (int i = 0; i < g_nc; i++) {
            const Ctl *c = &g_ctl[i];
            const int ok = c->kind == K_OK, bc = ok ? C_RED : C_PANEL;
            ring(c, 28);
            nv_gfx_panel(c->x, c->y, c->w, c->h, 28, bc, bc, 255);
            const char *l = ok ? T(g_sheet_open ? S_OPEN_V : S_UNLOCK) : T(S_CANCEL);
            text(c->x + (c->w - text_w(l, F_M)) / 2, c->y + 14, l, F_M, ok ? C_RED : C_INK, ok);
        }
    }
}

// ---- input --------------------------------------------------------------------------------------
static void activate(int ci, int lx, int ly) {
    if (ci < 0 || ci >= g_nc) return;
    const Ctl *c = &g_ctl[ci];
    switch (c->kind) {
    case K_CHIP: g_area = c->idx; g_page = 0; break;
    case K_MORE: g_chip0 = c->idx; break;
    case K_TILE: do_tile(c->idx, lx, ly); break;
    case K_PREV: if (g_page > 0) g_page--; break;
    case K_NEXT: if (g_page + 1 < g_pages) g_page++; break;
    case K_CANCEL: g_sheet = -1; break;
    case K_OK: sheet_confirm(); break;
    default: break;
    }
    g_redraw = 2;
}

static void tap(int x, int y) {
    for (int i = 0; i < g_nc; i++) {
        const Ctl *c = &g_ctl[i];
        if (x >= c->x && x < c->x + c->w && y >= c->y && y < c->y + c->h) { activate(i, x - c->x, y - c->y); return; }
    }
    if (g_sheet >= 0 && (x < 232 || x >= 792 || y < 170 || y >= 430)) { g_sheet = -1; g_redraw = 2; }   // tap outside
}

// nearest control in a direction (dx, dy one of -1/0/1)
static void move(int dx, int dy) {
    if (!g_nc) return;
    if (g_focus < 0) {
        for (int i = 0; i < g_nc; i++) if (g_ctl[i].kind == K_TILE || g_ctl[i].kind == K_CANCEL) { set_focus(i); return; }
        set_focus(0);
        return;
    }
    const Ctl *f = &g_ctl[g_focus];
    const int fx = f->x + f->w / 2, fy = f->y + f->h / 2;
    int best = -1, bs = 0x7fffffff;
    for (int i = 0; i < g_nc; i++) {
        if (i == g_focus) continue;
        const Ctl *c = &g_ctl[i];
        const int ex = c->x + c->w / 2 - fx, ey = c->y + c->h / 2 - fy;
        const int along = dx ? ex * dx : ey * dy, across = dx ? (ey < 0 ? -ey : ey) : (ex < 0 ? -ex : ex);
        if (along <= 8) continue;
        const int score = along + across * 3;
        if (score < bs) { bs = score; best = i; }
    }
    if (best >= 0) set_focus(best);
    else if (dx && (g_ctl[g_focus].kind == K_TILE)) {    // past the grid's edge: turn the page
        if (dx > 0 && g_page + 1 < g_pages) { g_page++; g_redraw = 2; }
        if (dx < 0 && g_page > 0) { g_page--; g_redraw = 2; }
    }
}

static int g_quit, g_esc_t;
static void key(int u, int mods) {
    g_kbd = 1;
    g_redraw = 2;
    const int shift = mods & 0x22;
    switch (u) {
    case 0x29:                                          // Esc
        g_esc_t = nv_millis();
        if (g_sheet >= 0) g_sheet = -1;
        else g_quit = 1;
        break;
    case 0x2B: {                                        // Tab / Shift+Tab
        if (!g_nc) break;
        const int n = g_focus < 0 ? (shift ? g_nc - 1 : 0) : (g_focus + (shift ? g_nc - 1 : 1)) % g_nc;
        set_focus(n);
        break;
    }
    case 0x4F: move(1, 0); break;
    case 0x50: move(-1, 0); break;
    case 0x51: move(0, 1); break;
    case 0x52: move(0, -1); break;
    case 0x28: case 0x58: case 0x2C:                    // Enter, keypad Enter, Space
        if (g_focus < 0) move(0, 0);
        else activate(g_focus, -1, -1);
        break;
    case 0x2E: case 0x57: case 0x2D: case 0x56:         // + / -: brightness, temperature
        if (g_focus >= 0 && g_ctl[g_focus].kind == K_TILE) adjust(&g_t[g_ctl[g_focus].idx], (u == 0x2E || u == 0x57) ? 1 : -1);
        break;
    case 0x4B: if (g_page > 0) g_page--; break;         // PgUp
    case 0x4E: if (g_page + 1 < g_pages) g_page++; break;   // PgDn
    default: g_redraw = 0; break;
    }
}

NV_EXPORT("run")
void run(void) {
    W = nv_gfx_width();
    H = nv_gfx_height();
    char lang[8] = "";
    nv_lang(lang, sizeof lang);
    g_lang = s_eq(lang, "it") ? 1 : s_eq(lang, "es") ? 2 : s_eq(lang, "fr") ? 3 : s_eq(lang, "de") ? 4 : 0;
    if (!nv_ha_available()) g_status = ST_NOCONF;
    int prev = 0, sx = 0, sy = 0, rep_key = 0, rep_t = 0;
    uint8_t kb[8], kprev[8] = { 0 };
    int nprev = 0;
    while (nv_gfx_present()) {
        if (nv_gfx_back() && nv_millis() - g_esc_t > 400) {
            if (g_sheet >= 0) { g_sheet = -1; g_redraw = 2; }
            else break;
        }
        // keyboard: presses are keys held now that were not held before; arrows repeat
        const int nk = nv_kbd_state(kb, sizeof kb);
        if (nk >= 0) {
            for (int i = 1; i <= nk && i < 7; i++) {
                int was = 0;
                for (int j = 1; j <= nprev && j < 7; j++) if (kprev[j] == kb[i]) was = 1;
                if (!was && kb[i] > 3) {
                    layout();
                    key(kb[i], kb[0]);
                    if (kb[i] >= 0x4F && kb[i] <= 0x52) { rep_key = kb[i]; rep_t = nv_millis() + 380; }
                }
            }
            int held = 0;
            for (int i = 1; i <= nk && i < 7; i++) if (kb[i] == rep_key) held = 1;
            if (!held) rep_key = 0;
            else if (nv_millis() - rep_t > 0) { layout(); key(rep_key, kb[0]); rep_t = nv_millis() + 110; }
            for (int i = 0; i < 8; i++) kprev[i] = kb[i];
            nprev = nk;
        }
        if (g_quit) break;
        // touch and mouse: a tap is a release that did not travel; a long horizontal drag turns pages
        int x, y;
        const int down = nv_touch(&x, &y);
        if (down && !prev) { sx = x; sy = y; }
        if (!down && prev) {
            const int dx = x - sx, dy = y - sy;
            layout();
            if (g_sheet < 0 && g_status == ST_OK && (dx > 120 || dx < -120) && dy * dy < dx * dx) {
                if (dx < 0 && g_page + 1 < g_pages) g_page++;
                if (dx > 0 && g_page > 0) g_page--;
            } else tap(sx, sy);
            g_kbd = 0;
            g_redraw = 2;
        }
        prev = down;
        if (g_status != ST_NOCONF) {
            ws_step();
            poll_step();
            act_step();
        }
        if (g_toast && g_toast_until && nv_millis() - g_toast_until > 0) { g_toast = 0; g_redraw = 2; }
        if (g_flash_tile >= 0 && nv_millis() - g_flash_until > 0) { g_flash_tile = -1; g_redraw = 2; }
        if (g_redraw > 0) { layout(); draw(); g_redraw--; }
    }
    if (g_poll_h >= 0) nv_http_close(g_poll_h);
    for (int k = 0; k < 4; k++) if (g_act[k] >= 0) nv_http_close(g_act[k]);
    if (g_ws >= 0) nv_ws_close(g_ws);
}
