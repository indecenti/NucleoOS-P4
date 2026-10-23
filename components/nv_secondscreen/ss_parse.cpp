// Pure NucleoCast parsers (see ss_parse.h). Everything here sees bytes from the LAN unauthenticated.
#include "ss_parse.h"

#include "cJSON.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <strings.h>

// ---------------------------------------------------------------- HTTP
static bool header_is(const char *line, const char *name, const char **val) {
    const size_t n = strlen(name);
    if (strncasecmp(line, name, n) != 0 || line[n] != ':') return false;
    const char *v = line + n + 1;
    while (*v == ' ' || *v == '\t') v++;
    *val = v;
    return true;
}

static void copy_trim(char *dst, size_t n, const char *src) {
    size_t i = 0;
    while (src[i] && src[i] != '\r' && src[i] != '\n' && i + 1 < n) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

bool SsHttpHead::push(size_t n) {
    if (n > room_len()) n = room_len();
    len += n;
    buf[len] = 0;
    // Requests are tiny GETs; a NUL byte hides the terminator, so such a head runs to the full buffer.
    return strstr(buf, "\r\n\r\n") != nullptr || len >= sizeof buf - 1;
}

bool ss_http_parse_head(SsHttpHead &h, SsHttpReq &r) {
    char *buf = h.buf;
    buf[h.len] = 0;
    // request line
    char *sp1 = strchr(buf, ' ');
    if (!sp1) return false;
    *sp1 = 0;
    snprintf(r.method, sizeof r.method, "%s", buf);
    char *uri = sp1 + 1;
    char *sp2 = strchr(uri, ' ');
    if (!sp2) return false;
    *sp2 = 0;
    char *q = strchr(uri, '?');
    if (q) { *q = 0; snprintf(r.query, sizeof r.query, "%s", q + 1); }
    snprintf(r.path, sizeof r.path, "%s", uri);
    // headers; any body is ignored
    char *line = strstr(sp2 + 1, "\r\n");
    bool upgrade = false, conn_upgrade = false;
    while (line) {
        line += 2;
        if (*line == '\r' || !*line) break;
        const char *v;
        if (header_is(line, "Host", &v)) copy_trim(r.host, sizeof r.host, v);
        else if (header_is(line, "User-Agent", &v)) copy_trim(r.ua, sizeof r.ua, v);
        else if (header_is(line, "Sec-WebSocket-Key", &v)) copy_trim(r.ws_key, sizeof r.ws_key, v);
        else if (header_is(line, "Upgrade", &v)) upgrade = strncasecmp(v, "websocket", 9) == 0;
        else if (header_is(line, "Connection", &v)) {
            char tmp[64]; copy_trim(tmp, sizeof tmp, v);
            for (char *p = tmp; *p; p++) *p = (char)tolower((unsigned char)*p);
            conn_upgrade = strstr(tmp, "upgrade") != nullptr;
        }
        line = strstr(line, "\r\n");
    }
    r.upgrade_ws = upgrade && conn_upgrade && r.ws_key[0];
    return true;
}

// ---------------------------------------------------------------- WebSocket
size_t ss_ws_header_len(const uint8_t h[2]) {
    const uint8_t l = h[1] & 0x7F;
    return 2 + (l == 126 ? 2 : l == 127 ? 8 : 0) + ((h[1] & 0x80) ? 4 : 0);
}

bool ss_ws_parse_header(const uint8_t *h, size_t n, SsWsFrame &f) {
    if (n < 2 || n != ss_ws_header_len(h)) return false;
    f.fin = (h[0] & 0x80) != 0;
    f.op = h[0] & 0x0F;
    f.masked = (h[1] & 0x80) != 0;
    const uint8_t *p = h + 2;
    uint64_t len = h[1] & 0x7F;
    if (len == 126) {
        len = ((uint64_t)p[0] << 8) | p[1];
        p += 2;
    } else if (len == 127) {
        len = 0;
        for (int i = 0; i < 8; i++) len = (len << 8) | p[i];
        p += 8;
        // RFC 6455: the top bit must be 0. A length near 2^64 wrapped the session's "message so far +
        // this frame" checks (heap overflow); one past size_t was cut short by the 32-bit (size_t) casts.
        if ((len >> 63) || len > (uint64_t)SIZE_MAX) return false;
    }
    f.len = len;
    if (f.masked) memcpy(f.mask, p, 4);
    return true;
}

// ---------------------------------------------------------------- NucleoCast "hi"
bool ss_cast_parse_hi(const char *json, SsCastHi &h) {
    cJSON *j = cJSON_Parse(json);
    if (!j) return false;
    const cJSON *tok = cJSON_GetObjectItem(j, "token");
    const cJSON *agent = cJSON_GetObjectItem(j, "agent");
    h.touch = cJSON_IsTrue(cJSON_GetObjectItem(j, "touch"));
    if (cJSON_IsString(agent) && agent->valuestring[0])
        snprintf(h.agent, sizeof h.agent, "%.30s", agent->valuestring);
    if (cJSON_IsString(tok) && strlen(tok->valuestring) == 16)
        snprintf(h.token, sizeof h.token, "%s", tok->valuestring);
    cJSON_Delete(j);
    return true;
}

// Whole entries only: a substring match accepted any 16 characters that happened to span two stored
// tokens (comma included).
bool ss_trust_has(const char *list, const char *tok) {
    if (!list || !tok || strlen(tok) != 16) return false;
    for (int i = 0; i < 16; i++)
        if (!((tok[i] >= '0' && tok[i] <= '9') || (tok[i] >= 'a' && tok[i] <= 'f'))) return false;
    bool hit = false;
    for (const char *p = list; *p;) {
        const char *c = strchr(p, ',');
        const size_t len = c ? (size_t)(c - p) : strlen(p);
        if (len == 16) {
            uint8_t d = 0;
            for (int i = 0; i < 16; i++) d |= (uint8_t)(p[i] ^ tok[i]);
            hit |= d == 0;
        }
        if (!c) break;
        p = c + 1;
    }
    return hit;
}
