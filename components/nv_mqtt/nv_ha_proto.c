// nv_ha_proto — see header. Pure C, no allocation, every loop bounded by an explicit length.
#include "nv_ha_proto.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

// Discovery keys use HA's documented abbreviations (stat_t, cmd_t, avty_t, ...) to keep each
// retained config small. "~" is the per-entity base topic.
static const ha_entity_def_t kDefs[HA_E_COUNT] = {
    [HA_E_SCREEN] = {"screen", "light", "Screen", "Schermo", true, true,
                     "\"schema\":\"json\",\"sup_clrm\":[\"brightness\"],\"bri_scl\":100,\"ic\":\"mdi:tablet\""},
    [HA_E_VOLUME] = {"volume", "number", "Volume", "Volume", true, true,
                     "\"min\":0,\"max\":100,\"step\":5,\"unit_of_meas\":\"%\",\"ic\":\"mdi:volume-high\""},
    [HA_E_MUTE]   = {"mute", "switch", "Mute", "Muto", true, true, "\"ic\":\"mdi:volume-off\""},
    [HA_E_DND]    = {"dnd", "switch", "Do not disturb", "Non disturbare", true, true,
                     "\"ic\":\"mdi:bell-off\""},
    [HA_E_NOTIFY] = {"notify", "notify", "Notification", "Notifica", true, false,
                     "\"ic\":\"mdi:message-badge\""},
    [HA_E_SAY]    = {"say", "notify", "Speak", "Parla", true, false, "\"ic\":\"mdi:account-voice\""},
    [HA_E_APP]    = {"app", "select", "App", "App", true, true, "\"ic\":\"mdi:apps\""},
    [HA_E_HOME]   = {"home", "button", "Go home", "Vai alla home", true, false, "\"ic\":\"mdi:home\""},
    [HA_E_LOCK]   = {"lock", "button", "Lock screen", "Blocca schermo", true, false, "\"ic\":\"mdi:lock\""},
    [HA_E_REBOOT] = {"reboot", "button", "Restart", "Riavvia", true, false,
                     "\"dev_cla\":\"restart\",\"ent_cat\":\"config\""},
    [HA_E_LOCKED] = {"locked", "binary_sensor", "Locked", "Bloccato", false, true,
                     "\"ic\":\"mdi:lock-outline\""},
    [HA_E_TOUCH]  = {"touch", "binary_sensor", "Touch activity", "Attivita' touch", false, true,
                     "\"dev_cla\":\"occupancy\""},
    [HA_E_TEMP]   = {"temp", "sensor", "Chip temperature", "Temperatura chip", false, false,
                     "\"dev_cla\":\"temperature\",\"unit_of_meas\":\"\\u00b0C\",\"stat_cla\":\"measurement\","
                     "\"ent_cat\":\"diagnostic\",\"val_tpl\":\"{{ value_json.temp }}\""},
    [HA_E_RSSI]   = {"rssi", "sensor", "Wi-Fi signal", "Segnale Wi-Fi", false, false,
                     "\"dev_cla\":\"signal_strength\",\"unit_of_meas\":\"dBm\",\"stat_cla\":\"measurement\","
                     "\"ent_cat\":\"diagnostic\",\"val_tpl\":\"{{ value_json.rssi }}\""},
    [HA_E_UPTIME] = {"uptime", "sensor", "Uptime", "Acceso da", false, false,
                     "\"dev_cla\":\"duration\",\"unit_of_meas\":\"s\",\"stat_cla\":\"total_increasing\","
                     "\"ent_cat\":\"diagnostic\",\"val_tpl\":\"{{ value_json.uptime }}\""},
    [HA_E_SRAM]   = {"sram", "sensor", "Free internal RAM", "RAM interna libera", false, false,
                     "\"dev_cla\":\"data_size\",\"unit_of_meas\":\"kB\",\"stat_cla\":\"measurement\","
                     "\"ent_cat\":\"diagnostic\",\"val_tpl\":\"{{ value_json.sram }}\""},
    [HA_E_PSRAM]  = {"psram", "sensor", "Free PSRAM", "PSRAM libera", false, false,
                     "\"dev_cla\":\"data_size\",\"unit_of_meas\":\"kB\",\"stat_cla\":\"measurement\","
                     "\"ent_cat\":\"diagnostic\",\"val_tpl\":\"{{ value_json.psram }}\""},
    [HA_E_IP]     = {"ip", "sensor", "IP address", "Indirizzo IP", false, false,
                     "\"ic\":\"mdi:ip-network\",\"ent_cat\":\"diagnostic\",\"val_tpl\":\"{{ value_json.ip }}\""},
};

