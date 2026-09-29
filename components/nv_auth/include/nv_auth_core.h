// nv_auth_core — the pure logic behind web pairing: token parsing, the stored session list and the
// pairing-PIN state machine. No ESP-IDF here, so tests/host unit-tests and fuzzes it as-is; the device
// glue (NVS, RNG, SHA-256, locking) lives in nv_auth.cpp.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace nv_auth_core {

constexpr size_t  kTokenHex    = 32;        // 128-bit session token as lowercase hex
constexpr size_t  kHashLen     = 32;        // SHA-256 of the token: only this is stored
constexpr size_t  kNameMax     = 32;        // session label incl. NUL
constexpr size_t  kWhoMax      = 48;        // "who asked" label for the pairing card incl. NUL
constexpr int     kMaxSessions = 12;        // oldest is evicted when a 13th client pairs
constexpr size_t  kPinLen      = 6;
constexpr int     kPinTries    = 5;         // wrong codes before the PIN is burnt and pairing locks
constexpr int64_t kPinTtlMs    = 120000;    // a shown PIN is valid for 2 minutes
constexpr int64_t kLockBaseMs  = 60000;     // first lockout; doubles per consecutive lockout
constexpr int64_t kLockMaxMs   = 3600000;   // ... up to an hour, so guessing stays hopeless
constexpr int64_t kDenyMs      = 300000;    // "Cancel" on the device: no new prompt for 5 minutes

// Exactly kTokenHex lowercase hex digits (the first n bytes of s; no NUL needed).
bool is_token(const char *s, size_t n);

// The session token from an Authorization header value ("Bearer <token>") or, failing that, from the
// nv_s cookie in a Cookie header value ("a=1; nv_s=<token>"). Either argument may be null or empty.
bool token_from_headers(const char *authorization, const char *cookie, char out[kTokenHex + 1]);

// Constant-time equality (no early exit on the first differing byte).
bool ct_equal(const uint8_t *a, const uint8_t *b, size_t n);

struct Session {
    uint8_t  hash[kHashLen];
    uint32_t created;           // unix time of pairing, 0 when the clock wasn't set yet
    char     name[kNameMax];
};

// Printable ASCII only, without the list separators ; and , (replaced by '_'), trimmed, never empty.
void sanitize_name(const char *in, char out[kNameMax]);

// The persisted form: "1;" then one "<64 hex>,<created>,<name>;" per session. Parsing skips
// malformed entries and stops at `max`; format returns the length written, or 0 when `cap` is short.
int    sessions_parse(const char *s, Session *out, int max);
size_t sessions_format(const Session *s, int n, char *out, size_t cap);

// Index of the session with this hash, or -1. Compares against every entry.
int sessions_find(const Session *s, int n, const uint8_t hash[kHashLen]);
// Append `e` (evicting the oldest-created entry when n == max); returns the new count.
int sessions_add(Session *s, int n, int max, const Session &e);
// Remove entry `index` (ignored when out of range); returns the new count.
int sessions_remove(Session *s, int n, int index);

enum class PinResult { Ok, Wrong, Locked, NoPin };

struct Pin {
    char    code[kPinLen + 1];  // "" when no code is out
    int64_t expires_ms;
    int64_t locked_until_ms;    // no pairing (and no new code) before this
    int     fails;              // wrong tries against the current code
    int     lockouts;           // consecutive lockouts, reset by a successful pairing
    char    who[kWhoMax];
};

void pin_reset(Pin &p);
bool pin_active(const Pin &p, int64_t now_ms);
// Put out a new code built from `rnd` unless one is still valid or pairing is locked.
// Returns true only when a NEW code was issued.
bool pin_issue(Pin &p, int64_t now_ms, uint32_t rnd, const char *who);
PinResult pin_try(Pin &p, int64_t now_ms, const char *attempt);
// The device owner dismissed the prompt: burn the code and keep quiet for kDenyMs.
void pin_deny(Pin &p, int64_t now_ms);

}  // namespace nv_auth_core
