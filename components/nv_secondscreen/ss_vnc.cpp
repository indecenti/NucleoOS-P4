// VNC viewer transport: the board shows (and controls) a VNC server's desktop.
//
// Covers the servers people actually have: macOS Screen Sharing (VNC password), Linux (GNOME
// Remote Desktop, krfb, x11vnc, wayvnc, TigerVNC), Windows (TightVNC/UltraVNC) and Android
// (droidVNC-NG). RFB 3.3/3.7/3.8 (+ Apple's 3.889 banner), security None / VNC-auth (DES in
// ss_des.c). Encodings: Tight (JPEG rects go through the P4 hardware decoder), ZRLE, Zlib,
// Hextile, CopyRect, Raw, plus DesktopSize / ExtendedDesktopSize / LastRect. When the server
// supports resizing (Xvnc, GNOME "extend", krfb-virtualmonitor) the viewer asks for exactly the
// panel size, which turns such a server into a true extended display.
//
// The remote framebuffer is never stored: every decoded source row is mapped straight into the
// panel (nearest-neighbour when the desktop is larger than 1024x600, centred otherwise), so a
// 4K desktop costs no extra RAM. Touch becomes pointer input: one finger = left button / drag,
// two-finger tap = right click, two-finger vertical drag = scroll wheel.
//
// One task ("ss_vnc", PSRAM stack): outgoing connects, the reverse-connection listener on 5500
// (droidVNC-NG / TightVNC / x11vnc "connect to viewer") and mDNS discovery of _rfb._tcp.
#include "ss_internal.h"
#include "ss_net.h"
#include "ss_des.h"
#include "nv_ss_links.h"

#include "nv_log.h"

#include "mdns.h"
#include "mbedtls/bignum.h"
#include "mbedtls/md5.h"
#include "mbedtls/aes.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "miniz.h"   // ESP ROM tinfl (esp_rom)

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <strings.h>

namespace {

constexpr const char *TAG = "ss_vnc";
constexpr int kW = NV_SS_PANEL_W, kH = NV_SS_PANEL_H;

enum : int32_t {
    ENC_RAW = 0, ENC_COPYRECT = 1, ENC_HEXTILE = 5, ENC_ZLIB = 6, ENC_TIGHT = 7, ENC_ZRLE = 16,
    ENC_DESKTOPSIZE = -223, ENC_LASTRECT = -224, ENC_EXTDESKTOPSIZE = -308,
    ENC_JPEG_Q6 = -32 + 6, ENC_COMPRESS_6 = -256 + 6,
};

// ---------------------------------------------------------------- shared state (s_lk)
SemaphoreHandle_t s_lk = nullptr;
QueueHandle_t s_q = nullptr;
nv_ss_vnc_info_t s_info = {};
const char *s_enc_name = "";
volatile bool s_listen_want = false;
volatile bool s_discover_req = false;
volatile bool s_discovering = false;
nv_ss_vnc_server_t s_found[8];
int s_found_n = 0;

struct Cmd {
    char host[40];
    uint16_t port;
    char user[64];
    char pw[64];
};

struct Lk {
    Lk() { xSemaphoreTake(s_lk, portMAX_DELAY); }
    ~Lk() { xSemaphoreGive(s_lk); }
};

void set_state(nv_ss_vnc_state_t st, nv_ss_vnc_err_t err, const char *detail) {
    Lk l;
    s_info.state = st;
    s_info.err = err;
    snprintf(s_info.detail, sizeof s_info.detail, "%s", detail ? detail : "");
}

// ---------------------------------------------------------------- buffered reader
struct Reader {
    SsConn *c = nullptr;
    uint8_t *buf = nullptr;
    size_t cap = 0, pos = 0, len = 0;
    volatile bool *stop = nullptr;
    bool err = false;

    bool fill(int stall_ms) {
        if (pos == len) { pos = len = 0; }
        else if (pos > cap / 2) { memmove(buf, buf + pos, len - pos); len -= pos; pos = 0; }
        const int64_t t0 = esp_timer_get_time();
        for (;;) {
            if (stop && *stop) { err = true; return false; }
            const int r = c->recv(buf + len, cap - len);
            if (r > 0) { len += (size_t)r; return true; }
            if (r == -2 && (esp_timer_get_time() - t0) / 1000 < stall_ms) continue;
            err = true;
            return false;
        }
    }
    bool read(void *dst, size_t n) {
        uint8_t *d = (uint8_t *)dst;
        while (n) {
            if (pos == len && !fill(20000)) return false;
            const size_t k = std::min(n, len - pos);
            if (d) { memcpy(d, buf + pos, k); d += k; }
            pos += k;
            n -= k;
        }
        return true;
    }
    bool skip(size_t n) { return read(nullptr, n); }
    uint8_t u8() { uint8_t v = 0; read(&v, 1); return v; }
    uint16_t u16() { uint8_t b[2] = {}; read(b, 2); return (uint16_t)(b[0] << 8 | b[1]); }
    uint32_t u32() { uint8_t b[4] = {}; read(b, 4); return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; }
    // Wait (indefinitely, 1 s slices) for the first byte of the next server message.
    int wait_message(void) {
        while (pos == len) {
            if (stop && *stop) return 0;
            const int r = c->recv(buf, cap);
            if (r > 0) { pos = 0; len = (size_t)r; return 1; }
            if (r == -2) return -2;
            return 0;
        }
        return 1;
    }
};

// ---------------------------------------------------------------- zlib streams (ROM tinfl)
struct ZStream {
    tinfl_decompressor *d = nullptr;
    uint8_t *dict = nullptr;       // 32 KB circular window (TINFL_LZ_DICT_SIZE)
    size_t ofs = 0;
    bool fresh = true;
    bool ensure(void) {
        if (!d) d = (tinfl_decompressor *)heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM);
        if (!dict) dict = (uint8_t *)heap_caps_malloc(TINFL_LZ_DICT_SIZE, MALLOC_CAP_SPIRAM);
        if (d && fresh) { tinfl_init(d); ofs = 0; fresh = false; }
        return d && dict;
    }
    void reset(void) { fresh = true; }
    void release(void) {
        heap_caps_free(d); heap_caps_free(dict);
        d = nullptr; dict = nullptr; fresh = true;
    }
};

// Pulls decompressed bytes out of one compressed block (already in memory).
struct ZPull {
    ZStream *z;
    const uint8_t *in;
    size_t in_left;
    size_t av_pos = 0, av_len = 0;
    bool bad = false;
    ZPull(ZStream *zz, const uint8_t *i, size_t n) : z(zz), in(i), in_left(n) {}

