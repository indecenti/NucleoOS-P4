// nv_net_policy — see header. Pure C, bounded loops, no allocation.
#include "nv_net_policy.h"

#include <string.h>

// Also refuses non-ASCII: a URL on the wire is ASCII (percent-encode anything else).
static bool ctl_or_space(unsigned char c) { return c <= 0x20 || c >= 0x7f; }

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

bool np_url_parse(const char *url, np_url_t *out)
{
    if (!url || !out) return false;
    memset(out, 0, sizeof *out);
    const size_t n = strnlen(url, 512);
    if (n == 0 || n >= 512) return false;
    for (size_t i = 0; i < n; i++)
        if (ctl_or_space((unsigned char)url[i])) return false;

    static const struct { const char *p; np_scheme_t s; uint16_t port; } kSch[] = {
        {"http://", NP_HTTP, 80}, {"https://", NP_HTTPS, 443}, {"ws://", NP_WS, 80}, {"wss://", NP_WSS, 443}};
    size_t i = 0;
    bool found = false;
    for (size_t k = 0; k < sizeof kSch / sizeof kSch[0]; k++) {
        const size_t pl = strlen(kSch[k].p);
        size_t j = 0;
        while (j < pl && j < n && lower(url[j]) == kSch[k].p[j]) j++;
        if (j == pl) { out->scheme = kSch[k].s; out->port = kSch[k].port; i = pl; found = true; break; }
    }
    if (!found) return false;

    // authority = host[:port], up to '/', '?' or '#'
    const size_t a0 = i;
    while (i < n && url[i] != '/' && url[i] != '?' && url[i] != '#') i++;
    const size_t a1 = i;
    size_t hend = a1;
    for (size_t k = a0; k < a1; k++) {
        if (url[k] == '@' || url[k] == '[' || url[k] == ']') return false;   // userinfo / IPv6
        if (url[k] == ':') { hend = k; break; }
    }
    const size_t hl = hend - a0;
    if (hl == 0 || hl >= sizeof out->host) return false;
    for (size_t k = 0; k < hl; k++) {
        const char c = lower(url[a0 + k]);
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.')) return false;
        out->host[k] = c;
    }
    if (out->host[0] == '.' || out->host[0] == '-' || strstr(out->host, "..")) return false;
    if (hend < a1) {                                          // ":port"
        size_t k = hend + 1;
        if (k == a1 || a1 - k > 5) return false;
        uint32_t p = 0;
        for (; k < a1; k++) {
            if (url[k] < '0' || url[k] > '9') return false;
            p = p * 10 + (uint32_t)(url[k] - '0');
        }
        if (p == 0 || p > 65535) return false;
        out->port = (uint16_t)p;
    }
    // path (+query); a fragment is never sent
    size_t pe = i;
    while (pe < n && url[pe] != '#') pe++;
    if (i == pe) { out->path[0] = '/'; return true; }
    size_t pl = pe - i;
    if (url[i] == '?') {                                      // "http://h?x" -> "/?x"
        if (pl + 1 >= sizeof out->path) return false;
        out->path[0] = '/';
        memcpy(out->path + 1, url + i, pl);
        return true;
    }
    if (pl >= sizeof out->path) return false;
    memcpy(out->path, url + i, pl);
    return true;
}

bool np_parse_ipv4(const char *s, uint32_t *out)
{
    if (!s) return false;
    uint32_t ip = 0;
    int parts = 0;
    const char *p = s;
    while (parts < 4) {
        if (*p < '0' || *p > '9') return false;
        uint32_t v = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            if (++digits > 3) return false;
            v = v * 10 + (uint32_t)(*p - '0');
            p++;
        }
        if (v > 255 || (digits > 1 && p[-digits] == '0')) return false;   // no octal-looking 010
        ip = (ip << 8) | v;
        parts++;
        if (parts < 4) {
            if (*p != '.') return false;
            p++;
        }
    }
    if (*p != '\0') return false;
    if (out) *out = ip;
    return true;
}

bool np_ip_is_private(uint32_t ip)
{
    const uint32_t a = ip >> 24, b = (ip >> 16) & 0xff;
    if (a == 0 || a == 10 || a == 127) return true;
    if (a == 100 && b >= 64 && b <= 127) return true;        // CGNAT
    if (a == 169 && b == 254) return true;                    // link-local
    if (a == 172 && b >= 16 && b <= 31) return true;
    if (a == 192 && b == 168) return true;
    if (a == 192 && b == 0 && ((ip >> 8) & 0xff) == 0) return true;   // 192.0.0.0/24
    if (a == 198 && (b == 18 || b == 19)) return true;       // benchmarking
    if (a >= 224) return true;                                // multicast, reserved, broadcast
    return false;
}

static bool ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (lower(*a) != lower(*b)) return false;
    return *a == *b;
}

bool np_header_ok(const char *name, const char *value)
{
    if (!name || !value) return false;
    const size_t nl = strnlen(name, 65), vl = strnlen(value, 1025);
    if (nl == 0 || nl > 64 || vl > 1024) return false;
    for (size_t i = 0; i < nl; i++) {
        const char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return false;
    }
    for (size_t i = 0; i < vl; i++) {
        const unsigned char c = (unsigned char)value[i];
        if ((c < 0x20 && c != '\t') || c == 0x7f) return false;          // CR/LF: header injection
    }
    static const char *const kHost[] = {"host", "content-length", "transfer-encoding", "connection",
                                        "upgrade", "te", "trailer", "proxy-authorization"};
    for (size_t i = 0; i < sizeof kHost / sizeof kHost[0]; i++)
        if (ieq(name, kHost[i])) return false;
    if (nl >= 14) {
        char pre[15];
        memcpy(pre, name, 14);
        pre[14] = '\0';
        if (ieq(pre, "sec-websocket-")) return false;
    }
    return true;
}

bool np_ha_path_ok(const char *path)
{
    if (!path) return false;
    const size_t n = strnlen(path, 192);
    if (n < 5 || n >= 192 || strncmp(path, "/api/", 5) != 0) return false;
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)path[i];
        if (ctl_or_space(c) || c == '\\' || c == '#') return false;
        if (c == '%') {                                                    // no encoded . / \\ ?
            if (i + 2 >= n) return false;
            const char h1 = lower((char)path[i + 1]), h2 = lower((char)path[i + 2]);
            if (h1 == '2' && (h2 == 'e' || h2 == 'f')) return false;
            if (h1 == '5' && h2 == 'c') return false;
        }
    }
    const char *q = memchr(path, '?', n) ? (const char *)memchr(path, '?', n) : path + n;
    for (const char *p = path; p + 1 < q; p++) {
        if (p[0] == '/' && p[1] == '/') return false;
        if (p[0] == '/' && p[1] == '.' && (p + 2 == q || p[2] == '/' || (p[2] == '.' && (p + 3 == q || p[3] == '/'))))
            return false;
    }
    return true;
}
