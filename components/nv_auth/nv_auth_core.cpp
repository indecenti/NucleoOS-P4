// nv_auth_core — see nv_auth_core.h. Pure C++, no allocation, no ESP-IDF.
#include "nv_auth_core.h"

#include <cstdio>
#include <cstring>

namespace nv_auth_core {

namespace {

bool is_hex_lower(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }
int  hex_val(char c) { return c <= '9' ? c - '0' : c - 'a' + 10; }
bool is_space(char c) { return c == ' ' || c == '\t'; }

// [p, e) with surrounding blanks trimmed; true when what is left is a token (copied to out).
bool take_token(const char *p, const char *e, char out[kTokenHex + 1]) {
    while (p < e && is_space(*p)) p++;
    while (e > p && is_space(e[-1])) e--;
    if (!is_token(p, (size_t)(e - p))) return false;
    memcpy(out, p, kTokenHex);
    out[kTokenHex] = '\0';
    return true;
}

bool from_bearer(const char *v, char out[kTokenHex + 1]) {
    if (!v) return false;
    while (is_space(*v)) v++;
    static const char kBearer[] = "bearer";
    for (size_t i = 0; i < sizeof kBearer - 1; i++) {
        char c = v[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != kBearer[i]) return false;
    }
    v += sizeof kBearer - 1;
    if (!is_space(*v)) return false;
    return take_token(v, v + strlen(v), out);
}

bool from_cookie(const char *v, char out[kTokenHex + 1]) {
    if (!v) return false;
    const char *p = v;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ';') p++;
        const char *eq = p;
        while (*eq && *eq != '=' && *eq != ';') eq++;
        const char *end = eq;
        while (*end && *end != ';') end++;
        const char *ne = eq;
        while (ne > p && is_space(ne[-1])) ne--;
        if (*eq == '=' && ne - p == 4 && memcmp(p, "nv_s", 4) == 0 && take_token(eq + 1, end, out))
            return true;
        p = end;
    }
    return false;
}

bool name_char_ok(char c) { return c >= 0x20 && c <= 0x7e && c != ';' && c != ',' && c != '\\' && c != '"'; }

}  // namespace

bool is_token(const char *s, size_t n) {
    if (!s || n != kTokenHex) return false;
    for (size_t i = 0; i < n; i++)
        if (!is_hex_lower(s[i])) return false;
    return true;
}

bool token_from_headers(const char *authorization, const char *cookie, char out[kTokenHex + 1]) {
    return from_bearer(authorization, out) || from_cookie(cookie, out);
}