    bool more(void) {
        for (int guard = 0; guard < 64; guard++) {
            size_t in_sz = in_left;
            size_t out_sz = TINFL_LZ_DICT_SIZE - z->ofs;
            const tinfl_status st = tinfl_decompress(z->d, in, &in_sz, z->dict, z->dict + z->ofs, &out_sz,
                                                     TINFL_FLAG_HAS_MORE_INPUT | TINFL_FLAG_PARSE_ZLIB_HEADER);
            in += in_sz;
            in_left -= in_sz;
            av_pos = z->ofs;
            av_len = out_sz;
            z->ofs = (z->ofs + out_sz) & (TINFL_LZ_DICT_SIZE - 1);
            if (st < 0) { bad = true; return false; }
            if (out_sz) return true;
            if (in_sz == 0) return false;   // needs input this block doesn't have
        }
        return false;
    }
    bool read(uint8_t *dst, size_t n) {
        while (n) {
            if (!av_len && !more()) { bad = true; return false; }
            const size_t k = std::min(n, av_len);
            if (dst) { memcpy(dst, z->dict + av_pos, k); dst += k; }
            av_pos += k;
            av_len -= k;
            n -= k;
        }
        return true;
    }
    uint8_t u8(void) { uint8_t v = 0; read(&v, 1); return v; }
};

// ---------------------------------------------------------------- panel mapping
inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
// client pixel format: 32 bpp little-endian, R<<16 G<<8 B -> bytes [B,G,R,x]
inline uint16_t px32(const uint8_t *p) { return rgb565(p[2], p[1], p[0]); }
inline uint16_t cpx24(const uint8_t *p) { return rgb565(p[2], p[1], p[0]); }   // ZRLE CPIXEL [B,G,R]
inline uint16_t tpx24(const uint8_t *p) { return rgb565(p[0], p[1], p[2]); }   // Tight TPIXEL [R,G,B]

struct Map {
    int rw = 0, rh = 0;          // remote framebuffer
    int dw = 0, dh = 0, ox = 0, oy = 0;
    bool unity = true;
    uint16_t *xmap = nullptr;    // dst column -> src column (dw entries)

    bool setup(int w, int h) {
        rw = w; rh = h;
        const float s = std::min(1.0f, std::min((float)kW / w, (float)kH / h));
        dw = std::max(1, std::min(kW, (int)(w * s + 0.5f)));
        dh = std::max(1, std::min(kH, (int)(h * s + 0.5f)));
        unity = (dw == w && dh == h);
        ox = (kW - dw) / 2;
        oy = (kH - dh) / 2;
        heap_caps_free(xmap);
        xmap = (uint16_t *)heap_caps_malloc(sizeof(uint16_t) * dw, MALLOC_CAP_SPIRAM);
        if (!xmap) return false;
        for (int d = 0; d < dw; d++) xmap[d] = (uint16_t)((int64_t)d * rw / dw);
        return true;
    }
    void release(void) { heap_caps_free(xmap); xmap = nullptr; }
    // first dst column/row whose source is >= s
    int xinv(int s) const { return (int)(((int64_t)s * dw + rw - 1) / rw); }
    int yinv(int s) const { return (int)(((int64_t)s * dh + rh - 1) / rh); }
    bool row_visible(int sy) const { return yinv(sy) < yinv(sy + 1); }
    int to_remote_x(int px) const { int v = (int)((int64_t)(px - ox) * rw / std::max(dw, 1)); return std::clamp(v, 0, rw - 1); }
    int to_remote_y(int py) const { int v = (int)((int64_t)(py - oy) * rh / std::max(dh, 1)); return std::clamp(v, 0, rh - 1); }
};

// ---------------------------------------------------------------- session
struct Session {
    SsConn c;
    Reader rd;
    Map map;
    ZStream zrle, zlib, tight[4];
    uint8_t *row32 = nullptr;    // raw source row (up to 8192 px * 4)
    uint16_t *row565 = nullptr;  // converted source row
    uint16_t *tile = nullptr;    // 64x64 RGB565 (ZRLE/Hextile/Tight palette)
    uint8_t *zbuf = nullptr;     // compressed block
    size_t zcap = 0;
    uint16_t *prev = nullptr;    // Tight gradient: previous row (RGB888 as 3x u16 per px)
    SemaphoreHandle_t tx = nullptr;
    volatile bool stop = false;
    volatile bool paused = false;
    volatile bool want_full = false;
    bool began = false;
    bool resize_asked = false;
    bool ext_desktop = false;
    uint32_t screen_id = 0;
    char desktop[48] = "";
    // touch -> pointer state
    uint8_t buttons = 0;
    bool two = false, two_moved = false;
    int two_y0 = 0, two_acc = 0, last_x = 0, last_y = 0;

    ~Session() {
        heap_caps_free(row32); heap_caps_free(row565); heap_caps_free(tile);
        heap_caps_free(zbuf); heap_caps_free(prev); heap_caps_free(rd.buf);
        zrle.release(); zlib.release();
        for (auto &t : tight) t.release();
        map.release();
        if (tx) vSemaphoreDelete(tx);
    }

    bool alloc(void) {
        rd.cap = 32 * 1024;
        rd.buf = (uint8_t *)heap_caps_malloc(rd.cap, MALLOC_CAP_SPIRAM);
        row32 = (uint8_t *)heap_caps_malloc(8192 * 4, MALLOC_CAP_SPIRAM);
        row565 = (uint16_t *)heap_caps_malloc(8192 * 2, MALLOC_CAP_SPIRAM);
        tile = (uint16_t *)heap_caps_malloc(64 * 64 * 2, MALLOC_CAP_SPIRAM);
        prev = (uint16_t *)heap_caps_malloc(8192 * 3 * 2, MALLOC_CAP_SPIRAM);
        tx = xSemaphoreCreateMutex();
        rd.c = &c;
        rd.stop = &stop;
        return rd.buf && row32 && row565 && tile && prev && tx;
    }

    bool send(const void *p, size_t n) {
        xSemaphoreTake(tx, portMAX_DELAY);
        const bool ok = c.send_all(p, n);
        xSemaphoreGive(tx);
        return ok;
    }

    // ---- output: one source row segment (RGB565) into the panel
    void emit(int sy, int x0, int w, const uint16_t *src) {
        if (!map.row_visible(sy)) return;
        const int dy0 = map.yinv(sy), dy1 = map.yinv(sy + 1);
        const int dx0 = map.xinv(x0), dx1 = map.xinv(x0 + w);
        if (dx0 >= dx1) return;
        uint16_t *fb;
        if (!nv_ss_fb_begin(NV_SS_SRC_VNC, &fb)) return;   // paused / ended: drop silently
        for (int dy = dy0; dy < dy1; dy++) {
            uint16_t *d = fb + (size_t)(map.oy + dy) * kW + map.ox;
            if (map.unity) memcpy(d + dx0, src, (size_t)(dx1 - dx0) * 2);
            else for (int dx = dx0; dx < dx1; dx++) d[dx] = src[map.xmap[dx] - x0];
        }
        nv_ss_fb_end(map.ox + dx0, map.oy + dy0, dx1 - dx0, dy1 - dy0);
    }
    void fill(int x, int y, int w, int h, uint16_t c565) {
        for (int i = 0; i < w; i++) row565[i] = c565;
        for (int r = 0; r < h; r++) emit(y + r, x, w, row565);
    }
    void emit_tile(int x, int y, int w, int h) {   // `tile` holds w*h pixels
        for (int r = 0; r < h; r++) emit(y + r, x, w, tile + r * w);
    }

