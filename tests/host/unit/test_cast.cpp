// Unit tests for NucleoCast's LAN-facing parsers (components/nv_secondscreen/ss_parse.cpp): the HTTP
// request head as the listener accumulates it, WebSocket frame headers, the sender's "hi", the trust list.
#include "check.h"
#include "ss_parse.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Feed `s` to a head accumulator in chunks of `step` (as recv() would hand them); true once complete.
static bool feed(SsHttpHead &h, const std::string &s, size_t step, size_t *chunks = nullptr) {
    size_t n = 0;
    for (size_t o = 0; o < s.size() && h.room_len(); o += step) {
        size_t k = s.size() - o < step ? s.size() - o : step;
        if (k > h.room_len()) k = h.room_len();
        memcpy(h.room(), s.data() + o, k);
        n++;
        if (h.push(k)) { if (chunks) *chunks = n; return true; }
    }
    if (chunks) *chunks = n;
    return false;
}

static std::vector<uint8_t> ws_head(uint8_t b0, uint8_t b1, const std::vector<uint8_t> &ext, bool mask) {
    std::vector<uint8_t> v{b0, b1};
    v.insert(v.end(), ext.begin(), ext.end());
    if (mask) { const uint8_t k[4] = {1, 2, 3, 4}; v.insert(v.end(), k, k + 4); }
    return v;
}