bool ct_equal(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

void sanitize_name(const char *in, char out[kNameMax]) {
    size_t n = 0;
    if (in) {
        while (*in == ' ') in++;
        for (; *in && n < kNameMax - 1; in++) out[n++] = name_char_ok(*in) ? *in : '_';
    }
    while (n && out[n - 1] == ' ') n--;
    if (!n) { memcpy(out, "client", 7); return; }
    out[n] = '\0';
}

int sessions_parse(const char *s, Session *out, int max) {
    if (!s || strncmp(s, "1;", 2) != 0 || max <= 0) return 0;
    int n = 0;
    const char *p = s + 2;
    while (*p && n < max) {
        const char *end = strchr(p, ';');
        if (!end) end = p + strlen(p);
        // <64 hex>,<created>,<name>
        const char *c1 = (const char *)memchr(p, ',', (size_t)(end - p));
        const char *c2 = c1 ? (const char *)memchr(c1 + 1, ',', (size_t)(end - c1 - 1)) : nullptr;
        Session e = {};
        bool ok = c1 && c2 && c1 - p == (long)(kHashLen * 2);
        for (size_t i = 0; ok && i < kHashLen; i++) {
            if (!is_hex_lower(p[2 * i]) || !is_hex_lower(p[2 * i + 1])) ok = false;
            else e.hash[i] = (uint8_t)(hex_val(p[2 * i]) << 4 | hex_val(p[2 * i + 1]));
        }
        if (ok) {
            uint64_t v = 0;
            const char *d = c1 + 1;
            if (d == c2 || c2 - d > 10) ok = false;
            for (; ok && d < c2; d++) {
                if (*d < '0' || *d > '9') ok = false;
                else v = v * 10 + (uint64_t)(*d - '0');
            }
            if (v > 0xffffffffull) ok = false;
            e.created = (uint32_t)v;
        }
        if (ok) {
            char raw[kNameMax];
            const size_t len = (size_t)(end - c2 - 1) < kNameMax - 1 ? (size_t)(end - c2 - 1) : kNameMax - 1;
            memcpy(raw, c2 + 1, len);
            raw[len] = '\0';
            sanitize_name(raw, e.name);
            out[n++] = e;
        }
        p = *end ? end + 1 : end;
    }
    return n;
}

size_t sessions_format(const Session *s, int n, char *out, size_t cap) {
    if (!out || cap < 3) return 0;
    size_t len = 0;
    out[len++] = '1';
    out[len++] = ';';
    static const char kHex[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        char name[kNameMax];
        sanitize_name(s[i].name, name);
        char tail[16 + kNameMax];
        const int tl = snprintf(tail, sizeof tail, ",%lu,%s;", (unsigned long)s[i].created, name);
        if (tl < 0 || len + kHashLen * 2 + (size_t)tl + 1 > cap) return 0;
        for (size_t k = 0; k < kHashLen; k++) {
            out[len++] = kHex[s[i].hash[k] >> 4];
            out[len++] = kHex[s[i].hash[k] & 15];
        }
        memcpy(out + len, tail, (size_t)tl);
        len += (size_t)tl;
    }
    out[len] = '\0';
    return len;
}

int sessions_find(const Session *s, int n, const uint8_t hash[kHashLen]) {
    int found = -1;
    for (int i = 0; i < n; i++) {
        const bool eq = ct_equal(s[i].hash, hash, kHashLen);
        if (eq && found < 0) found = i;
    }
    return found;
}

int sessions_add(Session *s, int n, int max, const Session &e) {
    if (max <= 0) return 0;
    if (n >= max) {
        int oldest = 0;
        for (int i = 1; i < n; i++)
            if (s[i].created < s[oldest].created) oldest = i;
        n = sessions_remove(s, n, oldest);
    }
    s[n] = e;
    sanitize_name(e.name, s[n].name);
    return n + 1;
}

int sessions_remove(Session *s, int n, int index) {
    if (index < 0 || index >= n) return n;
    for (int i = index; i + 1 < n; i++) s[i] = s[i + 1];
    return n - 1;
}

void pin_reset(Pin &p) { memset(&p, 0, sizeof p); }

bool pin_active(const Pin &p, int64_t now_ms) { return p.code[0] && now_ms < p.expires_ms; }

bool pin_issue(Pin &p, int64_t now_ms, uint32_t rnd, const char *who) {
    if (now_ms < p.locked_until_ms || pin_active(p, now_ms)) return false;
    snprintf(p.code, sizeof p.code, "%06lu", (unsigned long)(rnd % 1000000u));
    p.expires_ms = now_ms + kPinTtlMs;
    p.fails = 0;
    snprintf(p.who, sizeof p.who, "%s", who ? who : "");
    return true;
}

PinResult pin_try(Pin &p, int64_t now_ms, const char *attempt) {
    if (now_ms < p.locked_until_ms) return PinResult::Locked;
    if (!pin_active(p, now_ms)) return PinResult::NoPin;
    bool shape = attempt && strlen(attempt) == kPinLen;
    for (size_t i = 0; shape && i < kPinLen; i++)
        if (attempt[i] < '0' || attempt[i] > '9') shape = false;
    if (shape && ct_equal((const uint8_t *)attempt, (const uint8_t *)p.code, kPinLen)) {
        p.code[0] = '\0';
        p.fails = 0;
        p.lockouts = 0;
        return PinResult::Ok;
    }
    if (++p.fails < kPinTries) return PinResult::Wrong;
    int64_t lock = kLockBaseMs;
    for (int i = 0; i < p.lockouts && lock < kLockMaxMs; i++) lock *= 2;
    if (lock > kLockMaxMs) lock = kLockMaxMs;
    p.locked_until_ms = now_ms + lock;
    p.lockouts++;
    p.code[0] = '\0';
    p.fails = 0;
    return PinResult::Locked;
}

void pin_deny(Pin &p, int64_t now_ms) {
    p.code[0] = '\0';
    p.fails = 0;
    if (p.locked_until_ms < now_ms + kDenyMs) p.locked_until_ms = now_ms + kDenyMs;
}

}  // namespace nv_auth_core