    bool ensure_zbuf(size_t n) {
        if (n > 16u * 1024 * 1024) return false;
        if (n > zcap) {
            heap_caps_free(zbuf);
            zcap = std::max(n, (size_t)64 * 1024);
            zbuf = (uint8_t *)heap_caps_malloc(zcap, MALLOC_CAP_SPIRAM);
            if (!zbuf) { zcap = 0; return false; }
        }
        return true;
    }
    bool read_block(size_t n) { return ensure_zbuf(n) && rd.read(zbuf, n); }   // n bytes into zbuf

    // ---- encodings
    bool dec_raw(int x, int y, int w, int h) {
        for (int r = 0; r < h; r++) {
            if (!rd.read(row32, (size_t)w * 4)) return false;
            if (!map.row_visible(y + r)) continue;
            for (int i = 0; i < w; i++) row565[i] = px32(row32 + i * 4);
            emit(y + r, x, w, row565);
        }
        return true;
    }

    bool dec_copyrect(int x, int y, int w, int h) {
        const int sx = rd.u16(), sy = rd.u16();
        if (rd.err) return false;
        // The source rect comes from the network too: it must lie inside the remote framebuffer
        // (the destination was checked by the caller), else the copy would run off the panel.
        if (sx + w > map.rw || sy + h > map.rh) return false;
        uint16_t *fb;
        if (!nv_ss_fb_begin(NV_SS_SRC_VNC, &fb)) return true;
        // In panel space (exact when unity; a close approximation when scaled).
        const int ddx = map.xinv(x), ddy = map.yinv(y);
        const int dsx = map.xinv(sx), dsy = map.yinv(sy);
        const int cw = std::min(map.xinv(x + w) - ddx, map.xinv(sx + w) - dsx);
        const int ch = std::min(map.yinv(y + h) - ddy, map.yinv(sy + h) - dsy);
        if (cw > 0 && ch > 0) {
            uint16_t *base = fb + (size_t)map.oy * kW + map.ox;
            // The DMA/CPU view of the panel may be stale in the cache: refresh the source span.
            if (ddy <= dsy) {
                for (int r = 0; r < ch; r++)
                    memmove(base + (size_t)(ddy + r) * kW + ddx, base + (size_t)(dsy + r) * kW + dsx, (size_t)cw * 2);
            } else {
                for (int r = ch - 1; r >= 0; r--)
                    memmove(base + (size_t)(ddy + r) * kW + ddx, base + (size_t)(dsy + r) * kW + dsx, (size_t)cw * 2);
            }
        }
        nv_ss_fb_end(map.ox + ddx, map.oy + ddy, std::max(cw, 0), std::max(ch, 0));
        return true;
    }

    bool dec_hextile(int x, int y, int w, int h) {
        uint16_t bg = 0, fg = 0;
        uint8_t p[4];
        for (int ty = y; ty < y + h; ty += 16) {
            const int th = std::min(16, y + h - ty);
            for (int tx = x; tx < x + w; tx += 16) {
                const int tw = std::min(16, x + w - tx);
                const uint8_t se = rd.u8();
                if (rd.err) return false;
                if (se & 1) {   // raw tile
                    for (int i = 0; i < tw * th; i++) { if (!rd.read(p, 4)) return false; tile[i] = px32(p); }
                    emit_tile(tx, ty, tw, th);
                    continue;
                }
                if (se & 2) { if (!rd.read(p, 4)) return false; bg = px32(p); }
                if (se & 4) { if (!rd.read(p, 4)) return false; fg = px32(p); }
                for (int i = 0; i < tw * th; i++) tile[i] = bg;
                if (se & 8) {
                    const int n = rd.u8();
                    for (int s = 0; s < n; s++) {
                        uint16_t c = fg;
                        if (se & 16) { if (!rd.read(p, 4)) return false; c = px32(p); }
                        const uint8_t xy = rd.u8(), wh = rd.u8();
                        if (rd.err) return false;
                        const int sx = xy >> 4, sy = xy & 15, sw = (wh >> 4) + 1, sh = (wh & 15) + 1;
                        for (int r = sy; r < std::min(th, sy + sh); r++)
                            for (int q = sx; q < std::min(tw, sx + sw); q++) tile[r * tw + q] = c;
                    }
                }
                emit_tile(tx, ty, tw, th);
            }
        }
        return true;
    }

    bool dec_zlib(int x, int y, int w, int h) {
        const uint32_t n = rd.u32();
        if (rd.err || !read_block(n) || !zlib.ensure()) return false;
        ZPull zp(&zlib, zbuf, n);
        for (int r = 0; r < h; r++) {
            if (!zp.read(row32, (size_t)w * 4)) return false;
            if (!map.row_visible(y + r)) continue;
            for (int i = 0; i < w; i++) row565[i] = px32(row32 + i * 4);
            emit(y + r, x, w, row565);
        }
        return true;
    }

