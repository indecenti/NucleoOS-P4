// Unit tests for dmd_core (the DMD app): the WebSocket upgrade and frame parser as dmdext's
// websocket-sharp client drives them (masked frames, messages split every 1016 bytes), the dmdext
// networkstream messages, and the dot geometry / mask / renderer.
#include "check.h"
#include "dmd_core.h"
#include "miniz.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// A client frame as websocket-sharp sends it: FIN as given, masked with a fixed key.
static std::vector<uint8_t> frame(uint8_t op, const std::vector<uint8_t> &pl, bool fin = true) {
    std::vector<uint8_t> f;
    f.push_back((uint8_t)((fin ? 0x80 : 0) | op));
    const size_t n = pl.size();
    if (n < 126) f.push_back((uint8_t)(0x80 | n));
    else if (n <= 0xFFFF) { f.push_back(0x80 | 126); f.push_back((uint8_t)(n >> 8)); f.push_back((uint8_t)n); }
    else { f.push_back(0x80 | 127); for (int i = 7; i >= 0; i--) f.push_back((uint8_t)((uint64_t)n >> (8 * i))); }
    const uint8_t mk[4] = {0x37, 0xFA, 0x21, 0x3D};
    f.insert(f.end(), mk, mk + 4);
    for (size_t i = 0; i < n; i++) f.push_back(pl[i] ^ mk[i & 3]);
    return f;
}

// A message split like websocket-sharp does (FragmentLength 1016): first frame, continuations, final.
static std::vector<uint8_t> fragmented(const std::vector<uint8_t> &msg, size_t frag = 1016) {
    std::vector<uint8_t> out;
    if (msg.size() <= frag) return frame(0x2, msg);
    for (size_t o = 0; o < msg.size(); o += frag) {
        const size_t n = std::min(frag, msg.size() - o);
        const std::vector<uint8_t> part(msg.begin() + (long)o, msg.begin() + (long)(o + n));
        const auto f = frame(o == 0 ? 0x2 : 0x0, part, o + n == msg.size());
        out.insert(out.end(), f.begin(), f.end());
    }
    return out;
}

static void put32(std::vector<uint8_t> &v, uint32_t x) {
    for (int i = 0; i < 4; i++) v.push_back((uint8_t)(x >> (8 * i)));
}
static std::vector<uint8_t> msg(const char *name) {
    std::vector<uint8_t> v(name, name + strlen(name));
    v.push_back(0);
    return v;
}