int main() {
    // ---- a browser's upgrade to /cast, in one read
    const std::string up =
        "GET /cast?x=1 HTTP/1.1\r\nHost: 192.168.1.50:7070\r\nUser-Agent: Mozilla/5.0 Chrome/129\r\n"
        "Upgrade: websocket\r\nConnection: keep-alive, Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    {
        SsHttpHead h; SsHttpReq r;
        CHECK(feed(h, up, up.size()));
        CHECK(ss_http_parse_head(h, r));
        CHECK(!strcmp(r.method, "GET") && !strcmp(r.path, "/cast") && !strcmp(r.query, "x=1"));
        CHECK(!strcmp(r.host, "192.168.1.50:7070"));
        CHECK(!strcmp(r.ws_key, "dGhlIHNhbXBsZSBub25jZQ=="));
        CHECK(r.upgrade_ws);
    }
    // ---- the same head split across reads (3 bytes, then the blank line torn between two reads)
    {
        SsHttpHead h; SsHttpReq r; size_t chunks = 0;
        CHECK(feed(h, up, 3, &chunks));
        CHECK(chunks == (up.size() + 2) / 3);
        CHECK(ss_http_parse_head(h, r) && r.upgrade_ws);
        SsHttpHead h2;
        const std::string a = up.substr(0, up.size() - 3), b = up.substr(up.size() - 3);
        CHECK(!feed(h2, a, a.size()));
        CHECK(feed(h2, b, b.size()));
    }
    // ---- no Connection: Upgrade, or no key: a plain GET
    {
        SsHttpHead h; SsHttpReq r;
        CHECK(feed(h, "GET / HTTP/1.1\r\nUpgrade: websocket\r\nSec-WebSocket-Key: abc\r\n\r\n", 64));
        CHECK(ss_http_parse_head(h, r) && !r.upgrade_ws);
    }
    // ---- oversized head: never terminated, stops at the buffer, fields clamped, no overrun
    {
        SsHttpHead h; SsHttpReq r;
        std::string big = "GET /" + std::string(1000, 'p') + "?" + std::string(1000, 'q') + " HTTP/1.1\r\nUser-Agent: "
                        + std::string(3000, 'u');
        CHECK(feed(h, big, 512));
        CHECK(h.len == sizeof h.buf - 1 && h.room_len() == 0);
        CHECK(ss_http_parse_head(h, r));
        CHECK(strlen(r.path) == sizeof r.path - 1 && strlen(r.query) == sizeof r.query - 1);
        CHECK(strlen(r.ua) == sizeof r.ua - 1);
    }
    // ---- garbage: no spaces -> refused; a NUL hides the blank line, so it reads to the full buffer
    {
        SsHttpHead h; SsHttpReq r;
        CHECK(feed(h, "GARBAGE\r\n\r\n", 16));
        CHECK(!ss_http_parse_head(h, r));
        SsHttpHead h2;
        CHECK(!feed(h2, std::string("GET / HTTP/1.1\r\n\0\r\n\r\n", 21), 21));
    }

    // ---- WebSocket headers: short, 16-bit, 64-bit lengths; masked and unmasked
    {
        SsWsFrame f;
        auto v = ws_head(0x81, 0x85, {}, true);              // FIN text, masked, 5 bytes
        CHECK(ss_ws_header_len(v.data()) == 6 && v.size() == 6);
        CHECK(ss_ws_parse_header(v.data(), v.size(), f));
        CHECK(f.fin && f.op == 1 && f.masked && f.len == 5 && f.mask[0] == 1 && f.mask[3] == 4);

        SsWsFrame g;
        v = ws_head(0x02, 126, {0x01, 0x00}, false);         // binary, not final, unmasked, 256 bytes
        CHECK(ss_ws_header_len(v.data()) == 4);
        CHECK(ss_ws_parse_header(v.data(), v.size(), g));
        CHECK(!g.fin && g.op == 2 && !g.masked && g.len == 256);

        SsWsFrame k;
        v = ws_head(0x82, 0x80 | 127, {0, 0, 0, 0, 0, 0x08, 0, 0}, true);   // 512 KB tile, masked
        CHECK(ss_ws_header_len(v.data()) == 14);
        CHECK(ss_ws_parse_header(v.data(), v.size(), k));
        CHECK(k.len == 512u * 1024 && k.masked && k.mask[2] == 3);

        // a header shorter or longer than its first two bytes say is refused
        CHECK(!ss_ws_parse_header(v.data(), 13, k));
        CHECK(!ss_ws_parse_header(v.data(), 1, k));
    }
    // ---- length overflow: the top bit is illegal (RFC 6455), and >size_t cannot be read on 32 bits
    {
        SsWsFrame f;
        auto v = ws_head(0x80, 127, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF0}, false);
        CHECK(!ss_ws_parse_header(v.data(), v.size(), f));
        v = ws_head(0x80, 127, {0x80, 0, 0, 0, 0, 0, 0, 1}, false);
        CHECK(!ss_ws_parse_header(v.data(), v.size(), f));
        v = ws_head(0x80, 127, {0, 0, 0, 1, 0, 0, 0, 5}, false);            // 2^32 + 5
        CHECK(ss_ws_parse_header(v.data(), v.size(), f) == (sizeof(size_t) > 4));
        v = ws_head(0x80, 127, {0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, false);
        if (sizeof(size_t) > 4) CHECK(ss_ws_parse_header(v.data(), v.size(), f) && f.len == 0x7FFFFFFFFFFFFFFFull);
        else CHECK(!ss_ws_parse_header(v.data(), v.size(), f));
    }

    // ---- "hi"
    {
        SsCastHi h;
        CHECK(ss_cast_parse_hi("{\"t\":\"hi\",\"agent\":\"Laptop\",\"token\":\"0123456789abcdef\",\"touch\":true}", h));
        CHECK(h.touch && !strcmp(h.agent, "Laptop") && !strcmp(h.token, "0123456789abcdef"));
        SsCastHi l;
        const std::string longname(100, 'n');
        CHECK(ss_cast_parse_hi(("{\"t\":\"hi\",\"agent\":\"" + longname + "\",\"token\":\"0123456789abcdef0\"}").c_str(), l));
        CHECK(strlen(l.agent) == 30 && !l.token[0] && !l.touch);
        SsCastHi w;
        CHECK(ss_cast_parse_hi("{\"t\":\"hi\",\"agent\":7,\"token\":[1],\"touch\":1}", w));
        CHECK(!w.agent[0] && !w.token[0] && !w.touch);
        CHECK(!ss_cast_parse_hi("{\"t\":\"hi\"", w));
    }
    // ---- trust list: whole entries only, valid hex only
    {
        const char *list = "0123456789abcdef,fedcba9876543210";
        CHECK(ss_trust_has(list, "0123456789abcdef"));
        CHECK(ss_trust_has(list, "fedcba9876543210"));
        CHECK(!ss_trust_has(list, "89abcdef,fedcba9"));      // spans two entries
        CHECK(!ss_trust_has(list, "0123456789ABCDEF"));
        CHECK(!ss_trust_has(list, "0123456789abcde"));
        CHECK(!ss_trust_has("", "0123456789abcdef"));
        CHECK(!ss_trust_has(list, nullptr));
    }
    return TEST_DONE("cast");
}
