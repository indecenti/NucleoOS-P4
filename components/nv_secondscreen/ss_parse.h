// Pure parsers for what NucleoCast reads off the LAN before anyone is paired: the HTTP request
// head, WebSocket frame headers and the sender's "hi" message. No ESP-IDF, so tests/host can
// fuzz them on a PC (target "cast"); ss_net.cpp / ss_cast.cpp do the socket I/O around them.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ---- HTTP -----------------------------------------------------------------------------------
struct SsHttpReq {
    char method[8] = "";
    char path[160] = "";          // without the query
    char query[160] = "";
    char host[64] = "";
    char ua[160] = "";
    char ws_key[40] = "";         // Sec-WebSocket-Key (upgrade requests)
    bool upgrade_ws = false;
};

// Request-head accumulator: the socket reads into room(), push(n) accounts for the bytes and says
// whether the head is complete (blank line seen, or the buffer is full: what there is gets parsed).
struct SsHttpHead {
    char buf[4096];
    size_t len = 0;
    char *room() { return buf + len; }
    size_t room_len() const { return sizeof buf - 1 - len; }
    bool push(size_t n);
};
// Parse the accumulated head (in place). false = no request line.
bool ss_http_parse_head(SsHttpHead &h, SsHttpReq &r);

// ---- WebSocket ------------------------------------------------------------------------------
// Frame header parsed off the wire (payload not yet read).
struct SsWsFrame {
    bool fin = false;
    uint8_t op = 0;
    bool masked = false;
    uint8_t mask[4] = {};
    uint64_t len = 0;
};
constexpr size_t kSsWsHeaderMax = 14;
// Whole header size (2..14) given its first two bytes.
size_t ss_ws_header_len(const uint8_t h[2]);
// Parse a header of ss_ws_header_len(h) bytes. false = malformed or a length this device cannot hold.
bool ss_ws_parse_header(const uint8_t *h, size_t n, SsWsFrame &f);

// ---- NucleoCast "hi" ------------------------------------------------------------------------
struct SsCastHi {
    bool touch = false;
    char agent[31] = "";          // the sender's own name, at most 30 bytes ("" = none given)
    char token[17] = "";          // trust token, only when exactly 16 bytes (no other length matches)
};
// {"t":"hi","agent":"..","token":"..","touch":bool}. false = not JSON.
bool ss_cast_parse_hi(const char *json, SsCastHi &h);
// Is `tok` one whole entry of the comma-separated trust list? Constant time per entry.
bool ss_trust_has(const char *list, const char *tok);