const ha_entity_def_t *ha_entity(ha_entity_t e)
{
    return (unsigned)e < HA_E_COUNT ? &kDefs[e] : NULL;
}

static bool fits(int r, size_t cap) { return r >= 0 && (size_t)r < cap; }

bool ha_topic(char *out, size_t cap, const char *node, const char *obj, const char *leaf)
{
    return fits(snprintf(out, cap, "nucleo/%s/%s/%s", node, obj, leaf), cap);
}

bool ha_config_topic(char *out, size_t cap, const char *prefix, const char *node, ha_entity_t e)
{
    const ha_entity_def_t *d = ha_entity(e);
    if (!d) return false;
    return fits(snprintf(out, cap, "%s/%s/%s/%s/config", prefix, d->component, node, d->obj), cap);
}

size_t ha_json_escape(char *out, size_t n, const char *s)
{
    if (!n) return 0;
    size_t o = 0;
    for (; s && *s; s++) {
        const unsigned char c = (unsigned char)*s;
        char tmp[8];
        size_t k;
        if (c == '"' || c == '\\') { tmp[0] = '\\'; tmp[1] = (char)c; k = 2; }
        else if (c == '\n') { memcpy(tmp, "\\n", 2); k = 2; }
        else if (c < 0x20) { snprintf(tmp, sizeof tmp, "\\u%04x", c); k = 6; }
        else { tmp[0] = (char)c; k = 1; }
        if (o + k >= n) break;
        memcpy(out + o, tmp, k);
        o += k;
    }
    // Never leave a dangling UTF-8 lead/continuation sequence at a truncation point.
    if (s && *s) {
        size_t cut = o;
        while (cut > 0 && ((unsigned char)out[cut - 1] & 0xC0) == 0x80) cut--;
        if (cut > 0 && ((unsigned char)out[cut - 1] & 0xC0) == 0xC0) o = cut - 1;
    }
    out[o] = '\0';
    return o;
}

size_t ha_build_config(char *out, size_t cap, ha_entity_t e, const char *node, bool italian,
                       const char *sw, const char *model, const char *options_json)
{
    const ha_entity_def_t *d = ha_entity(e);
    if (!d || !out || cap == 0 || !node) return 0;
    char swe[40], mdl[48];
    ha_json_escape(swe, sizeof swe, sw ? sw : "");
    ha_json_escape(mdl, sizeof mdl, model ? model : "");
    const char *suffix = strchr(node, '_');
    suffix = suffix ? suffix + 1 : node;

    size_t o = 0;
    int r = snprintf(out, cap,
                     "{\"~\":\"nucleo/%s\",\"name\":\"%s\",\"uniq_id\":\"%s_%s\",\"avty_t\":\"~/status\"",
                     node, italian ? d->name_it : d->name_en, node, d->obj);
    if (!fits(r, cap)) return 0;
    o = (size_t)r;
#define APPEND(...) do { r = snprintf(out + o, cap - o, __VA_ARGS__); \
                         if (!fits(r, cap - o)) return 0; o += (size_t)r; } while (0)
    if (d->has_state) APPEND(",\"stat_t\":\"~/%s/state\"", d->obj);
    else if (!d->has_cmd) APPEND(",\"stat_t\":\"~/diag\"");
    if (d->has_cmd) APPEND(",\"cmd_t\":\"~/%s/set\"", d->obj);
    if (d->extra[0]) APPEND(",%s", d->extra);
    if (e == HA_E_APP) APPEND(",\"ops\":%s", (options_json && options_json[0]) ? options_json : "[\"home\"]");
    APPEND(",\"dev\":{\"ids\":[\"%s\"],\"name\":\"NucleoOS %s\",\"mf\":\"NucleoOS\",\"mdl\":\"%s\",\"sw\":\"%s\"}"
           ",\"o\":{\"name\":\"NucleoOS\",\"sw\":\"%s\"}}",
           node, suffix, mdl, swe, swe);
#undef APPEND
    return o;
}

ha_entity_t ha_route_cmd(const char *topic, size_t len, const char *node)
{
    if (!topic || !node) return HA_E_COUNT;
    static const char kPre[] = "nucleo/";
    const size_t pre = sizeof kPre - 1, nl = strlen(node);
    if (len < pre + nl + 1 || memcmp(topic, kPre, pre) != 0) return HA_E_COUNT;
    if (memcmp(topic + pre, node, nl) != 0 || topic[pre + nl] != '/') return HA_E_COUNT;
    const char *rest = topic + pre + nl + 1;
    const size_t rl = len - (pre + nl + 1);
    for (int i = 0; i < HA_E_COUNT; i++) {
        if (!kDefs[i].has_cmd) continue;
        const size_t ol = strlen(kDefs[i].obj);
        if (rl == ol + 4 && memcmp(rest, kDefs[i].obj, ol) == 0 && memcmp(rest + ol, "/set", 4) == 0)
            return (ha_entity_t)i;
    }
    return HA_E_COUNT;
}

