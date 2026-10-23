// libFuzzer target for NucleoCast (components/nv_secondscreen/ss_parse.cpp): what any host on the LAN
// can send to ports 7070/7443 before pairing - a request head in arbitrary recv() pieces, then a
// WebSocket stream whose frame headers, text messages ("hi") and the trust match the session runs on.
#include "ss_parse.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

static void check_str(const char *s, size_t cap) {
    if (strnlen(s, cap) >= cap) abort();
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    if (n < 1) return 0;
    // Request head, in pieces of 1..61 bytes like a slow or hostile sender's segments.
    const size_t step = 1 + d[0] % 61;
    SsHttpHead h;
    size_t off = 1;
    while (off < n && h.room_len()) {
        size_t k = n - off < step ? n - off : step;
        if (k > h.room_len()) k = h.room_len();
        memcpy(h.room(), d + off, k);
        off += k;
        if (h.push(k)) break;
    }
    if (h.len >= sizeof h.buf) abort();
    SsHttpReq r;
    if (ss_http_parse_head(h, r)) {
        check_str(r.method, sizeof r.method);
        check_str(r.path, sizeof r.path);
        check_str(r.query, sizeof r.query);
        check_str(r.host, sizeof r.host);
        check_str(r.ua, sizeof r.ua);
        check_str(r.ws_key, sizeof r.ws_key);
        if (r.upgrade_ws && !r.ws_key[0]) abort();
    }

    // The rest as the WebSocket stream, assembled into text messages like Session::run (2 KB cap).
    std::string text;
    uint8_t msg_op = 0;
    bool skip = false;
    while (n - off >= 2) {
        const size_t hl = ss_ws_header_len(d + off);
        if (hl < 2 || hl > kSsWsHeaderMax) abort();
        if (n - off < hl) break;
        SsWsFrame f;
        if (!ss_ws_parse_header(d + off, hl, f)) break;   // the session closes the link
        off += hl;
        if ((f.len >> 63) || f.len > (uint64_t)SIZE_MAX || f.op > 15) abort();
        const size_t take = f.len < n - off ? (size_t)f.len : n - off;
        if (f.op < 8) {
            if (f.op != 0) { msg_op = f.op; text.clear(); skip = false; }
            if (msg_op == 1) {
                // the session's own check: it cannot wrap only because the parser keeps f.len < 2^63
                if (text.size() + f.len >= 2047) skip = true;
                if (!skip)
                    for (size_t i = 0; i < take; i++) text.push_back((char)(d[off + i] ^ (f.masked ? f.mask[i & 3] : 0)));
                if (f.fin && !skip && take == f.len && strstr(text.c_str(), "\"hi\"")) {
                    SsCastHi hi;
                    if (ss_cast_parse_hi(text.c_str(), hi)) {
                        check_str(hi.agent, sizeof hi.agent);
                        const size_t tl = strnlen(hi.token, sizeof hi.token);
                        if (tl != 0 && tl != 16) abort();
                        (void)ss_trust_has("0123456789abcdef,fedcba9876543210", hi.token);
                    }
                }
            }
        }
        off += take;
    }

    // The input straight as a "hi" and as a stored trust list (both NUL-terminated).
    std::string s(reinterpret_cast<const char *>(d), n);
    SsCastHi hi;
    if (ss_cast_parse_hi(s.c_str(), hi)) check_str(hi.agent, sizeof hi.agent);
    (void)ss_trust_has(s.c_str(), n >= 16 ? std::string(s, n - 16).c_str() : "0123456789abcdef");
    return 0;
}