    bool dec_zrle(int x, int y, int w, int h) {
        const uint32_t n = rd.u32();
        if (rd.err || !read_block(n) || !zrle.ensure()) return false;
        ZPull zp(&zrle, zbuf, n);
        uint8_t c3[3];
        uint16_t pal[128];
        for (int ty = y; ty < y + h; ty += 64) {
            const int th = std::min(64, y + h - ty);
            for (int tx = x; tx < x + w; tx += 64) {
                const int tw = std::min(64, x + w - tx);
                const int npx = tw * th;
                const uint8_t se = zp.u8();
                if (zp.bad) return false;
                if (se == 0) {
                    for (int i = 0; i < npx; i++) { if (!zp.read(c3, 3)) return false; tile[i] = cpx24(c3); }
                } else if (se == 1) {
                    if (!zp.read(c3, 3)) return false;
                    const uint16_t v = cpx24(c3);
                    for (int i = 0; i < npx; i++) tile[i] = v;
                } else if (se <= 16) {   // packed palette
                    for (int i = 0; i < se; i++) { if (!zp.read(c3, 3)) return false; pal[i] = cpx24(c3); }
                    const int bpp = se == 2 ? 1 : se <= 4 ? 2 : 4;
                    for (int r = 0; r < th; r++) {
                        int bits = 0, byte = 0;
                        for (int q = 0; q < tw; q++) {
                            if (bits == 0) { byte = zp.u8(); bits = 8; }
                            bits -= bpp;
                            tile[r * tw + q] = pal[(byte >> bits) & ((1 << bpp) - 1)];
                        }
                    }
                    if (zp.bad) return false;
                } else if (se == 128) {  // plain RLE
                    int i = 0;
                    while (i < npx) {
                        if (!zp.read(c3, 3)) return false;
                        const uint16_t v = cpx24(c3);
                        int run = 1, b;
                        do { b = zp.u8(); run += b; } while (b == 255 && !zp.bad);
                        if (zp.bad || i + run > npx) return false;
                        for (int k = 0; k < run; k++) tile[i++] = v;
                    }
                } else if (se >= 130) {  // palette RLE
                    const int ps = se - 128;
                    for (int i = 0; i < ps; i++) { if (!zp.read(c3, 3)) return false; pal[i] = cpx24(c3); }
                    int i = 0;
                    while (i < npx) {
                        const int idx = zp.u8();
                        int run = 1;
                        if (idx & 128) {
                            int b;
                            do { b = zp.u8(); run += b; } while (b == 255 && !zp.bad);
                        }
                        if (zp.bad || i + run > npx || (idx & 127) >= ps) return false;
                        for (int k = 0; k < run; k++) tile[i++] = pal[idx & 127];
                    }
                } else {
                    return false;        // 17..127 / 129 are invalid
                }
                emit_tile(tx, ty, tw, th);
            }
        }
        return true;
    }

    uint32_t compact_len(void) {
        uint32_t v = rd.u8();
        uint32_t n = v & 0x7F;
        if (v & 0x80) {
            v = rd.u8();
            n |= (v & 0x7F) << 7;
            if (v & 0x80) n |= (uint32_t)rd.u8() << 14;
        }
        return n;
    }

    // Tight "basic" data: raw when < 12 bytes, else zlib block on stream `sid`.
    bool tight_data(int sid, size_t size, uint8_t **out) {
        if (size < 12) {
            if (!read_block(size)) return false;
            *out = zbuf;
            return true;
        }
        const uint32_t n = compact_len();
        if (rd.err || !tight[sid].ensure()) return false;
        // Decompress into row32-sized scratch: allocate a plain buffer for the whole rect (rects are
        // capped at 65536 px by every Tight server, i.e. <= 192 KB of TPIXELs).
        static uint8_t *plain = nullptr;
        static size_t plain_cap = 0;
        if (size > plain_cap) {
            heap_caps_free(plain);
            plain_cap = std::max(size, (size_t)256 * 1024);
            plain = (uint8_t *)heap_caps_malloc(plain_cap, MALLOC_CAP_SPIRAM);
            if (!plain) { plain_cap = 0; return false; }
        }
        if (!read_block(n)) return false;
        ZPull zp(&tight[sid], zbuf, n);
        if (!zp.read(plain, size)) return false;
        *out = plain;
        return true;
    }

    bool dec_tight(int x, int y, int w, int h) {
        uint8_t ctl = rd.u8();
        if (rd.err) return false;
        for (int i = 0; i < 4; i++) if (ctl & (1 << i)) tight[i].reset();
        ctl >>= 4;
        if (ctl == 8) {          // fill
            uint8_t p[3];
            if (!rd.read(p, 3)) return false;
            fill(x, y, w, h, tpx24(p));
            return true;
        }
        if (ctl == 9) {          // JPEG
            const uint32_t n = compact_len();
            if (rd.err || !read_block(n)) return false;
            const uint16_t *px;
            int jw, jh, stride;
            if (!nv_ss_decode_jpeg(NV_SS_SRC_VNC, zbuf, n, &px, &jw, &jh, &stride)) return true;   // drop rect
            const int cw = std::min(w, jw), ch = std::min(h, jh);
            for (int r = 0; r < ch; r++) emit(y + r, x, cw, px + (size_t)r * stride);
            return true;
        }
        if (ctl > 7) return false;   // PNG etc. (not advertised)
        const int sid = ctl & 3;
        int filter = 0;
        if (ctl & 4) filter = rd.u8();
        if (filter == 1) {           // palette
            const int n = rd.u8() + 1;
            uint16_t pal[256];
            uint8_t p[3];
            for (int i = 0; i < n; i++) { if (!rd.read(p, 3)) return false; pal[i] = tpx24(p); }
            const size_t rowb = n == 2 ? (size_t)(w + 7) / 8 : (size_t)w;
            uint8_t *data;
            if (!tight_data(sid, rowb * h, &data)) return false;
            for (int r = 0; r < h; r++) {
                if (!map.row_visible(y + r)) continue;
                const uint8_t *s = data + rowb * r;
                for (int q = 0; q < w; q++)
                    row565[q] = n == 2 ? pal[(s[q >> 3] >> (7 - (q & 7))) & 1] : pal[s[q] < n ? s[q] : 0];
                emit(y + r, x, w, row565);
            }
            return true;
        }
        const size_t rowb = (size_t)w * 3;
        uint8_t *data;
        if (!tight_data(sid, rowb * h, &data)) return false;
        if (filter == 2) {           // gradient: predict from left/up/up-left per channel
            memset(prev, 0, (size_t)w * 3 * 2);
            for (int r = 0; r < h; r++) {
                uint8_t *s = data + rowb * r;
                uint16_t left[3] = {0, 0, 0}, upleft[3] = {0, 0, 0};
                for (int q = 0; q < w; q++) {
                    for (int c = 0; c < 3; c++) {
                        const int up = prev[q * 3 + c];
                        int pr = (int)left[c] + up - (int)upleft[c];
                        pr = pr < 0 ? 0 : pr > 255 ? 255 : pr;
                        const uint16_t v = (uint16_t)((s[q * 3 + c] + pr) & 0xFF);
                        upleft[c] = (uint16_t)up;
                        left[c] = v;
                        prev[q * 3 + c] = v;
                        s[q * 3 + c] = (uint8_t)v;
                    }
                }
            }
        } else if (filter != 0) {
            return false;
        }
        for (int r = 0; r < h; r++) {
            if (!map.row_visible(y + r)) continue;
            const uint8_t *s = data + rowb * r;
            for (int q = 0; q < w; q++) row565[q] = tpx24(s + q * 3);
            emit(y + r, x, w, row565);
        }
        return true;
    }

