// libFuzzer target for nv_auth_core: header parsing, the persisted session list (parse/format round
// trip must be stable) and the PIN state machine driven by fuzzed operations (a pairing may succeed
// only with the code that is out at that moment).
#include "nv_auth_core.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace nv_auth_core;

static void headers(const uint8_t *d, size_t n) {
    std::string s(reinterpret_cast<const char *>(d), n);
    const size_t cut = s.find('\0');
    std::string a = s.substr(0, cut), c = cut == std::string::npos ? "" : s.substr(cut + 1);
    c = std::string(c.c_str());   // up to the next NUL, like a header value
    char t[kTokenHex + 1];
    if (token_from_headers(a.c_str(), c.c_str(), t)) {
        if (!is_token(t, strlen(t))) abort();
        if (a.find(t) == std::string::npos && c.find(t) == std::string::npos) abort();
    }
}

static bool same(const Session *x, const Session *y, int n) {
    for (int i = 0; i < n; i++)
        if (memcmp(x[i].hash, y[i].hash, kHashLen) || x[i].created != y[i].created || strcmp(x[i].name, y[i].name))
            return false;
    return true;
}

static void sessions(const uint8_t *d, size_t n) {
    std::string s(reinterpret_cast<const char *>(d), n);
    Session a[kMaxSessions], b[kMaxSessions];
    const int na = sessions_parse(s.c_str(), a, kMaxSessions);
    if (na < 0 || na > kMaxSessions) abort();
    char buf[4096];
    const size_t len = sessions_format(a, na, buf, sizeof buf);
    if (!len || len != strlen(buf)) abort();   // 12 entries always fit in 4 KB
    const int nb = sessions_parse(buf, b, kMaxSessions);
    if (nb != na || !same(a, b, na)) abort();
    // Edits keep the count in range.
    int m = na;
    if (m) m = sessions_remove(a, m, (int)(d[0] % (m + 1)));
    Session e = {};
    m = sessions_add(a, m, kMaxSessions, e);
    if (m < 1 || m > kMaxSessions) abort();
}

static void pin(const uint8_t *d, size_t n) {
    Pin p;
    pin_reset(p);
    int64_t t = 0;
    char out[kPinLen + 1] = "";   // the code currently out, per our own model
    for (size_t i = 0; i + 4 <= n; i += 4) {
        const uint32_t v = (uint32_t)d[i + 1] << 16 | (uint32_t)d[i + 2] << 8 | d[i + 3];
        switch (d[i] % 4) {
        case 0:
            if (pin_issue(p, t, v * 97u, "x")) snprintf(out, sizeof out, "%s", p.code);
            break;
        case 1: {
            char attempt[kPinLen + 1];
            snprintf(attempt, sizeof attempt, "%06u", v % 1000000u);
            if (d[i + 1] & 1) snprintf(attempt, sizeof attempt, "%s", out);   // sometimes the right one
            const bool right = out[0] && pin_active(p, t) && strcmp(attempt, out) == 0 && t >= p.locked_until_ms;
            const PinResult r = pin_try(p, t, attempt);
            if (r == PinResult::Ok && !right) abort();
            if (right && r != PinResult::Ok) abort();
            if (r == PinResult::Ok || r == PinResult::Locked) out[0] = '\0';
            break;
        }
        case 2: pin_deny(p, t); out[0] = '\0'; break;
        default: t += v; break;
        }
        if (p.locked_until_ms - t > kLockMaxMs + kDenyMs) abort();
        if (p.fails < 0 || p.fails >= kPinTries) abort();
    }
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    if (!n) return 0;
    switch (d[0] % 3) {
    case 0: headers(d + 1, n - 1); break;
    case 1: sessions(d + 1, n - 1); break;
    default: pin(d + 1, n - 1); break;
    }
    return 0;
}
