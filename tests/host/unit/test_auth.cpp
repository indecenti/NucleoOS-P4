// Unit tests for nv_auth_core: token parsing from headers, the stored session list and the pairing
// PIN state machine (lockouts, expiry, deny).
#include "check.h"
#include "nv_auth_core.h"

#include <cstring>
#include <string>

using namespace nv_auth_core;

static const char *kTok = "0123456789abcdef0123456789abcdef";

static bool tok(const char *authz, const char *cookie, std::string *out = nullptr) {
    char t[kTokenHex + 1];
    const bool ok = token_from_headers(authz, cookie, t);
    if (ok && out) *out = t;
    return ok;
}

static void test_tokens() {
    CHECK(is_token(kTok, 32));
    CHECK(!is_token(kTok, 31));
    CHECK(!is_token("0123456789ABCDEF0123456789abcdef", 32));   // lowercase only
    CHECK(!is_token(nullptr, 32));

    std::string t;
    CHECK(tok("Bearer 0123456789abcdef0123456789abcdef", nullptr, &t) && t == kTok);
    CHECK(tok("  bearer\t0123456789abcdef0123456789abcdef  ", nullptr));
    CHECK(tok("BEARER 0123456789abcdef0123456789abcdef", ""));
    CHECK(!tok("Bearer", nullptr));
    CHECK(!tok("Bearer0123456789abcdef0123456789abcdef", nullptr));    // no separator
    CHECK(!tok("Basic 0123456789abcdef0123456789abcdef", nullptr));
    CHECK(!tok("Bearer 0123456789abcdef0123456789abcdef0", nullptr));  // 33 digits
    CHECK(!tok("Bearer 0123456789abcdef 0123456789abcdef", nullptr));

    CHECK(tok(nullptr, "nv_s=0123456789abcdef0123456789abcdef", &t) && t == kTok);
    CHECK(tok("", "a=1; nv_s=0123456789abcdef0123456789abcdef; b=2"));
    CHECK(tok("", "a=1;nv_s = 0123456789abcdef0123456789abcdef"));
    CHECK(!tok("", "xnv_s=0123456789abcdef0123456789abcdef"));
    CHECK(!tok("", "nv_sx=0123456789abcdef0123456789abcdef"));
    CHECK(!tok("", "nv_s=0123456789abcdef0123456789abcdefX"));
    CHECK(!tok("", "a=nv_s=0123456789abcdef0123456789abcdef"));
    CHECK(tok("", "nv_s=bad; nv_s=0123456789abcdef0123456789abcdef"));   // first valid one wins
    CHECK(!tok(nullptr, nullptr));
    CHECK(!tok("", ";;;=;"));

    // A bad Authorization header does not hide a good cookie.
    CHECK(tok("Bearer nope", "nv_s=0123456789abcdef0123456789abcdef"));

    const uint8_t a[4] = {1, 2, 3, 4}, b[4] = {1, 2, 3, 5};
    CHECK(ct_equal(a, a, 4));
    CHECK(!ct_equal(a, b, 4));
    CHECK(ct_equal(a, b, 3));
}

static void test_names() {
    char n[kNameMax];
    sanitize_name("a;b,c\\d\"e", n);
    CHECK(strcmp(n, "a_b_c_d_e") == 0);
    sanitize_name("", n);
    CHECK(strcmp(n, "client") == 0);
    sanitize_name(nullptr, n);
    CHECK(strcmp(n, "client") == 0);
    sanitize_name("   ", n);
    CHECK(strcmp(n, "client") == 0);
    sanitize_name("  pc tool  ", n);
    CHECK(strcmp(n, "pc tool") == 0);
    sanitize_name("caf\xc3\xa9", n);
    CHECK(strcmp(n, "caf__") == 0);
    sanitize_name("0123456789012345678901234567890123456789", n);
    CHECK(strlen(n) == kNameMax - 1);
}

static Session mk(uint8_t fill, uint32_t created, const char *name) {
    Session s = {};
    memset(s.hash, fill, sizeof s.hash);
    s.created = created;
    snprintf(s.name, sizeof s.name, "%s", name);
    return s;
}