// ---------------------------------------------------------------- flat JSON reader

static size_t skip_ws(const char *j, size_t i, size_t len)
{
    while (i < len && (j[i] == ' ' || j[i] == '\t' || j[i] == '\n' || j[i] == '\r')) i++;
    return i;
}

static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Parse a JSON string starting at j[i] == '"'. If out != NULL, the unescaped value goes there
// (truncated to n-1). Returns the index just past the closing quote, or 0 on error.
static size_t parse_str(const char *j, size_t i, size_t len, char *out, size_t n)
{
    size_t o = 0;
    if (i >= len || j[i] != '"') return 0;
    for (i++; i < len; i++) {
        unsigned char c = (unsigned char)j[i];
        if (c == '"') {
            if (out && n) out[o < n ? o : n - 1] = '\0';
            return i + 1;
        }
        if (c < 0x20) return 0;                        // raw control char: invalid JSON
        char buf[4];
        size_t k = 1;
        if (c == '\\') {
            if (++i >= len) return 0;
            const char esc = j[i];
            switch (esc) {
                case '"': case '\\': case '/': buf[0] = esc; break;
                case 'n': buf[0] = '\n'; break;
                case 'b': case 'f': case 'r': case 't': buf[0] = ' '; break;
                case 'u': {
                    if (i + 4 >= len) return 0;
                    unsigned cp = 0;
                    for (int q = 1; q <= 4; q++) {
                        const int h = hexv(j[i + q]);
                        if (h < 0) return 0;
                        cp = (cp << 4) | (unsigned)h;
                    }
                    i += 4;
                    if (cp < 0x20) { k = 0; }                    // control: drop
                    else if (cp >= 0xD800 && cp <= 0xDFFF) { buf[0] = '?'; }  // surrogates: no pairs
                    else if (cp < 0x80) { buf[0] = (char)cp; }
                    else if (cp < 0x800) { buf[0] = (char)(0xC0 | (cp >> 6)); buf[1] = (char)(0x80 | (cp & 0x3F)); k = 2; }
                    else { buf[0] = (char)(0xE0 | (cp >> 12)); buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                           buf[2] = (char)(0x80 | (cp & 0x3F)); k = 3; }
                    break;
                }
                default: return 0;
            }
        } else {
            buf[0] = (char)c;
        }
        if (out && n) {
            if (o + k < n) { memcpy(out + o, buf, k); o += k; }
            else o = n;                                  // full: keep scanning to the close quote
        }
    }
    return 0;
}

// Skip any JSON value starting at j[i]; nested containers are tracked by depth only (strings
// inside them are honored so a '}' in a string doesn't close anything). Returns index past it.
static size_t skip_value(const char *j, size_t i, size_t len)
{
    i = skip_ws(j, i, len);
    if (i >= len) return 0;
    if (j[i] == '"') return parse_str(j, i, len, NULL, 0);
    if (j[i] == '{' || j[i] == '[') {
        int depth = 0;
        for (; i < len; i++) {
            const char c = j[i];
            if (c == '"') {
                const size_t e = parse_str(j, i, len, NULL, 0);
                if (!e) return 0;
                i = e - 1;
            } else if (c == '{' || c == '[') {
                if (++depth > 32) return 0;
            } else if (c == '}' || c == ']') {
                if (--depth == 0) return i + 1;
            }
        }
        return 0;
    }
    const size_t s = i;                                   // literal: number/true/false/null
    while (i < len && j[i] != ',' && j[i] != '}' && j[i] != ']' && j[i] != ' ' &&
           j[i] != '\t' && j[i] != '\n' && j[i] != '\r')
        i++;
    return i > s ? i : 0;
}