    // ---- client messages
    bool set_pixel_format(void) {
        const uint8_t m[20] = {0, 0, 0, 0, 32, 24, 0 /*little endian*/, 1 /*true colour*/,
                               0, 255, 0, 255, 0, 255, 16, 8, 0, 0, 0, 0};
        return send(m, sizeof m);
    }
    // CopyRect only at 1:1: a scaled copy is approximate (source rows map to fractional panel rows)
    // and scrolling would accumulate misalignment; without it the server re-encodes the area.
    bool set_encodings(void) {
        int32_t e[12];
        int n = 0;
        for (int32_t v : {ENC_TIGHT, ENC_ZRLE, ENC_ZLIB, ENC_HEXTILE}) e[n++] = v;
        if (map.unity) e[n++] = ENC_COPYRECT;
        for (int32_t v : {ENC_RAW, ENC_EXTDESKTOPSIZE, ENC_DESKTOPSIZE, ENC_LASTRECT, ENC_JPEG_Q6, ENC_COMPRESS_6})
            e[n++] = v;
        uint8_t m[4 + sizeof e];
        m[0] = 2; m[1] = 0;
        m[2] = 0; m[3] = (uint8_t)n;
        for (int i = 0; i < n; i++) {
            const uint32_t v = (uint32_t)e[i];
            m[4 + i * 4] = (uint8_t)(v >> 24); m[5 + i * 4] = (uint8_t)(v >> 16);
            m[6 + i * 4] = (uint8_t)(v >> 8);  m[7 + i * 4] = (uint8_t)v;
        }
        return send(m, 4 + (size_t)n * 4);
    }
    bool request(bool incremental) {
        const uint8_t m[10] = {3, (uint8_t)(incremental ? 1 : 0), 0, 0, 0, 0,
                               (uint8_t)(map.rw >> 8), (uint8_t)map.rw, (uint8_t)(map.rh >> 8), (uint8_t)map.rh};
        return send(m, sizeof m);
    }
    bool pointer(uint8_t mask, int rx, int ry) {
        const uint8_t m[6] = {5, mask, (uint8_t)(rx >> 8), (uint8_t)rx, (uint8_t)(ry >> 8), (uint8_t)ry};
        return send(m, sizeof m);
    }
    // Ask a resizable server (Xvnc, GNOME extend mode, krfb-virtualmonitor) for the panel size.
    void ask_panel_size(void) {
        if (resize_asked || !ext_desktop || (map.rw == kW && map.rh == kH)) return;
        resize_asked = true;
        uint8_t m[24] = {251, 0, kW >> 8, kW & 0xFF, kH >> 8, kH & 0xFF, 1, 0};
        m[8] = (uint8_t)(screen_id >> 24); m[9] = (uint8_t)(screen_id >> 16);
        m[10] = (uint8_t)(screen_id >> 8); m[11] = (uint8_t)screen_id;
        m[16] = kW >> 8; m[17] = kW & 0xFF; m[18] = kH >> 8; m[19] = kH & 0xFF;
        send(m, sizeof m);
        NV_LOGI(TAG, "asked the server for %dx%d (extended desktop)", kW, kH);
    }

    bool resize(int w, int h) {
        if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return false;
        const bool was_unity = map.unity, had_map = map.rw > 0;
        if (!map.setup(w, h)) return false;
        if (had_map && began && was_unity != map.unity) set_encodings();   // CopyRect on/off
        { Lk l; s_info.fb_w = w; s_info.fb_h = h; }
        nv_ss_fb_clear(NV_SS_SRC_VNC);
        NV_LOGI(TAG, "desktop %dx%d -> panel %dx%d at %d,%d", w, h, map.dw, map.dh, map.ox, map.oy);
        return true;
    }

    // ---- one FramebufferUpdate
    bool on_update(void) {
        rd.u8();   // padding
        const int n = rd.u16();
        if (rd.err) return false;
        for (int i = 0; i < n || n == 0xFFFF; i++) {
            const int x = rd.u16(), y = rd.u16(), w = rd.u16(), h = rd.u16();
            const int32_t enc = (int32_t)rd.u32();
            if (rd.err) return false;
            if (enc == ENC_LASTRECT) break;
            if (enc == ENC_DESKTOPSIZE) { if (!resize(w, h)) return false; want_full = true; continue; }
            if (enc == ENC_EXTDESKTOPSIZE) {
                const int ns = rd.u8();
                rd.skip(3);
                for (int s = 0; s < ns; s++) {
                    const uint32_t id = rd.u32();
                    rd.skip(12);
                    if (s == 0) screen_id = id;
                }
                if (rd.err) return false;
                ext_desktop = true;
                if (x == 0 || x == 2 || y == 0) {   // server/other-client change, or our request done
                    if (w != map.rw || h != map.rh) { if (!resize(w, h)) return false; want_full = true; }
                }
                ask_panel_size();
                continue;
            }
            if (w == 0 || h == 0) continue;
            if (x + w > map.rw || y + h > map.rh || w > 8192) return false;
            bool ok;
            switch (enc) {
            case ENC_RAW: ok = dec_raw(x, y, w, h); s_enc_name = "Raw"; break;
            case ENC_COPYRECT: ok = dec_copyrect(x, y, w, h); break;
            case ENC_HEXTILE: ok = dec_hextile(x, y, w, h); s_enc_name = "Hextile"; break;
            case ENC_ZLIB: ok = dec_zlib(x, y, w, h); s_enc_name = "Zlib"; break;
            case ENC_ZRLE: ok = dec_zrle(x, y, w, h); s_enc_name = "ZRLE"; break;
            case ENC_TIGHT: ok = dec_tight(x, y, w, h); s_enc_name = "Tight"; break;
            default:
                NV_LOGW(TAG, "unsupported encoding %ld", (long)enc);
                return false;
            }
            if (!ok) { NV_LOGW(TAG, "decode failed (enc %ld, %dx%d)", (long)enc, w, h); return false; }
        }
        nv_ss_stat_update(0);
        return true;
    }