static void test_sessions() {
    Session list[kMaxSessions];
    int n = 0;
    n = sessions_add(list, n, kMaxSessions, mk(0x11, 100, "one"));
    n = sessions_add(list, n, kMaxSessions, mk(0xab, 0, "two;bad"));
    CHECK(n == 2);
    CHECK(strcmp(list[1].name, "two_bad") == 0);

    char buf[2048];
    const size_t len = sessions_format(list, n, buf, sizeof buf);
    CHECK(len == strlen(buf));
    CHECK(strncmp(buf, "1;1111", 6) == 0);
    Session back[kMaxSessions];
    CHECK(sessions_parse(buf, back, kMaxSessions) == 2);
    CHECK(memcmp(back[0].hash, list[0].hash, kHashLen) == 0 && back[0].created == 100);
    CHECK(strcmp(back[1].name, "two_bad") == 0 && back[1].created == 0);
    CHECK(sessions_format(list, n, buf, 50) == 0);   // does not fit
    CHECK(sessions_parse(buf, back, 1) == 1);        // max respected

    // Malformed entries are skipped, good ones kept.
    std::string s = "1;";
    s += std::string(64, 'z') + ",1,x;";                    // not hex
    s += std::string(63, 'a') + ",1,x;";                    // short hash
    s += std::string(64, 'a') + ",4294967296,x;";           // created overflows u32
    s += std::string(64, 'a') + ",,x;";                     // empty created
    s += std::string(64, 'A') + ",1,x;";                    // uppercase hex
    s += std::string(64, 'b') + ",4294967295,ok;";          // the one good entry
    s += "garbage";
    CHECK(sessions_parse(s.c_str(), back, kMaxSessions) == 1);
    CHECK(back[0].created == 4294967295u && strcmp(back[0].name, "ok") == 0);
    CHECK(sessions_parse("", back, kMaxSessions) == 0);
    CHECK(sessions_parse("2;", back, kMaxSessions) == 0);
    CHECK(sessions_parse(nullptr, back, kMaxSessions) == 0);

    uint8_t h[kHashLen];
    memset(h, 0xab, sizeof h);
    CHECK(sessions_find(list, n, h) == 1);
    memset(h, 0x12, sizeof h);
    CHECK(sessions_find(list, n, h) == -1);

    // Full list: the oldest-created entry makes room.
    n = 0;
    for (int i = 0; i < kMaxSessions; i++) n = sessions_add(list, n, kMaxSessions, mk((uint8_t)i, 1000u + i, "c"));
    CHECK(n == kMaxSessions);
    n = sessions_add(list, n, kMaxSessions, mk(0xee, 5000, "new"));
    CHECK(n == kMaxSessions);
    memset(h, 0, sizeof h);
    CHECK(sessions_find(list, n, h) == -1);            // created 1000 evicted
    memset(h, 0xee, sizeof h);
    CHECK(sessions_find(list, n, h) == kMaxSessions - 1);

    CHECK(sessions_remove(list, n, -1) == n);
    CHECK(sessions_remove(list, n, n) == n);
    n = sessions_remove(list, n, 0);
    CHECK(n == kMaxSessions - 1);
    memset(h, 1, sizeof h);
    CHECK(sessions_find(list, n, h) == -1);
}

static void test_pin() {
    Pin p;
    pin_reset(p);
    int64_t t = 1000;
    CHECK(pin_try(p, t, "000000") == PinResult::NoPin);
    CHECK(pin_issue(p, t, 42, "10.0.0.2"));
    CHECK(strcmp(p.code, "000042") == 0 && strcmp(p.who, "10.0.0.2") == 0);
    CHECK(!pin_issue(p, t + 10, 7, "x"));              // still valid: same code stays out
    CHECK(strcmp(p.code, "000042") == 0);
    CHECK(pin_active(p, t + kPinTtlMs - 1) && !pin_active(p, t + kPinTtlMs));

    CHECK(pin_try(p, t, "000042") == PinResult::Ok);
    CHECK(!pin_active(p, t));                          // single use
    CHECK(pin_try(p, t, "000042") == PinResult::NoPin);

    // Expiry.
    CHECK(pin_issue(p, t, 999999, ""));
    CHECK(pin_try(p, t + kPinTtlMs, "999999") == PinResult::NoPin);

    // Wrong codes: kPinTries-1 Wrong, then Locked; the code is burnt and nothing pairs meanwhile.
    t = 1000000;
    CHECK(pin_issue(p, t, 123456, ""));
    for (int i = 0; i < kPinTries - 1; i++) CHECK(pin_try(p, t, "000000") == PinResult::Wrong);
    CHECK(pin_try(p, t, "12345") == PinResult::Locked);      // malformed counts as wrong
    CHECK(pin_try(p, t + 1, "123456") == PinResult::Locked);
    CHECK(!pin_issue(p, t + kLockBaseMs - 1, 1, ""));
    CHECK(pin_issue(p, t + kLockBaseMs, 111111, ""));

    // The second consecutive lockout lasts twice as long.
    t += kLockBaseMs;
    for (int i = 0; i < kPinTries; i++) pin_try(p, t, nullptr);
    CHECK(!pin_issue(p, t + 2 * kLockBaseMs - 1, 1, ""));
    CHECK(pin_issue(p, t + 2 * kLockBaseMs, 222222, ""));
    // A success resets the escalation.
    CHECK(pin_try(p, t + 2 * kLockBaseMs, "222222") == PinResult::Ok);
    CHECK(p.lockouts == 0);

    // Escalation is capped.
    pin_reset(p);
    t = 0;
    for (int round = 0; round < 20; round++) {
        CHECK(pin_issue(p, t, 5, ""));
        for (int i = 0; i < kPinTries; i++) pin_try(p, t, "999999");
        CHECK(p.locked_until_ms - t <= kLockMaxMs);
        t = p.locked_until_ms;
    }

    // Deny: the owner cancelled, no new prompt for kDenyMs.
    pin_reset(p);
    CHECK(pin_issue(p, 0, 1, ""));
    pin_deny(p, 10);
    CHECK(!pin_active(p, 10));
    CHECK(!pin_issue(p, 10 + kDenyMs - 1, 1, ""));
    CHECK(pin_issue(p, 10 + kDenyMs, 1, ""));
}

int main() {
    test_tokens();
    test_names();
    test_sessions();
    test_pin();
    return TEST_DONE("auth");
}