bool ha_json_get(const char *j, size_t len, const char *key, char *out, size_t n)
{
    if (!j || !key || !out || n == 0) return false;
    out[0] = '\0';
    size_t i = skip_ws(j, 0, len);
    if (i >= len || j[i] != '{') return false;
    i++;
    char k[32];
    for (;;) {
        i = skip_ws(j, i, len);
        if (i >= len) return false;
        if (j[i] == '}') return false;
        const size_t ke = parse_str(j, i, len, k, sizeof k);
        if (!ke) return false;
        i = skip_ws(j, ke, len);
        if (i >= len || j[i] != ':') return false;
        i = skip_ws(j, i + 1, len);
        if (i >= len) return false;
        if (strcmp(k, key) == 0) {
            if (j[i] == '"') return parse_str(j, i, len, out, n) != 0;
            if (j[i] == '{' || j[i] == '[') return false;
            const size_t e = skip_value(j, i, len);
            if (!e) return false;
            const size_t l = e - i < n - 1 ? e - i : n - 1;
            memcpy(out, j + i, l);
            out[l] = '\0';
            return true;
        }
        const size_t e = skip_value(j, i, len);
        if (!e) return false;
        i = skip_ws(j, e, len);
        if (i < len && j[i] == ',') { i++; continue; }
        return false;                                     // '}' (key absent) or garbage
    }
}

bool ha_parse_int(const char *p, size_t len, int *out)
{
    size_t i = 0;
    while (i < len && (p[i] == ' ' || p[i] == '\t' || p[i] == '\n' || p[i] == '\r')) i++;
    bool neg = false;
    if (i < len && (p[i] == '-' || p[i] == '+')) { neg = p[i] == '-'; i++; }
    const size_t ds = i;
    long long v = 0;                                      // saturates: 32-bit long would overflow
    while (i < len && p[i] >= '0' && p[i] <= '9') {
        if (v < 1000000000000LL) v = v * 10 + (p[i] - '0');
        i++;
    }
    if (i == ds) return false;
    if (i < len && p[i] == '.') {                         // HA number entities send "55.0"
        i++;
        while (i < len && p[i] >= '0' && p[i] <= '9') i++;
    }
    while (i < len && (p[i] == ' ' || p[i] == '\t' || p[i] == '\n' || p[i] == '\r')) i++;
    if (i != len && !(i < len && p[i] == '\0')) return false;
    if (v > 2147483647LL) v = 2147483647LL;
    *out = (int)(neg ? -v : v);
    return true;
}

bool ha_json_get_int(const char *json, size_t len, const char *key, int *out)
{
    char v[24];
    if (!ha_json_get(json, len, key, v, sizeof v)) return false;
    return ha_parse_int(v, strlen(v), out);
}

static bool ieq(const char *p, size_t len, const char *lit)
{
    const size_t l = strlen(lit);
    if (len != l) return false;
    for (size_t i = 0; i < l; i++) {
        char c = p[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != lit[i]) return false;
    }
    return true;
}

bool ha_parse_onoff(const char *p, size_t len, bool *on)
{
    while (len && (p[len - 1] == ' ' || p[len - 1] == '\n' || p[len - 1] == '\r' || p[len - 1] == '\0')) len--;
    while (len && (*p == ' ' || *p == '\n' || *p == '\r')) { p++; len--; }
    if (ieq(p, len, "on") || ieq(p, len, "1") || ieq(p, len, "true")) { *on = true; return true; }
    if (ieq(p, len, "off") || ieq(p, len, "0") || ieq(p, len, "false")) { *on = false; return true; }
    return false;
}

// Length of a valid UTF-8 sequence at p (1..4), or 0 if invalid/truncated.
static size_t utf8_len(const unsigned char *p, size_t avail)
{
    const unsigned char c = p[0];
    size_t k;
    uint32_t cp;
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) { k = 2; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { k = 3; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { k = 4; cp = c & 0x07; }
    else return 0;
    if (k > avail) return 0;
    for (size_t i = 1; i < k; i++) {
        if ((p[i] & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    if ((k == 2 && cp < 0x80) || (k == 3 && cp < 0x800) || (k == 4 && cp < 0x10000) ||
        cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        return 0;                                         // overlong / out of range / surrogate
    return k;
}

size_t ha_payload_text(const char *p, size_t len, char *out, size_t n)
{
    if (!out || n == 0) return 0;
    size_t o = 0, i = 0;
    while (i < len && p[i] != '\0') {
        const unsigned char c = (unsigned char)p[i];
        size_t k = utf8_len((const unsigned char *)p + i, len - i);
        if (k == 0) {                                     // invalid byte
            if (o + 1 >= n) break;
            out[o++] = '?';
            i++;
            continue;
        }
        if (k == 1 && c < 0x20 && c != '\n') { i++; continue; }
        if (k == 1 && c == 0x7F) { i++; continue; }
        if (o + k >= n) break;
        memcpy(out + o, p + i, k);
        o += k;
        i += k;
    }
    while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '\n')) o--;
    out[o] = '\0';
    return o;
}
