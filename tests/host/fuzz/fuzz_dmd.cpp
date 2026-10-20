// libFuzzer target for dmd_core: everything a hostile PC on the LAN can send to the DMD app's port —
// the HTTP upgrade, then a WebSocket byte stream whose messages go through the dmdext decoder and,
// for frames, the dot renderer — and everything a PC can send on the USB port to the ZeDMD emulation
// stays inside its buffers.
#include "dmd_core.h"
#include "miniz.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    if (n < 2) return 0;
    char resp[256];
    size_t used = 0, rl = 0;
    (void)dmd_ws_handshake(reinterpret_cast<const char *>(d), n, &used, resp, sizeof resp, &rl);
    if (rl >= sizeof resp || used > n) abort();

    // The rest of the input as a WebSocket stream, parsed incrementally like the server does.
    const size_t cap = 4096;
    std::vector<uint8_t> msg(cap), buf(d + 1, d + n);
    dmd_ws_t ws;
    dmd_ws_init(&ws, msg.data(), cap);
    dmd_state_t st;
    dmd_state_init(&st);
    if (d[0] & 1) st.color_override = 0x123456;
    std::vector<uint32_t> out(1024);       // small on purpose: dimensions over it must be refused
    size_t off = 0;
    while (off < buf.size()) {
        const dmd_ws_ev_t ev = dmd_ws_next(&ws, buf.data() + off, buf.size() - off, &used);
        if (ev == DMD_WS_MORE || ev == DMD_WS_ERROR) break;
        if (!used || used > buf.size() - off) abort();
        off += used;
        if (ev == DMD_WS_PING && ws.ctl_len > sizeof ws.ctl) abort();
        if (ev != DMD_WS_MESSAGE) continue;
        if (ws.msg_len > cap) abort();
        if (dmd_proto_feed(&st, ws.msg, ws.msg_len, out.data(), out.size()) != DMD_MSG_FRAME) continue;
        if ((size_t)st.w * (size_t)st.h > out.size()) abort();
        dmd_geom_t g;
        const int rot = d[0] >> 1 & 3, fit = d[0] >> 5 & 1;
        if (!dmd_geom(st.w, st.h, rot, fit, 256, 128, &g)) continue;
        if (g.ox < 0 || g.oy < 0 || g.ox + g.gw > 256 || g.oy + g.gh > 128) abort();
        static dmd_masks_t mk;
        dmd_masks_build(&mk, &g, d[0] >> 3 & 1, d[0] >> 4 & 1);
        std::vector<uint16_t> band((size_t)256 * g.gh);
        static dmd_sprites_t sc;
        dmd_sprites_reset(&sc);
        dmd_render_rows(band.data(), 256, &g, &mk, out.data(), st.w, st.h, rot, 1 + d[0] % 100, true, 256, 0, g.rows,
                        d[0] & 0x40 ? &sc : nullptr);
    }
    if (strlen(st.game) >= sizeof st.game) abort();
    {   // any text through the 5x7 renderer, into a small picture
        static uint32_t t[64 * 16];
        std::vector<char> txt(d, d + (n < 40 ? n : 40));
        txt.push_back(0);
        (void)dmd_text(t, 64, 16, txt.data(), 0xFFFFFF);
    }

    {   // the same bytes as the menu link (small buffer: long frames must be refused, not overflow)
        static uint8_t lb[512];
        dmd_link_t l;
        dmd_link_init(&l, lb, sizeof lb);
        auto cb = [](void *, uint8_t, const uint8_t *p, uint32_t len) {
            if (len > sizeof lb || (len && !p)) abort();
        };
        for (size_t o = 0; o < n; o += 7) {
            dmd_link_feed(&l, d + o, n - o < 7 ? n - o : 7, cb, nullptr);
            if (d[0] & 0x80 && (o / 7) % 5 == 4) dmd_link_reset(&l);   // the receiver's timeout, now and then
        }
        if (l.st < 0 || l.st > 3 || l.got > l.cap) abort();
    }

    // The same bytes as a ZeDMD serial stream, in pieces (chunk size and split from the first byte).
    static dmd_zedmd_t z;
    static uint32_t img[128 * 32];
    auto inflate = [](void *, const uint8_t *in, size_t len, uint8_t *o, size_t cap) -> int {
        mz_ulong ol = cap;
        return mz_uncompress(o, &ol, in, (mz_ulong)len) == MZ_OK ? (int)ol : -1;
    };
    dmd_zedmd_init(&z, 128, 32, (uint16_t)(32 * (1 + d[0] % 60)), img, inflate, nullptr);
    const size_t step = 1 + d[1] % 97;
    uint8_t outb[64 * 64];
    for (size_t o = 2; o < n; o += step) {
        size_t ol = 0;
        const size_t k = n - o < step ? n - o : step;
        (void)dmd_zedmd_feed(&z, d + o, k, outb, sizeof outb, &ol);
        if (ol > sizeof outb) abort();
        if (d[1] & 0x80 && dmd_zedmd_busy(&z)) dmd_zedmd_reset(&z);
    }
    return 0;
}