    // ---- handshake; returns false with the error already recorded
    // Apple Remote Desktop authentication (security type 30): Diffie-Hellman over the server's
    // group, MD5(shared secret) as an AES-128 key, the 128-byte user/password block encrypted with
    // it (ECB), followed by our DH public key. Lets a Mac with plain "Screen Sharing" (no separate
    // VNC password) accept the board with the Mac account's credentials.
    bool ard_auth(const char *user, const char *pw) {
        uint8_t gen[2];
        if (!rd.read(gen, 2)) return false;
        const int kl = rd.u16();
        if (rd.err || kl <= 0 || kl > 512) return false;
        uint8_t *m = (uint8_t *)heap_caps_malloc((size_t)kl * 4, MALLOC_CAP_SPIRAM);
        if (!m) return false;
        uint8_t *mod = m, *spub = m + kl, *cpub = m + 2 * kl, *secret = m + 3 * kl;
        bool ok = rd.read(mod, kl) && rd.read(spub, kl);
        mbedtls_mpi G, P, X, Y, S, SP;
        mbedtls_mpi_init(&G); mbedtls_mpi_init(&P); mbedtls_mpi_init(&X);
        mbedtls_mpi_init(&Y); mbedtls_mpi_init(&S); mbedtls_mpi_init(&SP);
        uint8_t key[16] = {}, creds[128] = {}, ct[128];
        if (ok) {
            auto rng = [](void *, unsigned char *o, size_t n) -> int { esp_fill_random(o, n); return 0; };
            ok = mbedtls_mpi_read_binary(&G, gen, 2) == 0 && mbedtls_mpi_read_binary(&P, mod, kl) == 0 &&
                 mbedtls_mpi_read_binary(&SP, spub, kl) == 0 &&
                 mbedtls_mpi_fill_random(&X, (size_t)kl - 1, rng, nullptr) == 0 &&
                 mbedtls_mpi_exp_mod(&Y, &G, &X, &P, nullptr) == 0 &&
                 mbedtls_mpi_exp_mod(&S, &SP, &X, &P, nullptr) == 0 &&
                 mbedtls_mpi_write_binary(&Y, cpub, kl) == 0 &&
                 mbedtls_mpi_write_binary(&S, secret, kl) == 0 &&
                 mbedtls_md5(secret, kl, key) == 0;
        }
        if (ok) {
            esp_fill_random(creds, sizeof creds);   // unused bytes are random, the strings NUL-ended
            const size_t ul = std::min<size_t>(strlen(user), 63), pl = std::min<size_t>(strlen(pw), 63);
            memcpy(creds, user, ul); creds[ul] = 0;
            memcpy(creds + 64, pw, pl); creds[64 + pl] = 0;
            mbedtls_aes_context aes;
            mbedtls_aes_init(&aes);
            ok = mbedtls_aes_setkey_enc(&aes, key, 128) == 0;
            for (int b = 0; ok && b < 8; b++)
                ok = mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, creds + b * 16, ct + b * 16) == 0;
            mbedtls_aes_free(&aes);
        }
        ok = ok && send(ct, sizeof ct) && send(cpub, kl);
        mbedtls_mpi_free(&G); mbedtls_mpi_free(&P); mbedtls_mpi_free(&X);
        mbedtls_mpi_free(&Y); mbedtls_mpi_free(&S); mbedtls_mpi_free(&SP);
        memset(creds, 0, sizeof creds);
        memset(key, 0, sizeof key);
        heap_caps_free(m);
        return ok;
    }

    bool handshake(const char *user, const char *pw) {
        char ver[13] = {};
        if (!rd.read(ver, 12) || memcmp(ver, "RFB ", 4) != 0) {
            set_state(NV_SS_VNC_FAILED, NV_SS_VNC_ERR_PROTOCOL, "");
            return false;
        }
        const int major = atoi(ver + 4), minor = atoi(ver + 8);
        int minor_use = 8;
        if (major == 3 && minor < 7) minor_use = 3;
        else if (major == 3 && minor < 8) minor_use = 7;
        char reply[13];
        snprintf(reply, sizeof reply, "RFB 003.%03d\n", minor_use);
        if (!send(reply, 12)) return false;

        int sec = 0;
        if (minor_use == 3) {
            sec = (int)rd.u32();
        } else {
            const int n = rd.u8();
            if (rd.err) return false;
            if (n == 0) { reason(NV_SS_VNC_ERR_CONNECT); return false; }
            uint8_t types[32];
            const int k = std::min(n, 32);
            if (!rd.read(types, k) || (n > k && !rd.skip(n - k))) return false;
            bool has_none = false, has_vnc = false, has_ard = false;
            for (int i = 0; i < k; i++) {
                has_none |= types[i] == 1; has_vnc |= types[i] == 2; has_ard |= types[i] == 30;
            }
            const bool have_pw = pw && pw[0], have_user = user && user[0];
            sec = (has_vnc && have_pw) ? 2 : (has_ard && have_user && have_pw) ? 30
                : has_none ? 1 : has_vnc ? 2 : has_ard ? 30 : 0;
            if (!sec) {
                char d[48] = "";
                for (int i = 0, o = 0; i < k && o < 40; i++) o += snprintf(d + o, sizeof d - o, "%s%d", i ? "," : "types ", types[i]);
                set_state(NV_SS_VNC_FAILED, NV_SS_VNC_ERR_SECURITY, d);
                return false;
            }
            const uint8_t s = (uint8_t)sec;
            if (!send(&s, 1)) return false;
        }
        if (sec == 0) { reason(NV_SS_VNC_ERR_CONNECT); return false; }
        if (sec == 30) {
            if (!user || !user[0] || !pw || !pw[0]) {
                set_state(NV_SS_VNC_FAILED, NV_SS_VNC_ERR_NEED_PASSWORD, "Mac user + password");
                return false;
            }
            if (!ard_auth(user, pw)) { set_state(NV_SS_VNC_FAILED, NV_SS_VNC_ERR_PROTOCOL, "ARD"); return false; }
        } else if (sec == 2) {
            if (!pw || !pw[0]) { set_state(NV_SS_VNC_FAILED, NV_SS_VNC_ERR_NEED_PASSWORD, ""); return false; }
            uint8_t ch[16], resp[16];
            if (!rd.read(ch, 16)) return false;
            ss_vnc_auth_response(pw, ch, resp);
            if (!send(resp, 16)) return false;
        } else if (sec != 1) {
            set_state(NV_SS_VNC_FAILED, NV_SS_VNC_ERR_SECURITY, "");
            return false;
        }
        if (sec != 1 || minor_use == 8) {   // SecurityResult (3.3/3.7 skip it for None)
            const uint32_t res = rd.u32();
            if (rd.err) return false;
            if (res != 0) {
                if (minor_use == 8) reason(NV_SS_VNC_ERR_AUTH);
                else set_state(NV_SS_VNC_FAILED, NV_SS_VNC_ERR_AUTH, "");
                return false;
            }
        }
        const uint8_t shared = 1;
        if (!send(&shared, 1)) return false;
        const int w = rd.u16(), h = rd.u16();
        rd.skip(16);   // server pixel format (we set ours)
        const uint32_t nl = rd.u32();
        if (rd.err || nl > 4096) return false;
        char name[64] = {};
        const size_t take = std::min<size_t>(nl, sizeof name - 1);
        if (!rd.read(name, take) || !rd.skip(nl - take)) return false;
        snprintf(desktop, sizeof desktop, "%s", name);
        { Lk l; snprintf(s_info.desktop, sizeof s_info.desktop, "%s", name); }
        return resize(w, h) || (set_state(NV_SS_VNC_FAILED, NV_SS_VNC_ERR_MEMORY, ""), false);
    }

    void reason(nv_ss_vnc_err_t e) {   // u32 length + text
        const uint32_t n = rd.u32();
        char t[64] = {};
        if (!rd.err && n < 1024) {
            const size_t take = std::min<size_t>(n, sizeof t - 1);
            rd.read(t, take);
            rd.skip(n - take);
        }
        set_state(NV_SS_VNC_FAILED, e, t);
    }

    // ---- touch -> pointer (runs on the engine's touch task)
    void on_touch(const nv_ss_touch_pt_t *p, int n) {
        if (n >= 2) {
            const int cy = (p[0].y + p[1].y) / 2;
            if (!two) {
                two = true; two_moved = false; two_y0 = cy; two_acc = 0;
                if (buttons & 1) { buttons = 0; pointer(0, last_x, last_y); }   // cancel the drag
                last_x = map.to_remote_x(p[0].x); last_y = map.to_remote_y(p[0].y);
                return;
            }
            const int dy = cy - two_y0;
            two_y0 = cy;
            two_acc += dy;
            while (two_acc >= 36 || two_acc <= -36) {   // one wheel notch per 36 px
                const uint8_t b = two_acc > 0 ? 8 : 16;    // finger down = scroll up (natural)
                pointer(b, last_x, last_y);
                pointer(0, last_x, last_y);
                two_acc += two_acc > 0 ? -36 : 36;
                two_moved = true;
            }
            return;
        }
        if (two) {   // second finger left: tap = right click, scroll ends
            if (n == 0) {
                if (!two_moved) { pointer(4, last_x, last_y); pointer(0, last_x, last_y); }
                two = false;
            }
            return;
        }
        if (n == 1) {
            last_x = map.to_remote_x(p[0].x);
            last_y = map.to_remote_y(p[0].y);
            buttons = 1;
            pointer(1, last_x, last_y);
        } else if (buttons) {
            buttons = 0;
            pointer(0, last_x, last_y);
        }
    }
};