int main() {
    // ---- handshake: RFC 6455 sample key, then the request websocket-sharp sends
    char acc[29];
    dmd_ws_accept("dGhlIHNhbXBsZSBub25jZQ==", 24, acc);
    CHECK(std::string(acc) == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");

    const std::string req =
        "GET /dmd HTTP/1.1\r\nUser-Agent: websocket-sharp/1.0\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nHost: 10.0.0.5:9090\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    char resp[256];
    size_t used = 0, rl = 0;
    CHECK(dmd_ws_handshake(req.data(), req.size() - 3, &used, resp, sizeof resp, &rl) == DMD_HS_INCOMPLETE);
    const std::string withdata = req + "\x82\x80";
    CHECK(dmd_ws_handshake(withdata.data(), withdata.size(), &used, resp, sizeof resp, &rl) == DMD_HS_OK);
    CHECK(used == req.size());
    CHECK(std::string(resp, rl).find("101 Switching Protocols") != std::string::npos);
    CHECK(std::string(resp, rl).find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n") != std::string::npos);
    const std::string lc = "GET / HTTP/1.1\r\nupgrade: WebSocket\r\nsec-websocket-key:  abc \r\n\r\n";
    CHECK(dmd_ws_handshake(lc.data(), lc.size(), &used, resp, sizeof resp, &rl) == DMD_HS_OK);   // any case, any path
    const std::string plain = "GET /dmd HTTP/1.1\r\nHost: x\r\n\r\n";
    CHECK(dmd_ws_handshake(plain.data(), plain.size(), &used, resp, sizeof resp, &rl) == DMD_HS_BAD);
    CHECK(std::string(resp, rl).find("400") != std::string::npos);
    const std::string v8 = "GET / HTTP/1.1\r\nUpgrade: websocket\r\nSec-WebSocket-Key: a\r\nSec-WebSocket-Version: 8\r\n\r\n";
    CHECK(dmd_ws_handshake(v8.data(), v8.size(), &used, resp, sizeof resp, &rl) == DMD_HS_BAD);
    const std::string post = "POST / HTTP/1.1\r\nUpgrade: websocket\r\nSec-WebSocket-Key: a\r\n\r\n";
    CHECK(dmd_ws_handshake(post.data(), post.size(), &used, resp, sizeof resp, &rl) == DMD_HS_BAD);
    const std::string huge(5000, 'A');
    CHECK(dmd_ws_handshake(huge.data(), huge.size(), &used, resp, sizeof resp, &rl) == DMD_HS_BAD);

    // ---- frames
    std::vector<uint8_t> mbuf(70000);
    dmd_ws_t ws;
    dmd_ws_init(&ws, mbuf.data(), mbuf.size());
    {
        auto f = frame(0x2, {1, 2, 3});
        CHECK(dmd_ws_next(&ws, f.data(), 5, &used) == DMD_WS_MORE && used == 0);   // partial
        CHECK(dmd_ws_next(&ws, f.data(), f.size(), &used) == DMD_WS_MESSAGE && used == f.size());
        CHECK(ws.msg_len == 3 && ws.msg[0] == 1 && ws.msg[2] == 3);
    }
    {   // a 49 KB message (rgb24 256x64) in 1016-byte fragments, delivered once, intact
        std::vector<uint8_t> big(49160);
        for (size_t i = 0; i < big.size(); i++) big[i] = (uint8_t)(i * 7 + 3);
        auto s = fragmented(big);
        size_t off = 0, msgs = 0;
        for (;;) {
            const dmd_ws_ev_t ev = dmd_ws_next(&ws, s.data() + off, s.size() - off, &used);
            if (ev == DMD_WS_MORE) break;
            CHECK(ev == DMD_WS_MESSAGE || ev == DMD_WS_CONTROL);
            if (ev == DMD_WS_MESSAGE) { msgs++; CHECK(ws.msg_len == big.size() && !memcmp(ws.msg, big.data(), big.size())); }
            off += used;
        }
        CHECK(msgs == 1 && off == s.size());
    }
    {   // a ping in the middle of a fragmented message is answered, the message survives
        auto a = frame(0x2, {9, 8}, false), p = frame(0x9, {'h', 'i'}), b = frame(0x0, {7}, true);
        CHECK(dmd_ws_next(&ws, a.data(), a.size(), &used) == DMD_WS_CONTROL);
        CHECK(dmd_ws_next(&ws, p.data(), p.size(), &used) == DMD_WS_PING && ws.ctl_len == 2 && ws.ctl[0] == 'h');
        CHECK(dmd_ws_next(&ws, b.data(), b.size(), &used) == DMD_WS_MESSAGE && ws.msg_len == 3 && ws.msg[2] == 7);
    }
    {
        auto c = frame(0x8, {0x03, 0xE8});
        CHECK(dmd_ws_next(&ws, c.data(), c.size(), &used) == DMD_WS_CLOSE);
        auto pong = frame(0xA, {});
        CHECK(dmd_ws_next(&ws, pong.data(), pong.size(), &used) == DMD_WS_CONTROL);
    }
    {   // protocol errors
        uint8_t unmasked[] = {0x82, 0x01, 0x00};
        CHECK(dmd_ws_next(&ws, unmasked, sizeof unmasked, &used) == DMD_WS_ERROR);
        auto cont = frame(0x0, {1});
        CHECK(dmd_ws_next(&ws, cont.data(), cont.size(), &used) == DMD_WS_ERROR);
        auto rsv = frame(0x2, {1});
        rsv[0] |= 0x40;
        CHECK(dmd_ws_next(&ws, rsv.data(), rsv.size(), &used) == DMD_WS_ERROR);
        uint8_t hugelen[] = {0x82, 0xFF, 0, 0, 0, 1, 0, 0, 0, 0};   // 4 GB: refused before any wait
        CHECK(dmd_ws_next(&ws, hugelen, sizeof hugelen, &used) == DMD_WS_ERROR);
        std::vector<uint8_t> pl(126);
        auto bigping = frame(0x9, pl);
        CHECK(dmd_ws_next(&ws, bigping.data(), bigping.size(), &used) == DMD_WS_ERROR);
        dmd_ws_t small;
        uint8_t sb[8];
        dmd_ws_init(&small, sb, sizeof sb);
        auto a = frame(0x2, {1, 2, 3, 4, 5}, false), b = frame(0x0, {6, 7, 8, 9}, true);
        CHECK(dmd_ws_next(&small, a.data(), a.size(), &used) == DMD_WS_CONTROL);
        CHECK(dmd_ws_next(&small, b.data(), b.size(), &used) == DMD_WS_ERROR);   // 9 > 8 reassembled
        auto c = frame(0x2, {1}, false), d = frame(0x2, {2});
        dmd_ws_init(&small, sb, sizeof sb);
        CHECK(dmd_ws_next(&small, c.data(), c.size(), &used) == DMD_WS_CONTROL);
        CHECK(dmd_ws_next(&small, d.data(), d.size(), &used) == DMD_WS_ERROR);   // new message mid-fragment
    }
    {
        uint8_t h[10];
        CHECK(dmd_ws_header(0xA, 5, h) == 2 && h[0] == 0x8A && h[1] == 5);
        CHECK(dmd_ws_header(0x2, 300, h) == 4 && h[1] == 126 && h[2] == 1 && h[3] == 44);
        CHECK(dmd_ws_header(0x2, 70000, h) == 10 && h[1] == 127 && h[7] == 1 && h[8] == 0x11 && h[9] == 0x70);
    }

    // ---- dmdext messages
    dmd_state_t st;
    dmd_state_init(&st);
    std::vector<uint32_t> out(DMD_MAX_PX);
    auto feed = [&](const std::vector<uint8_t> &m) { return dmd_proto_feed(&st, m.data(), m.size(), out.data(), out.size()); };
    CHECK(st.color == 0xFF4500);
    {
        auto g = msg("gameName");
        const char *rom = "mm_109c";
        g.insert(g.end(), rom, rom + strlen(rom));   // no terminator, as dmdext sends it
        CHECK(feed(g) == DMD_MSG_META && std::string(st.game) == "mm_109c");
    }
    auto gray2 = msg("gray2Planes");
    put32(gray2, 1234);
    CHECK(feed(gray2) == DMD_MSG_BAD);              // no dimensions yet
    {
        auto d = msg("dimensions");
        put32(d, 16); put32(d, 2);
        CHECK(feed(d) == DMD_MSG_META && st.w == 16 && st.h == 2);
        auto bad = msg("dimensions");
        put32(bad, 0x10000); put32(bad, 0x10000);
        CHECK(feed(bad) == DMD_MSG_BAD && st.w == 0);
        CHECK(feed(d) == DMD_MSG_META);
        auto c = msg("color");
        put32(c, 0x00FF8000);                       // bytes 00 80 FF 00: 0x00RRGGBB little-endian
        CHECK(feed(c) == DMD_MSG_META && st.color == 0xFF8000);
    }
    {   // gray2 16x2: plane bytes 4 each. Pixel 0 (leftmost) = bit 0 of byte 0.
        auto f = msg("gray2Planes");
        put32(f, 99);
        const uint8_t p0[4] = {0x01, 0x80, 0x00, 0x00};   // pixel 0 and pixel 15 have bit 0
        const uint8_t p1[4] = {0x01, 0x00, 0x02, 0x00};   // pixel 0 and pixel 17 have bit 1
        f.insert(f.end(), p0, p0 + 4);
        f.insert(f.end(), p1, p1 + 4);
        CHECK(feed(f) == DMD_MSG_FRAME && st.last_fmt == DMD_FMT_GRAY2);
        CHECK(out[0] == 0xFF8000);                        // level 3: full colour
        CHECK(out[15] == 0x552A00);                       // level 1: a third
        CHECK(out[17] == 0xAA5500);                       // level 2: two thirds
        CHECK(out[1] == 0 && out[16] == 0);
        st.color_override = 0x00FF00;
        CHECK(feed(f) == DMD_MSG_FRAME && out[0] == 0x00FF00);
        st.color_override = -1;
        f.pop_back();
        CHECK(feed(f) == DMD_MSG_BAD);                    // a plane byte short
    }
    {   // palette of 4 applies to gray2; clearPalette goes back to the colour
        auto p = msg("palette");
        put32(p, 4);
        put32(p, 0x000000); put32(p, 0x0000FF); put32(p, 0x00FF00); put32(p, 0xFF0000);
        CHECK(feed(p) == DMD_MSG_META && st.pal_n == 4);
        auto f = msg("gray2Planes");
        put32(f, 1);
        const uint8_t planes[8] = {0x01, 0, 0, 0, 0x02, 0, 0, 0};   // px0 = level 1, px1 = level 2
        f.insert(f.end(), planes, planes + 8);
        CHECK(feed(f) == DMD_MSG_FRAME && out[0] == 0x0000FF && out[1] == 0x00FF00);
        CHECK(feed(msg("clearPalette")) == DMD_MSG_META && st.pal_n == 0);
        auto bad = msg("palette");
        put32(bad, 5);
        put32(bad, 1);
        CHECK(feed(bad) == DMD_MSG_BAD);                  // claims 5 colours, carries 1
    }
    {   // gray4 with a 16-colour palette and coloredGray4 with its own palette
        auto f = msg("coloredGray4");
        put32(f, 7);
        put32(f, 16);
        for (uint32_t i = 0; i < 16; i++) put32(f, i * 0x010101);
        const uint8_t planes[16] = {0x01, 0, 0, 0, 0x01, 0, 0, 0, 0x01, 0, 0, 0, 0x01, 0, 0, 0};   // px0 = 15
        f.insert(f.end(), planes, planes + 16);
        CHECK(feed(f) == DMD_MSG_FRAME && out[0] == 0x0F0F0F && out[1] == 0 && st.last_fmt == DMD_FMT_CGRAY4);
        auto g4 = msg("gray4Planes");
        put32(g4, 7);
        g4.insert(g4.end(), planes, planes + 16);
        CHECK(feed(g4) == DMD_MSG_FRAME && out[0] == 0xFF8000);   // level 15 of 15: full mono colour
    }
    {
        auto f = msg("rgb24");
        put32(f, 5);
        for (int i = 0; i < 32; i++) { f.push_back((uint8_t)i); f.push_back(0x80); f.push_back(0xFE); }
        CHECK(feed(f) == DMD_MSG_FRAME && out[0] == 0x0080FE && out[31] == 0x1F80FE);
        f.pop_back();
        CHECK(feed(f) == DMD_MSG_BAD);
    }
    CHECK(feed(msg("coloredGray6")) == DMD_MSG_IGNORED);
    CHECK(feed({'x', 'y'}) == DMD_MSG_BAD);
    CHECK(feed(msg("clearColor")) == DMD_MSG_META && st.color == 0xFF4500);
    {   // a 128x32 gray4 frame arriving fragmented end to end (2048 B of planes + header)
        auto d = msg("dimensions");
        put32(d, 128); put32(d, 32);
        CHECK(feed(d) == DMD_MSG_META);
        auto f = msg("gray4Planes");
        put32(f, 1);
        for (int k = 0; k < 4; k++) for (int i = 0; i < 512; i++) f.push_back(k == 3 ? 0xFF : 0);   // all level 8
        auto s = fragmented(f);
        dmd_ws_init(&ws, mbuf.data(), mbuf.size());
        size_t off = 0;
        int frames = 0;
        while (off < s.size()) {
            const dmd_ws_ev_t ev = dmd_ws_next(&ws, s.data() + off, s.size() - off, &used);
            if (ev == DMD_WS_MORE) break;
            if (ev == DMD_WS_MESSAGE && dmd_proto_feed(&st, ws.msg, ws.msg_len, out.data(), out.size()) == DMD_MSG_FRAME) frames++;
            off += used;
        }
        CHECK(frames == 1 && out[0] == 0x882400 && out[128 * 32 - 1] == 0x882400);   // 0xFF4500 * 8/15
    }

    // ---- geometry, square dots: the spec's table on the 1024x600 panel
    dmd_geom_t g;
    const int SQ = DMD_FIT_SQUARE, FILL = DMD_FIT_FILL;
    CHECK(dmd_geom(128, 32, 0, SQ, 1024, 600, &g) && g.pw == 8 && g.ph == 8 && g.gw == 1024 && g.gh == 256 && g.ox == 0 && g.oy == 172);
    CHECK(dmd_geom(128, 16, 0, SQ, 1024, 600, &g) && g.pw == 8 && g.gh == 128);
    CHECK(dmd_geom(192, 64, 0, SQ, 1024, 600, &g) && g.pw == 5 && g.gw == 960 && g.gh == 320 && g.ox == 32);
    CHECK(dmd_geom(256, 64, 0, SQ, 1024, 600, &g) && g.pw == 4 && g.gw == 1024 && g.gh == 256);
    CHECK(dmd_geom(128, 32, 1, SQ, 1024, 600, &g) && g.cols == 32 && g.rows == 128 && g.pw == 4 && g.gw == 128 && g.gh == 512);
    CHECK(!dmd_geom(0, 32, 0, SQ, 1024, 600, &g));
    CHECK(!dmd_geom(128, 32, 0, SQ, 100, 100, &g));     // wider than the panel: no dot size fits
    // ---- geometry, the whole panel: every pixel, cells differ by at most one pixel
    CHECK(dmd_geom(128, 32, 0, FILL, 1024, 600, &g) && g.gw == 1024 && g.gh == 600 && g.ox == 0 && g.oy == 0);
    CHECK(g.pw == 8 && g.ph == 18);
    {
        int tall = 0;
        bool ok = dmd_cell_y(&g, 0) == 0 && dmd_cell_y(&g, g.rows) == 600 && dmd_cell_x(&g, g.cols) == 1024;
        for (int r = 0; r < g.rows; r++) {
            const int hgt = dmd_cell_y(&g, r + 1) - dmd_cell_y(&g, r);
            ok = ok && (hgt == 18 || hgt == 19);
            tall += hgt == 19;
        }
        CHECK(ok && tall == 24);                         // 24 rows of 19 + 8 of 18 = 600
    }
    CHECK(dmd_geom(192, 64, 0, FILL, 1024, 600, &g) && g.pw == 5 && g.ph == 9 && dmd_cell_x(&g, 192) == 1024);
    CHECK(dmd_geom(128, 32, 1, FILL, 1024, 600, &g) && g.cols == 32 && g.rows == 128 && g.pw == 32 && g.ph == 4);
    CHECK(dmd_geom(4, 2, 0, FILL, 1024, 600, &g) && g.pw == 63 && g.gw == 252 && g.ox == 386);   // tiny: capped, centred

    // ---- masks
    uint8_t m8[64];
    dmd_mask_build(m8, 8, 8, 0, DMD_SHAPE_ROUND, false);
    // the centre: full coverage and the hot core (above 15), the corners dark
    CHECK(m8[3 * 8 + 3] > 15 && m8[4 * 8 + 4] > 15 && m8[0] == 0 && m8[7] == 0 && m8[63] == 0);
    CHECK(m8[3 * 8 + 3] < DMD_MASK_VALUES && m8[3 * 8 + 3] == m8[4 * 8 + 4] && m8[3 * 8 + 0] <= 15);   // core: centre only
    dmd_mask_build(m8, 8, 8, 0, DMD_SHAPE_ROUND, true);
    CHECK(m8[0] > 0 && m8[0] < 5 && m8[3 * 8 + 3] > 15);   // a faint halo in the corners
    dmd_mask_build(m8, 8, 8, 0, DMD_SHAPE_SQUARE, false);
    CHECK(m8[1 * 8 + 1] == 15 && m8[0] <= 1);   // the corner pixel grazes the square's edge
    uint8_t m2[4];
    dmd_mask_build(m2, 2, 2, 0, DMD_SHAPE_ROUND, false);
    CHECK(m2[0] == 15 && m2[3] == 15);
    {   // a tall cell holds the same round dot as a square one, centred (never an ellipse)
        uint8_t e[8 * 18], sq[64];
        dmd_mask_build(e, 8, 18, 0, DMD_SHAPE_ROUND, true);
        dmd_mask_build(sq, 8, 8, 0, DMD_SHAPE_ROUND, true);
        bool same = true;   // 18 rows: the 8x8 dot's rows land on rows 5..12, pixel for pixel
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) same = same && e[(y + 5) * 8 + x] == sq[y * 8 + x];
        CHECK(same);
        CHECK(e[0] == 0 && e[17 * 8 + 7] == 0 && e[1 * 8 + 3] == 0);   // well above / below the dot: dark
        uint8_t o[8 * 19];
        dmd_mask_build(o, 8, 19, 0, DMD_SHAPE_ROUND, false);          // odd height: centred on a half pixel
        CHECK(o[9 * 8 + 3] >= 15 && o[9 * 8 + 0] == o[9 * 8 + 7] && o[5 * 8 + 3] == o[13 * 8 + 3]);
        uint8_t e2[8 * 19];
        dmd_mask_build(e2, 8, 19, 6, DMD_SHAPE_ROUND, false);         // an explicit smaller dot
        CHECK(e2[9 * 8 + 3] >= 15 && e2[9 * 8 + 0] == 0);
    }

    // ---- render: a 2x1 DMD (red, green), every rotation, on an 8 x 2 band, square dots
    {
        const uint32_t px[2] = {0xFF0000, 0x00FF00};
        const uint16_t R = 0xF800, G = 0x07E0;
        static dmd_masks_t mk;
        uint16_t band[8 * 2];
        for (int rot = 0; rot < 4; rot++) {
            CHECK(dmd_geom(2, 1, rot, SQ, 8, 2, &g));
            CHECK(g.pw == (rot & 1 ? 1 : 2));
            dmd_masks_build(&mk, &g, DMD_SHAPE_ROUND, false);
            for (auto &v : band) v = 0x1234;
            dmd_render(band, 8, &g, &mk, px, 2, 1, rot, 100, true, 8);
            if (rot == 0) CHECK(band[3] == R && band[4] == G && band[0] == 0 && band[7] == 0);
            if (rot == 2) CHECK(band[3] == G && band[4] == R);
            if (rot == 1) CHECK(g.cols == 1 && g.rows == 2 && band[g.ox] == R && band[8 + g.ox] == G);
            if (rot == 3) CHECK(band[g.ox] == G && band[8 + g.ox] == R);
        }
        CHECK(dmd_geom(2, 1, 0, SQ, 8, 2, &g));
        dmd_masks_build(&mk, &g, DMD_SHAPE_ROUND, false);
        dmd_render(band, 8, &g, &mk, px, 2, 1, 0, 50, true, 8);
        CHECK(band[3] == (uint16_t)((((0xFF * 128) >> 8) >> 3) << 11));   // half brightness
    }
    {   // 128x32 square at 8 px fills the 1024-wide band; dot centres lit, gaps dark
        std::vector<uint32_t> px(128 * 32, 0xFFFFFF);
        CHECK(dmd_geom(128, 32, 0, SQ, 1024, 600, &g));
        static dmd_masks_t mk;
        dmd_masks_build(&mk, &g, DMD_SHAPE_ROUND, false);
        std::vector<uint16_t> fb((size_t)1024 * g.gh, 0x1234);
        dmd_render(fb.data(), 1024, &g, &mk, px.data(), 128, 32, 0, 100, true, 1024);
        CHECK(fb[4 * 1024 + 4] == 0xFFFF && fb[0] == 0 && fb[(size_t)(g.gh - 1) * 1024 + 1023] == 0);
        CHECK(fb[(size_t)(8 * 31 + 4) * 1024 + 8 * 127 + 4] == 0xFFFF);
    }
    {   // the whole panel: 600 rows drawn, every dot centre lit, nothing left of the old picture
        std::vector<uint32_t> px(128 * 32, 0xFFFFFF);
        CHECK(dmd_geom(128, 32, 0, FILL, 1024, 600, &g));
        static dmd_masks_t mk;
        dmd_masks_build(&mk, &g, DMD_SHAPE_ROUND, true);
        std::vector<uint16_t> fb((size_t)1024 * 600, 0x1234);
        dmd_render(fb.data(), 1024, &g, &mk, px.data(), 128, 32, 0, 100, true, 1024);
        bool centres = true, stale = false;
        for (int r = 0; r < 32; r++)
            for (int c = 0; c < 128; c++) {
                const int x = (dmd_cell_x(&g, c) + dmd_cell_x(&g, c + 1)) / 2, y = (dmd_cell_y(&g, r) + dmd_cell_y(&g, r + 1)) / 2;
                centres = centres && fb[(size_t)y * 1024 + x] == 0xFFFF;
            }
        for (uint16_t v : fb) stale = stale || v == 0x1234;
        CHECK(centres && !stale);
    }
    {   // strips of dot rows through the sprite cache == the whole band at once, both fits, every rotation
        for (int fit = 0; fit < 2; fit++)
            for (int rot = 0; rot < 4; rot++) {
                std::vector<uint32_t> px(192 * 64);
                for (size_t i = 0; i < px.size(); i++) px[i] = (uint32_t)(i * 2654435761u) & 0xFFFFFF;
                CHECK(dmd_geom(192, 64, rot, fit, 1024, 600, &g));
                static dmd_masks_t mk;
                dmd_masks_build(&mk, &g, DMD_SHAPE_ROUND, true);
                std::vector<uint16_t> whole((size_t)1024 * g.gh, 1), strips((size_t)1024 * g.gh, 2);
                dmd_render(whole.data(), 1024, &g, &mk, px.data(), 192, 64, rot, 77, true, 1024);
                static dmd_sprites_t sc;
                dmd_sprites_reset(&sc);
                for (int cy = 0; cy < g.rows; cy += 3)
                    dmd_render_rows(strips.data() + (size_t)dmd_cell_y(&g, cy) * 1024, 1024, &g, &mk, px.data(), 192, 64,
                                    rot, 77, true, 1024, cy, cy + 3, &sc);
                CHECK(whole == strips);   // the sprite cache draws the same pixels as the plain path
                std::fill(strips.begin(), strips.end(), 3);   // again with a warm, crowded cache
                dmd_render_rows(strips.data(), 1024, &g, &mk, px.data(), 192, 64, rot, 77, true, 1024, 0, g.rows, &sc);
                CHECK(whole == strips);
            }
        std::vector<uint16_t> one(1024 * 8, 7);
        CHECK(dmd_geom(128, 32, 0, SQ, 1024, 600, &g));
        static dmd_masks_t mk;
        dmd_masks_build(&mk, &g, DMD_SHAPE_ROUND, false);
        std::vector<uint32_t> px(128 * 32, 0);
        dmd_render_rows(one.data(), 1024, &g, &mk, px.data(), 128, 32, 0, 100, true, 1024, 40, 50, nullptr);   // past the end
        CHECK(one[0] == 7);
    }

    // ---- text: 5x7 glyphs, centred, largest integer scale
    {
        std::vector<uint32_t> t(128 * 32, 0x123456);
        CHECK(dmd_text(t.data(), 128, 32, "12:34", 0xFF4500) == 4);    // 29 x 7 -> x4 = 116 x 28
        CHECK(t[0] == 0);                                                 // cleared first
        // '1' row 0 = 00100: its lit column is x0 + 2*4, x0 = (128 - 116) / 2 = 6, y0 = (32 - 28) / 2 = 2
        CHECK(t[2 * 128 + 6 + 8] == 0xFF4500 && t[2 * 128 + 6] == 0);
        CHECK(dmd_text(t.data(), 128, 32, "CIAO\nNICOLA", 0xFFFFFF) == 2); // 35 x 15 -> x2 (the height limits)
        CHECK(dmd_text(t.data(), 128, 32, "", 1) == 0);
        CHECK(dmd_text(t.data(), 8, 4, "TOO LONG", 1) == 0);
        int lit = 0;
        dmd_text(t.data(), 128, 32, "a~b", 0x00FF00);                     // lower case = capitals, '~' = space
        for (uint32_t v : t) lit += v != 0;
        std::vector<uint32_t> u(128 * 32);
        int lit2 = 0;
        dmd_text(u.data(), 128, 32, "A B", 0x00FF00);
        for (uint32_t v : u) lit2 += v != 0;
        CHECK(lit == lit2 && lit > 0);
    }

    // ---- ZeDMD: the PC side as libzedmd 0.11.0 drives it (handshake, zone packets, padded chunks)
    {
        auto inflate = [](void *, const uint8_t *in, size_t n, uint8_t *out, size_t cap) -> int {
            mz_ulong ol = cap;
            return mz_uncompress(out, &ol, in, (mz_ulong)n) == MZ_OK ? (int)ol : -1;
        };
        std::vector<uint32_t> img(128 * 32);
        static dmd_zedmd_t z;
        dmd_zedmd_init(&z, 128, 32, 1920, img.data(), inflate, nullptr);
        uint8_t outb[512];
        size_t ol = 0;

        // handshake: 64 bytes, then 29 blocks of zeros; the reply is exactly 64 bytes
        std::vector<uint8_t> hs(64, 0);
        memcpy(hs.data(), "FRAMEZeDMD\x0c", 11);
        int ev = dmd_zedmd_feed(&z, hs.data(), hs.size(), outb, sizeof outb, &ol);
        std::vector<uint8_t> zeros(29 * 64, 0);
        ev |= dmd_zedmd_feed(&z, zeros.data(), zeros.size(), outb, sizeof outb, &ol);
        CHECK(ev == DMD_Z_EV_HELLO && ol == 64);
        CHECK(!memcmp(outb, "ZeDM", 4) && outb[57] == 'R' && outb[8] != 0 && !memcmp(outb + 58, "ZeDMDA", 6));
        CHECK(outb[4] + outb[5] * 256 == 128 && outb[6] + outb[7] * 256 == 32 && outb[11] + outb[12] * 256 == 1920);

        // a frame: zone 0 red, zone 17 (x 8..15, y 4..7) blue, zone 5 black; compressed packet + render
        std::vector<uint8_t> zp;
        auto zone = [&](uint8_t idx, uint16_t c) {
            zp.push_back(idx);
            for (int i = 0; i < 32; i++) { zp.push_back((uint8_t)c); zp.push_back((uint8_t)(c >> 8)); }
        };
        zone(0, 0xF800);
        zone(17, 0x001F);
        zp.push_back(5 + 128);
        mz_ulong cl = mz_compressBound((mz_ulong)zp.size());
        std::vector<uint8_t> comp(cl);
        CHECK(mz_compress(comp.data(), &cl, zp.data(), (mz_ulong)zp.size()) == MZ_OK);
        std::vector<uint8_t> xfer = {'F', 'R', 'A', 'M', 'E', 'Z', 'e', 'D', 'M', 'D', 0x05,
                                     (uint8_t)(cl >> 8), (uint8_t)cl, 1};
        xfer.insert(xfer.end(), comp.begin(), comp.begin() + (long)cl);
        const uint8_t render[] = {'Z', 'e', 'D', 'M', 'D', 0x06, 0, 0, 0};
        xfer.insert(xfer.end(), render, render + sizeof render);
        xfer.resize(1920, 0);                                       // one chunk, zero-padded
        ol = 0;
        ev = dmd_zedmd_feed(&z, xfer.data(), 700, outb, sizeof outb, &ol);   // any split
        CHECK(ol == 0);                                             // no ack before the chunk is complete
        ev |= dmd_zedmd_feed(&z, xfer.data() + 700, xfer.size() - 700, outb, sizeof outb, &ol);
        CHECK(ev == DMD_Z_EV_RENDER && ol == 6 && !memcmp(outb, "ZeDMDA", 6));
        CHECK(img[0] == 0xFF0000 && img[7 + 3 * 128] == 0xFF0000 && img[8] == 0);
        CHECK(img[4 * 128 + 8] == 0x0000FF && img[7 * 128 + 15] == 0x0000FF);

        // a transfer over 3 chunks (uncompressed zones, 4 x 65 bytes = 260 > 2 chunks of 128): one ack each
        dmd_zedmd_init(&z, 128, 32, 128, img.data(), inflate, nullptr);
        std::vector<uint8_t> raw;
        for (int k = 0; k < 4; k++) { raw.push_back((uint8_t)(k * 16)); for (int i = 0; i < 32; i++) { raw.push_back(0xE0); raw.push_back(0x07); } }
        std::vector<uint8_t> t2 = {'F', 'R', 'A', 'M', 'E', 'Z', 'e', 'D', 'M', 'D', 0x05,
                                   (uint8_t)(raw.size() >> 8), (uint8_t)raw.size(), 0};
        t2.insert(t2.end(), raw.begin(), raw.end());
        t2.insert(t2.end(), render, render + sizeof render);
        t2.resize((t2.size() + 127) / 128 * 128, 0);
        int acks = 0, renders = 0;
        for (size_t o = 0; o < t2.size(); o += 128) {
            ol = 0;
            renders += dmd_zedmd_feed(&z, t2.data() + o, 128, outb, sizeof outb, &ol) & DMD_Z_EV_RENDER;
            if (ol == 6 && !memcmp(outb, "ZeDMDA", 6)) acks++;
        }
        CHECK(acks == (int)(t2.size() / 128) && renders == 1);
        CHECK(img[0] == 0x00FF00 && img[3 * 128 + 112 + 7] == 0 && img[3 * 128 + 8 * 0] == 0x00FF00);
        CHECK(img[3 * 4 * 128 + 0] == 0x00FF00);                    // zone 48 = row 3, x 0
        CHECK(!dmd_zedmd_busy(&z));

        // clear, keep-alive, reset (the PC closes)
        auto cmd = [&](uint8_t c) {
            std::vector<uint8_t> t = {'F', 'R', 'A', 'M', 'E', 'Z', 'e', 'D', 'M', 'D', c, 0, 0, 0};
            t.resize(128, 0);
            ol = 0;
            return dmd_zedmd_feed(&z, t.data(), t.size(), outb, sizeof outb, &ol);
        };
        CHECK(cmd(0x0B) == 0 && ol == 6);
        CHECK(cmd(0x0A) == DMD_Z_EV_RENDER && ol == 6 && img[0] == 0);
        CHECK(cmd(0x1F) == DMD_Z_EV_BYE && ol == 6);

        // a bad compressed packet is refused with 'F', and the next transfer works again
        std::vector<uint8_t> bad = {'F', 'R', 'A', 'M', 'E', 'Z', 'e', 'D', 'M', 'D', 0x05, 0, 4, 1, 1, 2, 3, 4};
        bad.resize(128, 0);
        ol = 0;
        CHECK(dmd_zedmd_feed(&z, bad.data(), bad.size(), outb, sizeof outb, &ol) == DMD_Z_EV_ERROR);
        CHECK(ol == 6 && !memcmp(outb, "ZeDMDF", 6));
        CHECK(cmd(0x0B) == 0 && ol == 6 && !memcmp(outb, "ZeDMDA", 6));
        std::vector<uint8_t> big = {'F', 'R', 'A', 'M', 'E', 'Z', 'e', 'D', 'M', 'D', 0x05, 0x10, 0x00, 0};
        big.resize(128, 0);
        ol = 0;
        CHECK(dmd_zedmd_feed(&z, big.data(), big.size(), outb, sizeof outb, &ol) == DMD_Z_EV_ERROR);   // 4096 > max
        // the PC gave up mid-transfer: reset, and the next "FRAME" starts clean
        std::vector<uint8_t> half = {'F', 'R', 'A', 'M', 'E', 'Z', 'e', 'D'};
        ol = 0;
        dmd_zedmd_feed(&z, half.data(), half.size(), outb, sizeof outb, &ol);
        CHECK(dmd_zedmd_busy(&z));
        dmd_zedmd_reset(&z);
        CHECK(cmd(0x06) == DMD_Z_EV_RENDER && ol == 6);
    }
    // ---- the menu link: "NVDM" type len payload sum16, in any split, among noise and ZeDMD traffic
    {
        auto msgv = [](uint8_t type, const std::string &pl) {
            std::vector<uint8_t> f(9);
            dmd_link_header(type, (uint32_t)pl.size(), f.data());
            f.insert(f.end(), pl.begin(), pl.end());
            uint8_t t[2];
            dmd_link_trailer(type, (uint32_t)pl.size(), (const uint8_t *)pl.data(), t);
            f.push_back(t[0]);
            f.push_back(t[1]);
            return f;
        };
        struct Got { std::vector<std::pair<uint8_t, std::string>> v; } got;
        auto cb = [](void *ctx, uint8_t type, const uint8_t *p, uint32_t n) {
            static_cast<Got *>(ctx)->v.push_back({type, std::string((const char *)p, n)});
        };
        std::vector<uint8_t> lbuf(4096);
        dmd_link_t l;
        dmd_link_init(&l, lbuf.data(), (uint32_t)lbuf.size());
        std::vector<uint8_t> stream = {'N', 'V', 'x', 'N', 'N'};                 // noise, a false start
        auto a = msgv('L', "{\"list\":\"recent\"}"), b = msgv('G', "{\"game\":\"end\"}"), e = msgv('P', "");
        stream.insert(stream.end(), a.begin(), a.end());
        const char *zed = "ZeDMDAFRAMEZeDMD";                                      // ZeDMD bytes in between
        stream.insert(stream.end(), zed, zed + strlen(zed));
        stream.insert(stream.end(), b.begin(), b.end());
        stream.insert(stream.end(), e.begin(), e.end());
        for (uint8_t x : stream) dmd_link_feed(&l, &x, 1, cb, &got);              // byte by byte
        CHECK(got.v.size() == 3 && got.v[0].first == 'L' && got.v[0].second == "{\"list\":\"recent\"}");
        CHECK(got.v[1].first == 'G' && got.v[2].first == 'P' && got.v[2].second.empty() && l.bad == 0);
        got.v.clear();
        dmd_link_feed(&l, stream.data(), stream.size(), cb, &got);                // all at once
        CHECK(got.v.size() == 3);
        got.v.clear();
        auto bad = msgv('T', "{\"id\":7}");
        bad[11] ^= 1;                                                             // a payload bit flipped
        dmd_link_feed(&l, bad.data(), bad.size(), cb, &got);
        dmd_link_feed(&l, a.data(), a.size(), cb, &got);                          // the next one still arrives
        CHECK(got.v.size() == 1 && got.v[0].first == 'L' && l.bad == 1);
        got.v.clear();
        std::vector<uint8_t> huge(9);
        dmd_link_header('I', 5000, huge.data());                                  // over the buffer
        dmd_link_feed(&l, huge.data(), huge.size(), cb, &got);
        dmd_link_feed(&l, a.data(), a.size(), cb, &got);
        CHECK(got.v.size() == 1 && l.bad == 2);
        std::string big(4000, 'x');                                               // a large one, in pieces
        auto m = msgv('I', big);
        got.v.clear();
        for (size_t o = 0; o < m.size(); o += 333) dmd_link_feed(&l, m.data() + o, std::min<size_t>(333, m.size() - o), cb, &got);
        CHECK(got.v.size() == 1 && got.v[0].second == big);
    }
    {   // FullDMD: the dark window left for the DMD in the art
        const int W = 1024, H = 600;
        std::vector<uint16_t> img(W * H);
        auto paint = [&](uint16_t bg, int x, int y, int w, int h, uint16_t win) {
            std::fill(img.begin(), img.end(), bg);
            for (int yy = y; yy < y + h; yy++)
                for (int xx = x; xx < x + w; xx++) img[yy * W + xx] = win;
        };
        dmd_rect_t r{};
        paint(0xF800, 101, 203, 797, 198, 0x0000);                                // off the 4 px blocks
        CHECK(dmd_find_window(img.data(), W, H, &r) && r.x == 101 && r.y == 203 && r.w == 797 && r.h == 198);
        paint(0x07E0, 60, 350, 900, 220, 0x1082);                                 // a dark grey glass counts
        CHECK(dmd_find_window(img.data(), W, H, &r) && r.x == 60 && r.y == 350 && r.w == 900 && r.h == 220);
        paint(0xFFFF, 300, 100, 400, 400, 0x0000);                                // a square hole: cut to 2:1
        CHECK(dmd_find_window(img.data(), W, H, &r) && r.w == 400 && r.h >= 200 && r.h <= 208 && r.y >= 100 && r.y + r.h <= 500);
        paint(0xFFFF, 0, 450, 1024, 150, 0x0000);                                 // dark along the edge: background
        CHECK(!dmd_find_window(img.data(), W, H, &r));
        paint(0xFFFF, 40, 500, 950, 80, 0x0000);                                  // a strip wider than 6:1
        CHECK(!dmd_find_window(img.data(), W, H, &r));
        paint(0xFFFF, 400, 300, 200, 60, 0x0000);                                 // too small
        CHECK(!dmd_find_window(img.data(), W, H, &r));
        std::fill(img.begin(), img.end(), 0x8410);                                // no dark at all
        CHECK(!dmd_find_window(img.data(), W, H, &r));
        CHECK(!dmd_find_window(nullptr, W, H, &r) && !dmd_find_window(img.data(), 8, 8, &r));
        CHECK(!dmd_find_window(img.data(), DMD_WIN_MAX_W + 4, 16, &r));
    }
    {   // Rebuilt DMD: twice the rows, Scale2x
        const int W = 8, H = 4;
        std::vector<uint32_t> in(W * H, 0), out(W * H * 2, 0xDEAD);
        CHECK(dmd_scale2x_rows(in.data(), W, H, out.data()));
        bool dark = true;
        for (uint32_t v : out) dark = dark && v == 0;
        CHECK(dark);                                                              // black stays black
        in[1 * W + 3] = 0xFF0000;                                                 // one dot: two rows of it
        CHECK(dmd_scale2x_rows(in.data(), W, H, out.data()));
        CHECK(out[2 * W + 3] == 0xFF0000 && out[3 * W + 3] == 0xFF0000);
        int lit = 0;
        for (uint32_t v : out) lit += v != 0;
        CHECK(lit == 2);
        std::fill(in.begin(), in.end(), 0x00FF00);                                // all lit stays all lit
        CHECK(dmd_scale2x_rows(in.data(), W, H, out.data()));
        bool all = true;
        for (uint32_t v : out) all = all && v == 0x00FF00;
        CHECK(all);
        std::fill(in.begin(), in.end(), 0u);                                      // a diagonal gets no gaps
        for (int i = 0; i < H; i++) in[i * W + i] = 0xFFFFFF;
        CHECK(dmd_scale2x_rows(in.data(), W, H, out.data()));
        for (int r = 0; r < 2 * H; r++) {
            int n = 0;
            for (int c = 0; c < W; c++) n += out[r * W + c] != 0;
            CHECK(n >= 1);
        }
        std::vector<uint32_t> big(DMD_MAX_PX);
        CHECK(!dmd_scale2x_rows(big.data(), 256, DMD_MAX_H / 2 + 1, big.data()));   // too many rows
        CHECK(!dmd_scale2x_rows(nullptr, W, H, out.data()) && !dmd_scale2x_rows(in.data(), 0, H, out.data()));
    }
    {   // Rebuilt DMD: a 128x16 DMD centred in 128x32 is rebuilt 4x, a full one 2x
        const int W = 16, H = 32;
        std::vector<uint32_t> in(W * H, 0), out(W * H * 2, 0xDEAD), tmp(W * H * 2);
        int lo = 0, hi = 0, r0 = 0, n = 0;
        CHECK(!dmd_lit_rows(in.data(), W, H, &lo, &hi) && lo == -1 && hi == -1);
        for (int r = 8; r < 24; r++) in[r * W + 5] = 0x00FF00;                    // a column in the middle half
        CHECK(dmd_lit_rows(in.data(), W, H, &lo, &hi) && lo == 8 && hi == 23);
        dmd_rebuild_rows(H, lo, hi, &r0, &n);
        CHECK(r0 == 8 && n == 16);
        CHECK(dmd_rebuild(in.data(), W, H, r0, n, out.data(), tmp.data()));
        bool col = true, rest = true;
        for (int r = 0; r < 2 * H; r++)
            for (int c = 0; c < W; c++) {
                const uint32_t v = out[r * W + c];
                if (c == 5) col = col && v == 0x00FF00;                           // 16 rows -> all 64
                else rest = rest && v == 0;
            }
        CHECK(col && rest);
        dmd_rebuild_rows(H, 3, 29, &r0, &n);                                      // lit outside: all rows, 2x
        CHECK(r0 == 0 && n == H);
        dmd_rebuild_rows(H, -1, -1, &r0, &n);                                     // nothing lit yet: all rows
        CHECK(r0 == 0 && n == H);
        CHECK(dmd_rebuild(in.data(), W, H, 0, H, out.data(), tmp.data()));
        std::vector<uint32_t> ref(W * H * 2);
        CHECK(dmd_scale2x_rows(in.data(), W, H, ref.data()) && ref == out);       // 2x: same as one pass
        std::vector<uint32_t> odd(W * H, 0);                                      // 24 rows: 48 of 64, centred
        for (int r = 4; r < 28; r++) odd[r * W] = 0xFFFFFF;
        CHECK(dmd_rebuild(odd.data(), W, H, 4, 24, out.data(), tmp.data()));
        int first = -1, last = -1;
        for (int r = 0; r < 2 * H; r++) if (out[r * W]) { if (first < 0) first = r; last = r; }
        CHECK(first == 8 && last == 55);
        CHECK(!dmd_rebuild(in.data(), W, H, 20, 16, out.data(), tmp.data()));     // past the frame
        CHECK(!dmd_rebuild(in.data(), W, H, 0, H, out.data(), nullptr));
    }
    {   // Ambient bands: the light the DMD throws above and below it
        const int W = 128, H = 32;
        std::vector<uint32_t> px(W * H, 0), top(W), bot(W), scr(W), band(W * 4);
        dmd_ambient(px.data(), W, H, 0, top.data(), bot.data(), scr.data());
        bool dark = true;
        for (int c = 0; c < W; c++) dark = dark && top[c] == 0 && bot[c] == 0;
        CHECK(dark);                                                              // nothing lit: no light
        dmd_ambient_band(top.data(), W, 4, true, 0, 0x050302, band.data());
        bool floor = true;
        for (uint32_t v : band) floor = floor && v == 0x050302;
        CHECK(floor);                                                             // only the unlit dots
        for (int y = 0; y < 8; y++)                                               // a red block, top left
            for (int x = 10; x < 30; x++) px[y * W + x] = 0xFF0000;
        dmd_ambient(px.data(), W, H, 0, top.data(), bot.data(), scr.data());
        CHECK((top[20] >> 16 & 255) > 0 && (top[20] & 0xFFFF) == 0);              // red above it
        CHECK(top[100] == 0 && bot[20] == 0);                                     // not far away, not below
        CHECK((top[20] >> 16) > (top[36] >> 16));                                 // fading sideways
        dmd_ambient_band(top.data(), W, 4, true, 0, 0, band.data());
        CHECK((band[3 * W + 20] >> 16) > (band[0 * W + 20] >> 16));               // brightest next to the DMD
        dmd_ambient_band(top.data(), W, 4, false, 0, 0, band.data());
        CHECK((band[0 * W + 20] >> 16) > (band[3 * W + 20] >> 16));               // ...on either side
        dmd_ambient_band(top.data(), W, 4, true, 1, 0x010101, band.data());       // a margin row of unlit dots
        CHECK(band[3 * W + 20] == 0x010101 && (band[2 * W + 20] >> 16) > (band[0 * W + 20] >> 16));
        dmd_ambient_band(top.data(), W, 4, false, 1, 0x010101, band.data());
        CHECK(band[0 * W + 20] == 0x010101 && (band[1 * W + 20] >> 16) > (band[3 * W + 20] >> 16));
        std::fill(px.begin(), px.end(), 0u);                                      // white vs a colour: half
        for (int y = 0; y < 16; y++) for (int x = 0; x < W; x++) px[y * W + x] = 0xFFFFFF;
        dmd_ambient(px.data(), W, H, 0, top.data(), bot.data(), scr.data());
        const uint32_t white = top[64] >> 16 & 255;
        for (int y = 0; y < 16; y++) for (int x = 0; x < W; x++) px[y * W + x] = 0x00FF00;
        dmd_ambient(px.data(), W, H, 0, top.data(), bot.data(), scr.data());
        CHECK(white > 0 && white * 2 <= (top[64] >> 8 & 255) + 2);
        std::vector<uint32_t> side(H), sb(H), ss(H);                              // rotated: columns are rows
        dmd_ambient(px.data(), W, H, 1, side.data(), sb.data(), ss.data());
        CHECK((side[28] != 0 || sb[28] != 0) && side[5] == 0 && sb[5] == 0);   // panel column 28 = frame row 3 (lit), 5 = row 26 (dark)
    }
    {   // Rebuilt into the frame's own height: a 128x16 DMD inside 128x32 gets its 32 rows back
        const int W = 16, H = 32;
        std::vector<uint32_t> in(W * H, 0), out(W * H, 0xDEAD), tmp(W * H);
        for (int r = 8; r < 24; r++) in[r * W + 3] = 0xFF8800;
        CHECK(dmd_rebuild_to(in.data(), W, H, 8, 16, out.data(), tmp.data(), H));
        bool col = true;
        for (int r = 0; r < H; r++) col = col && out[r * W + 3] == 0xFF8800 && out[r * W + 4] == 0;
        CHECK(col);
        CHECK(!dmd_rebuild_to(in.data(), W, H, 8, 16, out.data(), tmp.data(), H - 1));   // fewer rows than h
        CHECK(!dmd_rebuild_to(in.data(), W, H, 8, 16, out.data(), tmp.data(), 2 * H + 1));
    }
    {   // Rebuilt DMD: right angles stay square (an L's inside corner gets no dot across it)
        const int W = 8, H = 8;
        std::vector<uint32_t> in(W * H, 0), out(W * H * 2);
        for (int r = 1; r <= 5; r++) in[r * W + 2] = 0xFFFFFF;                     // the L's stem, column 2
        for (int c = 2; c <= 5; c++) in[5 * W + c] = 0xFFFFFF;                     // its foot, row 5
        CHECK(dmd_scale2x_rows(in.data(), W, H, out.data()));
        CHECK(out[9 * W + 3] == 0);            // row 4's lower half, next to the stem: still dark (was a dot)
        bool foot = true;
        for (int c = 2; c <= 5; c++) foot = foot && out[10 * W + c] && out[11 * W + c];
        CHECK(foot && out[10 * W + 6] == 0 && out[8 * W + 3] == 0);                // the foot exactly, 2 rows
        std::vector<uint32_t> d(W * H, 0), o2(W * H * 2);                          // a diagonal still smooth
        for (int i = 0; i < 6; i++) d[i * W + i] = 0xFF0000;
        CHECK(dmd_scale2x_rows(d.data(), W, H, o2.data()));
        int gaps = 0;
        for (int r = 0; r < 12; r++) {
            int n = 0;
            for (int c = 0; c < W; c++) n += o2[r * W + c] != 0;
            gaps += n == 0;
        }
        CHECK(gaps == 0 && o2[1 * W + 1] != 0);                                    // the step filled across
    }
    {   // The housing in a grill: dark grey (an unlit backglass), centred, may touch the grill's top / bottom
        const int W = 640, H = 140;
        std::vector<uint16_t> g(W * H, 0x8410);                                    // a mid-grey grill
        auto box = [&](int x, int y, int w, int h, uint16_t c) {
            for (int yy = y; yy < y + h; yy++) for (int xx = x; xx < x + w; xx++) g[yy * W + xx] = c;
        };
        box(170, 20, 300, 100, 0x4208);                                            // dark grey housing (~26%)
        box(30, 30, 90, 80, 0x0000);                                               // a black speaker, left
        box(520, 30, 90, 80, 0x0000);                                              // and right
        dmd_rect_t r{};
        CHECK(!dmd_find_window(g.data(), W, H, &r));                               // not black: the art rule misses it
        CHECK(dmd_find_housing(g.data(), W, H, &r) && r.x == 170 && r.y == 20 && r.w == 300 && r.h == 100);
        box(170, 0, 300, 140, 0x4208);                                             // the whole grill's height
        CHECK(dmd_find_housing(g.data(), W, H, &r) && r.x == 170 && r.w == 300);
        std::fill(g.begin(), g.end(), 0x8410);                                     // only off-centre dark: none
        box(20, 20, 250, 100, 0x0000);
        CHECK(!dmd_find_housing(g.data(), W, H, &r));
        std::fill(g.begin(), g.end(), 0xFFFF);                                     // no dark at all
        CHECK(!dmd_find_housing(g.data(), W, H, &r));
    }
    {   // The second screen: Dracula's grill and housing, TMNT's FullDMD art, a backglass over the DMD
        auto near = [](int a, int b) { return a - b <= 3 && b - a <= 3; };
        dmd_s2_in_t in{};
        in.sw = 1280; in.sh = 1024;
        in.bw = 644; in.bh = 600; in.grill = 155; in.housing = {168, 468, 313, 87};
        in.dw = 128; in.dh = 32;
        dmd_s2_out_t o{};
        CHECK(dmd_s2_avail(DMD_S2_KEEP, &in) && !dmd_s2_layout(DMD_S2_KEEP, &in, &o));
        CHECK(dmd_s2_avail(DMD_S2_HOUSING, &in) && !dmd_s2_avail(DMD_S2_ART, &in) && !dmd_s2_avail(DMD_S2_BG_ART, &in));
        CHECK(dmd_s2_layout(DMD_S2_HOUSING, &in, &o) && o.grill && o.art.w == 0);
        CHECK(o.bg.x == 90 && o.bg.y == 0 && o.bg.w == 1099 && o.bg.h == 1024);      // its shape kept, centred
        CHECK(near(o.dmd.x, 380) && near(o.dmd.y, 802) && near(o.dmd.w, 526) && near(o.dmd.h, 141));
        CHECK(dmd_s2_layout(DMD_S2_BG, &in, &o) && !o.grill && o.dmd.w == 0);
        CHECK(o.bg.w == 1280 && o.bg.h == 1024);                                   // 644x445: 14% stretch fills
        in.grill = 0;                                                              // no grill: no housing
        CHECK(!dmd_s2_avail(DMD_S2_HOUSING, &in));
        in.bw = 1280; in.bh = 750;                                                 // TMNT
        CHECK(dmd_s2_layout(DMD_S2_BG_DMD, &in, &o));
        CHECK(o.bg.x == 0 && o.bg.y == 0 && o.bg.w == 1280 && o.bg.h == 704);
        CHECK(o.dmd.x == 0 && o.dmd.y == 704 && o.dmd.w == 1280 && o.dmd.h == 320);
        in.aw = 1920; in.ah = 1080; in.window = {84, 398, 1751, 299};
        CHECK(dmd_s2_layout(DMD_S2_ART, &in, &o));
        CHECK(o.art.x == 0 && o.art.y == 152 && o.art.w == 1280 && o.art.h == 720);  // 16:9 kept (30% off)
        // the window is 6:1: the 4:1 DMD keeps its shape (20% wider at most), centred in it
        CHECK(near(o.dmd.y, 422) && near(o.dmd.h, 189) && near(o.dmd.w, 189 * 4 * 12 / 10) && near(o.dmd.x, 61 + (1157 - o.dmd.w) / 2));
        CHECK(o.bg.x == o.art.x && o.bg.w == o.art.w);                             // behind the art
        CHECK(dmd_s2_layout(DMD_S2_BG_ART, &in, &o));
        CHECK(o.bg.h + o.art.h <= 1024 && o.art.y == o.bg.y + o.bg.h && o.bg.w == o.art.w && o.bg.w < 1280);
        CHECK(o.dmd.y > o.art.y && o.dmd.y + o.dmd.h < o.art.y + o.art.h);
        // previews: a 2x2 picture doubled, a DMD frame with its dots apart
        const uint16_t src[4] = {1, 2, 3, 4};
        uint16_t d[16] = {};
        dmd_blit565(d, 4, 4, {0, 0, 4, 4}, src, 2, {0, 0, 2, 2});
        CHECK(d[0] == 1 && d[1] == 1 && d[2] == 2 && d[3] == 2 && d[8] == 3 && d[15] == 4);
        dmd_blit565(d, 4, 4, {2, 2, 4, 4}, src, 2, {0, 0, 2, 2});                    // clipped
        CHECK(d[10] == 1 && d[11] == 1 && d[15] == 1);
        const uint32_t fr[2] = {0xFF0000, 0};
        std::vector<uint16_t> f(6 * 3, 0xFFFF);
        dmd_blit_frame(f.data(), 6, 3, {0, 0, 6, 3}, fr, 2, 1);
        CHECK(f[0] == 0xF800 && f[1] == 0xF800 && f[2] == 0 && f[3] == 0x1082 && f[5] == 0 && f[12] == 0);
        dmd_fill565(f.data(), 6, 3, {-1, -1, 3, 3}, 7);
        CHECK(f[0] == 7 && f[7] == 7 && f[2] == 0);
    }
    return TEST_DONE("dmd");
}