Session *volatile s_sess = nullptr;   // the live session (touch/pause ops reach it)

void op_touch(const nv_ss_touch_pt_t *p, int n, void *) { Session *s = s_sess; if (s) s->on_touch(p, n); }
void op_pause(void *) { Session *s = s_sess; if (s) s->paused = true; }
void op_resume(void *) { Session *s = s_sess; if (s) { s->paused = false; s->want_full = true; } }
void op_stop(bool, void *) { Session *s = s_sess; if (s) s->stop = true; }
bool op_alive(void *) { Session *s = s_sess; return s && !s->stop; }
const nv_ss_source_ops_t kOps = {op_touch, op_pause, op_resume, op_stop, op_alive, nullptr};

// ---------------------------------------------------------------- networking helpers
bool resolve(const char *host, struct in_addr *out) {
    if (inet_aton(host, out)) return true;
    const size_t l = strlen(host);
    if (l > 6 && strcasecmp(host + l - 6, ".local") == 0) {
        char name[40];
        snprintf(name, sizeof name, "%.*s", (int)(l - 6), host);
        esp_ip4_addr_t a = {};
        if (mdns_query_a(name, 2500, &a) == ESP_OK) { out->s_addr = a.addr; return true; }
        return false;
    }
    struct addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, nullptr, &hints, &res) != 0 || !res) return false;
    *out = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return true;
}

int connect_timeout(struct in_addr ip, uint16_t port, int ms) {
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;
    const int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    struct sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr = ip;
    int r = connect(fd, (struct sockaddr *)&a, sizeof a);
    if (r != 0 && errno != EINPROGRESS) { close(fd); return -1; }
    if (r != 0) {
        fd_set ws;
        FD_ZERO(&ws);
        FD_SET(fd, &ws);
        struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
        if (select(fd + 1, nullptr, &ws, nullptr, &tv) <= 0) { close(fd); return -1; }
        int err = 0;
        socklen_t el = sizeof err;
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err) { close(fd); return -1; }
    }
    fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

// ---------------------------------------------------------------- one session
void run_session(int fd, const char *host, const char *user, const char *pw) {
    auto *s = new (std::nothrow) Session();
    if (!s || !s->alloc()) {
        delete s;
        if (fd >= 0) close(fd);
        set_state(NV_SS_VNC_FAILED, NV_SS_VNC_ERR_MEMORY, "");
        return;
    }
    s->c.fd = fd;
    snprintf(s->c.peer_ip, sizeof s->c.peer_ip, "%s", host);
    s->c.set_timeout_ms(1000);
    s_enc_name = "";

    bool ok = s->handshake(user, pw);
    if (ok) {
        // Needs the app page open (it owns the panel). A reverse connection may arrive while the
        // app is closed: bring it forward and give it a few seconds.
        if (!nv_ss_is_open()) {
            ss_request_foreground();
            for (int i = 0; i < 40 && !nv_ss_is_open(); i++) vTaskDelay(pdMS_TO_TICKS(250));
        }
        char peer[48];
        snprintf(peer, sizeof peer, "%s", s->desktop[0] ? s->desktop : host);
        s_sess = s;
        ok = nv_ss_begin(NV_SS_SRC_VNC, &kOps, peer);
        if (!ok) set_state(NV_SS_VNC_FAILED, NV_SS_VNC_ERR_CLOSED, "display busy");
    }
    if (ok) {
        s->began = true;
        set_state(NV_SS_VNC_RUNNING, NV_SS_VNC_ERR_NONE, "");
        NV_LOGI(TAG, "connected: %s (%s) %dx%d", s->desktop, host, s->map.rw, s->map.rh);
        ok = s->set_pixel_format() && s->set_encodings() && s->request(false);
        bool pending = true;   // one update request in flight
        while (ok && !s->stop) {
            const int r = s->rd.wait_message();
            if (r == -2) {   // idle second: re-request if a pause ended / a full repaint is due
                if (!s->paused && (s->want_full || !pending)) {
                    ok = s->request(!s->want_full);
                    s->want_full = false;
                    pending = true;
                }
                continue;
            }
            if (r == 0) { ok = false; break; }
            const uint8_t type = s->rd.u8();
            switch (type) {
            case 0:
                ok = s->on_update();
                pending = false;
                if (ok && !s->paused) {
                    ok = s->request(!s->want_full);
                    s->want_full = false;
                    pending = true;
                }
                { Lk l; s_info.encoding = s_enc_name; }
                break;
            case 1: {   // SetColourMapEntries: we use true colour, skip it
                s->rd.skip(3);
                const int n = s->rd.u16();
                ok = s->rd.skip((size_t)n * 6);
                break;
            }
            case 2: break;   // Bell
            case 3: {        // ServerCutText
                s->rd.skip(3);
                const uint32_t n = s->rd.u32();
                ok = !s->rd.err && n < 16u * 1024 * 1024 && s->rd.skip(n);
                break;
            }
            case 150:        // EndOfContinuousUpdates (not requested; harmless)
                break;
            default:
                NV_LOGW(TAG, "unknown server message %u", type);
                ok = false;
            }
            ok = ok && !s->rd.err;
        }
        // A session that ran and was then closed by the server (shutdown, user left) is not a
        // failed method: only errors before the desktop appeared count as FAILED for the wizard.
        set_state(NV_SS_VNC_OFF, s->stop ? NV_SS_VNC_ERR_NONE : NV_SS_VNC_ERR_CLOSED, "");
    } else if (s_info.state != NV_SS_VNC_FAILED) {
        set_state(NV_SS_VNC_FAILED, NV_SS_VNC_ERR_PROTOCOL, "");
    }
    s->stop = true;
    if (s->began) nv_ss_end(NV_SS_SRC_VNC, "VNC closed");
    s_sess = nullptr;
    vTaskDelay(pdMS_TO_TICKS(50));   // a touch op that grabbed s_sess before the reset finishes
    s->c.close();
    delete s;
    NV_LOGI(TAG, "session ended");
}

// ---------------------------------------------------------------- discovery
void discover(void) {
    s_discovering = true;
    mdns_result_t *res = nullptr;
    nv_ss_vnc_server_t found[8];
    int n = 0;
    if (mdns_init() == ESP_OK && mdns_query_ptr("_rfb", "_tcp", 2500, 8, &res) == ESP_OK) {
        for (mdns_result_t *r = res; r && n < 8; r = r->next) {
            for (mdns_ip_addr_t *a = r->addr; a; a = a->next) {
                if (a->addr.type != ESP_IPADDR_TYPE_V4) continue;
                nv_ss_vnc_server_t &f = found[n];
                snprintf(f.name, sizeof f.name, "%s", r->instance_name ? r->instance_name : (r->hostname ? r->hostname : ""));
                snprintf(f.host, sizeof f.host, IPSTR, IP2STR(&a->addr.u_addr.ip4));
                f.port = r->port ? r->port : 5900;
                bool dup = false;
                for (int i = 0; i < n; i++) dup |= strcmp(found[i].host, f.host) == 0 && found[i].port == f.port;
                if (!dup) n++;
                break;
            }
        }
        mdns_query_results_free(res);
    }
    {
        Lk l;
        memcpy(s_found, found, sizeof(nv_ss_vnc_server_t) * n);
        s_found_n = n;
    }
    s_discovering = false;
}

// ---------------------------------------------------------------- task
void vnc_task(void *) {
    if (!ss_des_selftest()) NV_LOGE(TAG, "DES self-test FAILED: VNC passwords won't work");
    int lfd = -1;
    for (;;) {
        Cmd cmd;
        if (xQueueReceive(s_q, &cmd, 0) == pdTRUE) {
            set_state(NV_SS_VNC_CONNECTING, NV_SS_VNC_ERR_NONE, "");
            { Lk l; snprintf(s_info.host, sizeof s_info.host, "%s", cmd.host); s_info.desktop[0] = 0; }
            struct in_addr ip;
            int fd = -1;
            if (resolve(cmd.host, &ip)) fd = connect_timeout(ip, cmd.port, 6000);
            if (fd < 0) set_state(NV_SS_VNC_FAILED, NV_SS_VNC_ERR_CONNECT, "");
            else run_session(fd, cmd.host, cmd.user, cmd.pw);
            memset(cmd.pw, 0, sizeof cmd.pw);
            continue;
        }
        if (s_discover_req) { s_discover_req = false; discover(); continue; }

        // reverse connections (server-initiated): listen only while wanted
        if (s_listen_want && lfd < 0) {
            lfd = ss_listen_tcp(NV_SS_VNC_LISTEN_PORT, 2);
            Lk l;
            s_info.listening = lfd >= 0;
        } else if (!s_listen_want && lfd >= 0) {
            close(lfd);
            lfd = -1;
            Lk l;
            s_info.listening = false;
        }
        if (lfd < 0) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(lfd, &rs);
        struct timeval tv = {0, 200000};
        if (select(lfd + 1, &rs, nullptr, nullptr, &tv) > 0 && FD_ISSET(lfd, &rs)) {
            struct sockaddr_in a = {};
            socklen_t al = sizeof a;
            const int cfd = accept(lfd, (struct sockaddr *)&a, &al);
            if (cfd < 0) continue;
            char host[16];
            inet_ntoa_r(a.sin_addr, host, sizeof host);
            int one = 1;
            setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            NV_LOGI(TAG, "reverse connection from %s", host);
            set_state(NV_SS_VNC_CONNECTING, NV_SS_VNC_ERR_NONE, "");
            { Lk l; snprintf(s_info.host, sizeof s_info.host, "%s", host); }
            // Reverse viewers can't prompt: the server side must not require a password, or use
            // the one remembered for that host by the wizard (not available here) -> none.
            run_session(cfd, host, "", "");
        }
    }
}

}  // namespace

// ================================================================ internal / public
void ss_vnc_init(void) {
    if (s_q) return;
    s_lk = xSemaphoreCreateMutex();
    s_q = xQueueCreate(1, sizeof(Cmd));
    // Forever task; NVS never touched from it. Deep stack: handshake + decoders + mdns queries.
    xTaskCreateWithCaps(vnc_task, "ss_vnc", 12288, nullptr, 4, nullptr, MALLOC_CAP_SPIRAM);
}

void nv_ss_vnc_info(nv_ss_vnc_info_t *out) {
    if (!out) return;
    if (!s_lk) { memset(out, 0, sizeof *out); return; }
    Lk l;
    *out = s_info;
    out->encoding = s_enc_name;
}

bool nv_ss_vnc_connect(const char *host, uint16_t port, const char *user, const char *password) {
    if (!s_q || !host || !host[0]) return false;
    {
        Lk l;
        if (s_info.state == NV_SS_VNC_CONNECTING || s_info.state == NV_SS_VNC_RUNNING) return false;
        s_info.state = NV_SS_VNC_CONNECTING;
        s_info.err = NV_SS_VNC_ERR_NONE;
        s_info.detail[0] = 0;
        snprintf(s_info.host, sizeof s_info.host, "%s", host);
    }
    Cmd c = {};
    snprintf(c.host, sizeof c.host, "%s", host);
    c.port = port ? port : 5900;
    snprintf(c.user, sizeof c.user, "%s", user ? user : "");
    snprintf(c.pw, sizeof c.pw, "%s", password ? password : "");
    return xQueueSend(s_q, &c, 0) == pdTRUE;
}

void nv_ss_vnc_disconnect(void) {
    Session *s = s_sess;
    if (s) s->stop = true;
}

void nv_ss_vnc_discover(void) { s_discover_req = true; }
bool nv_ss_vnc_discovering(void) { return s_discovering || s_discover_req; }

int nv_ss_vnc_discovered(nv_ss_vnc_server_t *out, int max) {
    if (!s_lk || !out) return 0;
    Lk l;
    const int n = std::min(max, s_found_n);
    memcpy(out, s_found, sizeof(nv_ss_vnc_server_t) * n);
    return n;
}

void nv_ss_vnc_set_listen(bool on) { s_listen_want = on; }
