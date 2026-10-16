// dmd_core — see dmd_core.h. Pure C: no ESP-IDF, FreeRTOS or LVGL (tests/host builds it as is).
#include "dmd_core.h"

#include <stdio.h>
#include <string.h>

// ================================================================== SHA-1 + base64 (handshake only)

typedef struct {
    uint32_t h[5];
    uint64_t len;
    uint8_t  blk[64];
    size_t   n;
} sha1_t;

static uint32_t rol(uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }

static void sha1_block(sha1_t *c, const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & cc) | (~b & d);           k = 0x5A827999u; }
        else if (i < 40) { f = b ^ cc ^ d;                    k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDCu; }
        else             { f = b ^ cc ^ d;                    k = 0xCA62C1D6u; }
        const uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = rol(b, 30); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

static void sha1_init(sha1_t *c) {
    static const uint32_t iv[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    memcpy(c->h, iv, sizeof iv);
    c->len = 0;
    c->n = 0;
}

static void sha1_update(sha1_t *c, const uint8_t *p, size_t n) {
    c->len += n;
    while (n) {
        size_t take = 64 - c->n;
        if (take > n) take = n;
        memcpy(c->blk + c->n, p, take);
        c->n += take; p += take; n -= take;
        if (c->n == 64) { sha1_block(c, c->blk); c->n = 0; }
    }
}

static void sha1_final(sha1_t *c, uint8_t out[20]) {
    const uint64_t bits = c->len * 8;
    const uint8_t pad = 0x80, zero = 0;
    sha1_update(c, &pad, 1);
    while (c->n != 56) sha1_update(c, &zero, 1);
    uint8_t lb[8];
    for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_update(c, lb, 8);
    for (int i = 0; i < 5; i++) {
        out[4 * i] = (uint8_t)(c->h[i] >> 24); out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8); out[4 * i + 3] = (uint8_t)c->h[i];
    }
}

void dmd_ws_accept(const char *key, size_t key_len, char out[29]) {
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    sha1_t c;
    uint8_t d[21];
    sha1_init(&c);
    sha1_update(&c, (const uint8_t *)key, key_len);
    sha1_update(&c, (const uint8_t *)guid, sizeof guid - 1);
    sha1_final(&c, d);
    d[20] = 0;
    int o = 0;
    for (int i = 0; i < 21; i += 3) {          // 20 bytes: 6 full groups + one of 2 bytes
        const uint32_t v = (uint32_t)d[i] << 16 | (uint32_t)d[i + 1] << 8 | (i + 2 < 21 ? d[i + 2] : 0);
        out[o++] = b64[(v >> 18) & 63];
        out[o++] = b64[(v >> 12) & 63];
        out[o++] = i + 1 < 20 ? b64[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < 20 ? b64[v & 63] : '=';
    }
    out[28] = 0;
}

// ================================================================== HTTP upgrade

static int lower(int ch) { return ch >= 'A' && ch <= 'Z' ? ch + 32 : ch; }

static bool ci_eq(const char *a, size_t an, const char *b) {
    const size_t bn = strlen(b);
    if (an != bn) return false;
    for (size_t i = 0; i < an; i++)
        if (lower((unsigned char)a[i]) != lower((unsigned char)b[i])) return false;
    return true;
}

static bool ci_contains(const char *a, size_t an, const char *b) {
    const size_t bn = strlen(b);
    for (size_t i = 0; i + bn <= an; i++)
        if (ci_eq(a + i, bn, b)) return true;
    return false;
}

#define HS_MAX 4096   // a real upgrade request is a few hundred bytes

static size_t bad_request(char *resp, size_t cap) {
    const int n = snprintf(resp, cap, "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

dmd_hs_t dmd_ws_handshake(const char *req, size_t n, size_t *consumed, char *resp, size_t cap,
                          size_t *resp_len) {
    *consumed = 0;
    *resp_len = 0;
    if (cap) resp[0] = 0;
    size_t end = 0;
    for (size_t i = 0; i + 4 <= n && i + 4 <= HS_MAX; i++)
        if (req[i] == '\r' && req[i + 1] == '\n' && req[i + 2] == '\r' && req[i + 3] == '\n') { end = i + 4; break; }
    if (!end) {
        if (n < HS_MAX) return DMD_HS_INCOMPLETE;
        *resp_len = bad_request(resp, cap);
        return DMD_HS_BAD;
    }
    *consumed = end;
    bool upgrade = false, version_ok = true;
    const char *key = NULL;
    size_t key_len = 0;
    if (end < 5 || memcmp(req, "GET ", 4) != 0) { *resp_len = bad_request(resp, cap); return DMD_HS_BAD; }
    size_t i = 0;
    while (i < end && req[i] != '\n') i++;   // skip the request line
    i++;
    while (i < end) {
        size_t e = i;
        while (e < end && req[e] != '\n') e++;
        size_t le = e;
        if (le > i && req[le - 1] == '\r') le--;
        const char *colon = memchr(req + i, ':', le - i);
        if (colon) {
            const size_t nl = (size_t)(colon - (req + i));
            const char *v = colon + 1;
            const char *ve = req + le;
            while (v < ve && (*v == ' ' || *v == '\t')) v++;
            while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
            const size_t vl = (size_t)(ve - v);
            if (ci_eq(req + i, nl, "upgrade") && ci_contains(v, vl, "websocket")) upgrade = true;
            else if (ci_eq(req + i, nl, "sec-websocket-key")) { key = v; key_len = vl; }
            else if (ci_eq(req + i, nl, "sec-websocket-version")) version_ok = vl == 2 && v[0] == '1' && v[1] == '3';
        }
        i = e + 1;
    }
    if (!upgrade || !key || key_len == 0 || key_len > 64 || !version_ok) {
        *resp_len = bad_request(resp, cap);
        return DMD_HS_BAD;
    }
    char acc[29];
    dmd_ws_accept(key, key_len, acc);
    const int r = snprintf(resp, cap,
                           "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                           "Sec-WebSocket-Accept: %s\r\n\r\n", acc);
    if (r <= 0 || (size_t)r >= cap) return DMD_HS_BAD;
    *resp_len = (size_t)r;
    return DMD_HS_OK;
}

// ================================================================== WebSocket frames

void dmd_ws_init(dmd_ws_t *ws, uint8_t *msg_buf, size_t cap) {
    memset(ws, 0, sizeof *ws);
    ws->msg = msg_buf;
    ws->msg_cap = cap;
}

dmd_ws_ev_t dmd_ws_next(dmd_ws_t *ws, uint8_t *buf, size_t n, size_t *used) {
    *used = 0;
    if (n < 2) return DMD_WS_MORE;
    const bool fin = buf[0] & 0x80;
    const uint8_t op = buf[0] & 0x0F;
    if (buf[0] & 0x70) return DMD_WS_ERROR;            // no extension was negotiated
    if (!(buf[1] & 0x80)) return DMD_WS_ERROR;         // client frames must be masked
    uint64_t len = buf[1] & 0x7F;
    size_t hdr = 2;
    if (len == 126) {
        if (n < 4) return DMD_WS_MORE;
        len = (uint64_t)buf[2] << 8 | buf[3];
        hdr = 4;
    } else if (len == 127) {
        if (n < 10) return DMD_WS_MORE;
        len = 0;
        for (int i = 0; i < 8; i++) len = len << 8 | buf[2 + i];
        hdr = 10;
    }
    const bool control = op & 0x08;
    if (control && (!fin || len > 125)) return DMD_WS_ERROR;
    if (!control && len > ws->msg_cap) return DMD_WS_ERROR;   // checked before waiting for it
    if (n < hdr + 4) return DMD_WS_MORE;
    const uint8_t *mk = buf + hdr;
    hdr += 4;
    if (n - hdr < len) return DMD_WS_MORE;
    const size_t pl = (size_t)len;
    uint8_t *p = buf + hdr;
    for (size_t i = 0; i < pl; i++) p[i] ^= mk[i & 3];
    *used = hdr + pl;

    switch (op) {
    case 0x8: return DMD_WS_CLOSE;
    case 0x9:
        memcpy(ws->ctl, p, pl);
        ws->ctl_len = pl;
        return DMD_WS_PING;
    case 0xA: return DMD_WS_CONTROL;
    case 0x1:
    case 0x2:
        if (ws->in_msg) return DMD_WS_ERROR;            // a new message inside a fragmented one
        ws->msg_len = 0;
        ws->done = false;
        break;
    case 0x0:
        if (!ws->in_msg) return DMD_WS_ERROR;           // continuation of nothing
        break;
    default: return DMD_WS_ERROR;
    }
    if (pl > ws->msg_cap - ws->msg_len) return DMD_WS_ERROR;
    memcpy(ws->msg + ws->msg_len, p, pl);
    ws->msg_len += pl;
    ws->in_msg = !fin;
    if (!fin) return DMD_WS_CONTROL;
    ws->done = true;
    return DMD_WS_MESSAGE;
}

size_t dmd_ws_header(uint8_t opcode, size_t len, uint8_t out[10]) {
    out[0] = (uint8_t)(0x80 | (opcode & 0x0F));
    if (len < 126) { out[1] = (uint8_t)len; return 2; }
    if (len <= 0xFFFF) { out[1] = 126; out[2] = (uint8_t)(len >> 8); out[3] = (uint8_t)len; return 4; }
    out[1] = 127;
    const uint64_t l = len;
    for (int i = 0; i < 8; i++) out[2 + i] = (uint8_t)(l >> (56 - 8 * i));
    return 10;
}

// ================================================================== dmdext messages

#define DMD_DEFAULT_COLOR 0xFF4500u   // System.Windows.Media.Colors.OrangeRed, dmdext's default

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void dmd_state_init(dmd_state_t *s) {
    memset(s, 0, sizeof *s);
    s->color = DMD_DEFAULT_COLOR;
    s->color_override = -1;
}

const char *dmd_fmt_name(dmd_fmt_t f) {
    switch (f) {
    case DMD_FMT_GRAY2:  return "gray2";
    case DMD_FMT_GRAY4:  return "gray4";
    case DMD_FMT_CGRAY2: return "coloredGray2";
    case DMD_FMT_CGRAY4: return "coloredGray4";
    case DMD_FMT_RGB24:  return "rgb24";
    default:             return "-";
    }
}

static uint32_t scale_rgb(uint32_t c, int num, int den) {
    const uint32_t r = ((c >> 16) & 0xFF) * (uint32_t)num / (uint32_t)den;
    const uint32_t g = ((c >> 8) & 0xFF) * (uint32_t)num / (uint32_t)den;
    const uint32_t b = (c & 0xFF) * (uint32_t)num / (uint32_t)den;
    return r << 16 | g << 8 | b;
}

// Colour of each of the `levels` gray levels: from a palette (resampled when its size differs, as
// dmdext's ColorUtil does), else the monochrome colour scaled by level / (levels - 1).
static void level_colors(const dmd_state_t *s, const uint32_t *pal, int pal_n, int levels, uint32_t *lut) {
    for (int l = 0; l < levels; l++) {
        if (pal_n > 0) {
            const int idx = pal_n == levels ? l : (l * (pal_n - 1) + (levels - 1) / 2) / (levels - 1);
            lut[l] = pal[idx] & 0xFFFFFF;
        } else {
            const uint32_t mono = s->color_override >= 0 ? (uint32_t)s->color_override : s->color;
            lut[l] = scale_rgb(mono, l, levels - 1);
        }
    }
}

// planes: `bits` planes of w*h/8 bytes each, plane k = bit k of the level, LSB = leftmost pixel.
static void decode_planes(const uint8_t *planes, int bits, int w, int h, const uint32_t *lut, uint32_t *out) {
    const size_t px = (size_t)w * (size_t)h;
    const size_t plane = px / 8;
    for (size_t p = 0; p < px; p++) {
        const size_t byte = p >> 3;
        unsigned lvl = 0;
        if (byte < plane)
            for (int k = 0; k < bits; k++) lvl |= ((planes[(size_t)k * plane + byte] >> (p & 7)) & 1u) << k;
        out[p] = lut[lvl];
    }
}

static bool name_is(const uint8_t *m, size_t nl, const char *s) { return nl == strlen(s) && !memcmp(m, s, nl); }

dmd_msg_t dmd_proto_feed(dmd_state_t *s, const uint8_t *m, size_t n, uint32_t *out, size_t out_cap) {
    size_t nl = 0;
    while (nl < n && nl < 32 && m[nl]) nl++;
    if (nl >= n || m[nl] != 0) return DMD_MSG_BAD;     // no "<name>\0" header
    const uint8_t *p = m + nl + 1;
    const size_t pn = n - nl - 1;

    if (name_is(m, nl, "gameName")) {
        size_t o = 0;
        for (size_t i = 0; i < pn && p[i] && o + 1 < sizeof s->game; i++)
            if (p[i] >= 0x20 && p[i] < 0x7F) s->game[o++] = (char)p[i];
        s->game[o] = 0;
        return DMD_MSG_META;
    }
    if (name_is(m, nl, "dimensions")) {
        if (pn < 8) return DMD_MSG_BAD;
        const uint32_t w = le32(p), h = le32(p + 4);
        if (w < 1 || h < 1 || w > DMD_MAX_W || h > DMD_MAX_H || w * h > DMD_MAX_PX) {
            s->w = s->h = 0;     // frames are dropped until valid dimensions arrive
            return DMD_MSG_BAD;
        }
        s->w = (int)w;
        s->h = (int)h;
        return DMD_MSG_META;
    }
    if (name_is(m, nl, "color")) {
        if (pn < 4) return DMD_MSG_BAD;
        s->color = le32(p) & 0xFFFFFF;
        return DMD_MSG_META;
    }
    if (name_is(m, nl, "palette")) {
        if (pn < 4) return DMD_MSG_BAD;
        const uint32_t c = le32(p);
        if (c < 1 || c > DMD_PAL_MAX || pn - 4 < (size_t)c * 4) return DMD_MSG_BAD;
        for (uint32_t i = 0; i < c; i++) s->pal[i] = le32(p + 4 + 4 * i) & 0xFFFFFF;
        s->pal_n = (int)c;
        return DMD_MSG_META;
    }
    if (name_is(m, nl, "clearColor"))   { s->color = DMD_DEFAULT_COLOR; return DMD_MSG_META; }
    if (name_is(m, nl, "clearPalette")) { s->pal_n = 0; return DMD_MSG_META; }

    int bits = 0;
    bool colored = false, rgb = false;
    dmd_fmt_t fmt = DMD_FMT_NONE;
    if (name_is(m, nl, "gray2Planes"))       { bits = 2; fmt = DMD_FMT_GRAY2; }
    else if (name_is(m, nl, "gray4Planes"))  { bits = 4; fmt = DMD_FMT_GRAY4; }
    else if (name_is(m, nl, "coloredGray2")) { bits = 2; colored = true; fmt = DMD_FMT_CGRAY2; }
    else if (name_is(m, nl, "coloredGray4")) { bits = 4; colored = true; fmt = DMD_FMT_CGRAY4; }
    else if (name_is(m, nl, "rgb24"))        { rgb = true; fmt = DMD_FMT_RGB24; }
    else return DMD_MSG_IGNORED;

    if (s->w < 1 || s->h < 1) return DMD_MSG_BAD;
    const size_t px = (size_t)s->w * (size_t)s->h;
    if (px > out_cap) return DMD_MSG_BAD;
    if (pn < 4) return DMD_MSG_BAD;                     // uint32 timestamp
    const uint8_t *d = p + 4;
    size_t dn = pn - 4;

    if (rgb) {
        if (dn < px * 3) return DMD_MSG_BAD;
        for (size_t i = 0; i < px; i++)
            out[i] = (uint32_t)d[3 * i] << 16 | (uint32_t)d[3 * i + 1] << 8 | d[3 * i + 2];
        s->last_fmt = fmt;
        return DMD_MSG_FRAME;
    }

    uint32_t pal[DMD_PAL_MAX];
    int pal_n = s->pal_n;
    const uint32_t *pp = s->pal;
    if (colored) {
        if (dn < 4) return DMD_MSG_BAD;
        const uint32_t c = le32(d);
        if (c < 1 || c > DMD_PAL_MAX || dn - 4 < (size_t)c * 4) return DMD_MSG_BAD;
        for (uint32_t i = 0; i < c; i++) pal[i] = le32(d + 4 + 4 * i);
        pal_n = (int)c;
        pp = pal;
        d += 4 + 4 * (size_t)c;
        dn -= 4 + 4 * (size_t)c;
    }
    if (dn < (px / 8) * (size_t)bits) return DMD_MSG_BAD;
    uint32_t lut[16];
    level_colors(s, pp, pal_n, 1 << bits, lut);
    decode_planes(d, bits, s->w, s->h, lut, out);
    s->last_fmt = fmt;
    return DMD_MSG_FRAME;
}

// ================================================================== ZeDMD

static const uint8_t kFrame[5] = {'F', 'R', 'A', 'M', 'E'};
static const uint8_t kCtrl[6] = {'Z', 'e', 'D', 'M', 'D', 'A'};

void dmd_zedmd_reset(dmd_zedmd_t *z) {
    z->st = 0;
    z->match = 0;
    z->chunk_left = 0;
    z->done = z->err = false;
    z->ctrl = z->hdr = 0;
}

void dmd_zedmd_init(dmd_zedmd_t *z, int w, int h, uint16_t chunk, uint32_t *img, dmd_inflate_fn inflate, void *ctx) {
    memset(z, 0, offsetof(dmd_zedmd_t, pay));
    z->w = w;
    z->h = h;
    z->zw = w / 16;
    z->zh = h / 8;
    z->chunk = chunk < 32 ? 32 : chunk > 1920 ? 1920 : chunk;
    z->img = img;
    z->inflate = inflate;
    z->inflate_ctx = ctx;
    if (img) memset(img, 0, (size_t)w * (size_t)h * 4);
    dmd_zedmd_reset(z);
}

static void put(uint8_t *out, size_t cap, size_t *len, const void *p, size_t n) {
    if (n > cap - *len) return;   // the caller sized out for every reply a feed can produce
    memcpy(out + *len, p, n);
    *len += n;
}

static void hello(const dmd_zedmd_t *z, uint8_t *out, size_t cap, size_t *len) {
    uint8_t r[64] = {0};
    memcpy(r, kCtrl, 4);                                   // "ZeDM"
    r[4] = (uint8_t)z->w; r[5] = (uint8_t)(z->w >> 8);
    r[6] = (uint8_t)z->h; r[7] = (uint8_t)(z->h >> 8);
    r[8] = 5; r[9] = 1; r[10] = 8;                         // firmware 5.1.8 (libzedmd: major != 0)
    r[11] = (uint8_t)z->chunk; r[12] = (uint8_t)(z->chunk >> 8);
    r[13] = 8;                                             // brightness (ours is the app's own setting)
    r[18] = 8;                                             // panel i2s speed: libzedmd clamps < 8 up anyway
    r[19] = 2;                                             // latch blanking
    r[20] = 60;                                            // min refresh: no "too low" warning on the PC
    r[22] = 0x02;                                          // flags: not half, S3-class (USB CDC)
    r[23] = 0x01;                                          // id
    r[57] = 'R';
    memcpy(r + 58, kCtrl, 6);                              // + "ZeDMDA": 64 bytes, as the firmware sends
    put(out, cap, len, r, 64);
}

// One zones packet (already inflated): [idx][zw*zh*2 LE RGB565] or [idx+128] = a black zone.
static bool zones(dmd_zedmd_t *z, const uint8_t *d, size_t n) {
    const size_t zb = (size_t)z->zw * (size_t)z->zh * 2;
    size_t pos = 0;
    while (pos < n) {
        const uint8_t i = d[pos++];
        const int idx = i & 127;
        const int x0 = (idx % 16) * z->zw, y0 = (idx / 16) * z->zh;
        if (i >= 128) {
            for (int y = 0; y < z->zh; y++)
                memset(z->img + (size_t)(y0 + y) * z->w + x0, 0, (size_t)z->zw * 4);
            continue;
        }
        if (n - pos < zb) return false;
        for (int y = 0; y < z->zh; y++)
            for (int x = 0; x < z->zw; x++, pos += 2) {
                const uint16_t c = (uint16_t)(d[pos] | d[pos + 1] << 8);
                uint32_t r = (c >> 8) & 0xF8, g = (c >> 3) & 0xFC, b = (c << 3) & 0xF8;
                r |= r >> 5; g |= g >> 6; b |= b >> 5;          // full-scale, as the firmware expands
                z->img[(size_t)(y0 + y) * z->w + x0 + x] = r << 16 | g << 8 | b;
            }
    }
    return true;
}

// A command's header is complete (or its payload): act on it. Returns DMD_Z_EV_* bits.
static int command(dmd_zedmd_t *z, uint8_t *out, size_t cap, size_t *len) {
    int ev = 0;
    switch (z->cmd) {
    case 0x0C:                                             // handshake: answer at once, no chunk ack
        hello(z, out, cap, len);
        dmd_zedmd_reset(z);
        return DMD_Z_EV_HELLO;
    case 0x05:
        if (z->got < z->size) return 0;                    // payload still coming
        if (z->comp) {
            const int k = z->inflate ? z->inflate(z->inflate_ctx, z->pay, z->size, z->raw, sizeof z->raw) : -1;
            if (k < 0 || !zones(z, z->raw, (size_t)k)) { z->err = true; return DMD_Z_EV_ERROR; }
        } else if (!zones(z, z->pay, z->size)) {
            z->err = true;
            return DMD_Z_EV_ERROR;
        }
        z->ctrl = z->hdr = 0;                              // more commands follow in this transfer
        return 0;
    case 0x06: ev = DMD_Z_EV_RENDER; break;
    case 0x0A:
        memset(z->img, 0, (size_t)z->w * (size_t)z->h * 4);
        ev = DMD_Z_EV_RENDER;
        break;
    case 0x1F: ev = DMD_Z_EV_BYE; break;
    default: break;                                        // keep-alive, brightness, settings: acked, ignored
    }
    z->done = true;
    return ev;
}

static int cmd_byte(dmd_zedmd_t *z, uint8_t b, uint8_t *out, size_t cap, size_t *len) {
    if (z->ctrl < 5) {
        z->ctrl = b == kCtrl[z->ctrl] ? z->ctrl + 1 : (b == 'Z' ? 1 : 0);
        return 0;
    }
    switch (z->hdr) {
    case 0: z->cmd = b; z->hdr = 1; return 0;
    case 1: z->size = (uint16_t)(b << 8); z->hdr = 2; return 0;
    case 2: z->size |= b; z->got = 0; z->hdr = 3; return 0;
    case 3:
        z->comp = b != 0;
        z->hdr = 4;
        if (z->cmd == 0x05 && z->size > DMD_Z_PAY_MAX) { z->err = true; return DMD_Z_EV_ERROR; }
        if (z->cmd == 0x05 && z->size == 0) { z->ctrl = z->hdr = 0; return 0; }
        return z->cmd == 0x05 ? 0 : command(z, out, cap, len);
    default:
        z->pay[z->got++] = b;
        return command(z, out, cap, len);
    }
}

int dmd_zedmd_feed(dmd_zedmd_t *z, const uint8_t *in, size_t n, uint8_t *out, size_t out_cap, size_t *out_len) {
    int ev = 0;
    for (size_t i = 0; i < n; i++) {
        const uint8_t b = in[i];
        if (z->st == 0) {
            z->match = b == kFrame[z->match] ? z->match + 1 : (b == 'F' ? 1 : 0);
            if (z->match == 5) {
                z->st = 1;
                z->match = 0;
                z->chunk_left = z->chunk - 5u;
                z->done = z->err = false;
                z->ctrl = z->hdr = 0;
            }
            continue;
        }
        z->chunk_left--;
        if (!z->done && !z->err) {
            ev |= cmd_byte(z, b, out, out_cap, out_len);
            if (z->st == 0) continue;                      // the handshake answered and left the transfer
        }
        if (z->chunk_left == 0) {
            if (z->err) {
                const uint8_t f[6] = {'Z', 'e', 'D', 'M', 'D', 'F'};
                put(out, out_cap, out_len, f, 6);
                dmd_zedmd_reset(z);
            } else {
                put(out, out_cap, out_len, kCtrl, 6);
                if (z->done) dmd_zedmd_reset(z);
                else z->chunk_left = z->chunk;
            }
        }
    }
    return ev;
}

// ================================================================== menu link

void dmd_link_init(dmd_link_t *l, uint8_t *buf, uint32_t cap) {
    memset(l, 0, sizeof *l);
    l->buf = buf;
    l->cap = cap;
}

static uint16_t sum_bytes(uint16_t s, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) s = (uint16_t)(s + p[i]);
    return s;
}

static void link_restart(dmd_link_t *l, uint8_t b) {   // resync: b may start the next magic
    l->st = 0;
    l->n = b == 'N' ? 1 : 0;
}

void dmd_link_feed(dmd_link_t *l, const uint8_t *in, size_t n, dmd_link_cb cb, void *ctx) {
    static const uint8_t magic[4] = {'N', 'V', 'D', 'M'};
    for (size_t i = 0; i < n; i++) {
        const uint8_t b = in[i];
        switch (l->st) {
        case 0:
            if (b == magic[l->n]) {
                if (++l->n == 4) { l->st = 1; l->n = 0; }
            } else {
                l->n = b == 'N' ? 1 : 0;
            }
            break;
        case 1:
            l->hdr[l->n++] = b;
            if (l->n == 5) {
                l->len = (uint32_t)l->hdr[1] | (uint32_t)l->hdr[2] << 8 | (uint32_t)l->hdr[3] << 16 | (uint32_t)l->hdr[4] << 24;
                if (l->len > l->cap) { l->bad++; link_restart(l, b); break; }
                l->sum = sum_bytes(0, l->hdr, 5);
                l->got = 0;
                l->n = 0;
                l->st = l->len ? 2 : 3;
            }
            break;
        case 2: {   // payload: take as much as this buffer has
            size_t take = n - i;
            if (take > l->len - l->got) take = l->len - l->got;
            memcpy(l->buf + l->got, in + i, take);
            l->sum = sum_bytes(l->sum, in + i, take);
            l->got += (uint32_t)take;
            i += take - 1;
            if (l->got == l->len) l->st = 3;
            break;
        }
        default:
            l->sumb[l->n++] = b;
            if (l->n == 2) {
                const uint16_t want = (uint16_t)(l->sumb[0] | l->sumb[1] << 8);
                if (want == l->sum) { if (cb) cb(ctx, l->hdr[0], l->buf, l->len); }
                else l->bad++;
                l->st = 0;
                l->n = 0;
            }
            break;
        }
    }
}

void dmd_link_header(uint8_t type, uint32_t len, uint8_t out[9]) {
    out[0] = 'N'; out[1] = 'V'; out[2] = 'D'; out[3] = 'M';
    out[4] = type;
    out[5] = (uint8_t)len; out[6] = (uint8_t)(len >> 8); out[7] = (uint8_t)(len >> 16); out[8] = (uint8_t)(len >> 24);
}

void dmd_link_trailer(uint8_t type, uint32_t len, const uint8_t *payload, uint8_t out[2]) {
    uint8_t h[9];
    dmd_link_header(type, len, h);
    const uint16_t s = sum_bytes(sum_bytes(0, h + 4, 5), payload, len);
    out[0] = (uint8_t)s;
    out[1] = (uint8_t)(s >> 8);
}

// ================================================================== renderer

bool dmd_geom(int w, int h, int rot, int fit, int scr_w, int scr_h, dmd_geom_t *g) {
    memset(g, 0, sizeof *g);
    if (w < 1 || h < 1 || w > DMD_MAX_W || h > DMD_MAX_H || scr_w < 1 || scr_h < 1) return false;
    const bool side = rot & 1;
    g->cols = side ? h : w;
    g->rows = side ? w : h;
    if (fit == DMD_FIT_FILL) {
        // every panel pixel, dots no larger than the masks allow (a tiny DMD then stays centred)
        g->gw = scr_w < g->cols * (DMD_PITCH_MAX - 1) ? scr_w : g->cols * (DMD_PITCH_MAX - 1);
        g->gh = scr_h < g->rows * (DMD_PITCH_MAX - 1) ? scr_h : g->rows * (DMD_PITCH_MAX - 1);
        g->pw = g->gw / g->cols;
        g->ph = g->gh / g->rows;
    } else {
        int pitch = scr_w / g->cols;
        if (scr_h / g->rows < pitch) pitch = scr_h / g->rows;
        if (pitch > DMD_PITCH_MAX - 1) pitch = DMD_PITCH_MAX - 1;
        g->pw = g->ph = pitch;
        g->gw = g->cols * pitch;
        g->gh = g->rows * pitch;
    }
    if (g->pw < 1 || g->ph < 1) return false;
    g->ox = (scr_w - g->gw) / 2;
    g->oy = (scr_h - g->gh) / 2;
    for (int c = 0; c <= g->cols; c++) g->x[c] = (int16_t)((uint32_t)c * (uint32_t)g->gw / (uint32_t)g->cols);
    for (int r = 0; r <= g->rows; r++) g->y[r] = (int16_t)((uint32_t)r * (uint32_t)g->gh / (uint32_t)g->rows);
    return true;
}

static float approx_sqrt(float q, float guess) {   // two Newton steps: plenty for a coverage mask
    if (q <= 0) return 0;
    float d = guess > 0 ? (q + guess * guess) / (2 * guess) : q;
    d = 0.5f * (d + q / d);
    return 0.5f * (d + q / d);
}

void dmd_mask_build(uint8_t *mask, int w, int h, int dot, int shape, bool glow) {
    if (w < 1 || h < 1) return;
    if (w > DMD_PITCH_MAX) w = DMD_PITCH_MAX;
    if (h > DMD_PITCH_MAX) h = DMD_PITCH_MAX;
    if (dot < 1 || dot > (w < h ? w : h)) dot = w < h ? w : h;
    if (dot <= 2) {                        // too small for a shape: a lit cell
        memset(mask, DMD_MASK_LEVELS - 1, (size_t)w * (size_t)h);
        return;
    }
    // In units of half the dot's side, the same on both axes: the dot is a true circle 0.90 of `dot` wide
    // (a square 0.84: it already covers more of the cell), centred in the cell however tall or wide the cell
    // is; the glow fades over the next half dot; the centre is hotter (the core levels, out to 3/4 of the
    // radius), as a lit LED. In a square cell (dot = the cell) this is exactly the dot of the square fit.
    const float cx = w * 0.5f, cy = h * 0.5f, hs = dot * 0.5f, rn = shape == DMD_SHAPE_SQUARE ? 0.84f : 0.90f;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int in = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    const float nx = (x + (sx + 0.5f) / 4 - cx) / hs, ny = (y + (sy + 0.5f) / 4 - cy) / hs;
                    const float ax = nx < 0 ? -nx : nx, ay = ny < 0 ? -ny : ny;
                    if (shape == DMD_SHAPE_SQUARE ? (ax <= rn && ay <= rn) : (nx * nx + ny * ny <= rn * rn)) in++;
                }
            int lvl = (in * (DMD_MASK_LEVELS - 1) + 8) / 16;
            const float nx = (x + 0.5f - cx) / hs, ny = (y + 0.5f - cy) / hs;
            if (glow && in < 16) {
                const float ax = nx < 0 ? -nx : nx, ay = ny < 0 ? -ny : ny;
                const float d = shape == DMD_SHAPE_SQUARE ? (ax > ay ? ax : ay) : approx_sqrt(nx * nx + ny * ny, 1.0f);
                float gv = 0.32f * (1.0f - (d - rn));
                if (gv > 0.32f) gv = 0.32f;
                const int gl = gv > 0 ? (int)(gv * (DMD_MASK_LEVELS - 1) + 0.5f) : 0;
                if (gl > lvl) lvl = gl;
            }
            if (in == 16) {   // the hot core: brightest at the centre, none from 3/4 of the radius out
                const float wgt = 1.0f - approx_sqrt(nx * nx + ny * ny, 0.5f) / (rn * 0.75f);
                const int cl = wgt > 0 ? (int)(wgt * DMD_CORE_LEVELS + 0.5f) : 0;
                if (cl > 0) lvl = DMD_MASK_LEVELS - 1 + (cl > DMD_CORE_LEVELS ? DMD_CORE_LEVELS : cl);
            }
            mask[y * w + x] = (uint8_t)lvl;
        }
    }
}

void dmd_masks_build(dmd_masks_t *mk, const dmd_geom_t *g, int shape, bool glow) {
    mk->pw = g->pw;
    mk->ph = g->ph;
    for (int v = 0; v < 4; v++) {
        const int w = g->pw + (v & 1), h = g->ph + (v >> 1);
        // one dot size for all four cell shapes: every dot on screen is the same round dot
        if (w <= DMD_PITCH_MAX && h <= DMD_PITCH_MAX) dmd_mask_build(mk->m[v], w, h, g->pw < g->ph ? g->pw : g->ph, shape, glow);
    }
}

static inline uint16_t to565(uint32_t c, uint32_t k) {   // k: 0..256 intensity
    const uint32_t r = (((c >> 16) & 0xFF) * k) >> 8, g = (((c >> 8) & 0xFF) * k) >> 8, b = ((c & 0xFF) * k) >> 8;
    return (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
}

// The colour of mask value l for a dot of colour c at intensity k: coverage levels as they are; the core
// levels also step towards white, scaled by how lit the dot is (its brightest channel), so an unlit dot (a
// ninth of the colour) gets almost none and a dark grey never turns pale.
static inline uint16_t dot565(uint32_t c, uint32_t k, int l) {
    if (l < DMD_MASK_LEVELS) return to565(c, k);
    uint32_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    const uint32_t mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    const uint32_t f = (uint32_t)(l - DMD_MASK_LEVELS + 1) * DMD_CORE_WHITE * mx / (DMD_CORE_LEVELS * 255u);
    r += ((255 - r) * f) >> 8;
    g += ((255 - g) * f) >> 8;
    b += ((255 - b) * f) >> 8;
    return to565(r << 16 | g << 8 | b, k);
}

void dmd_render(uint16_t *dst, int stride, const dmd_geom_t *g, const dmd_masks_t *mk, const uint32_t *px,
                int w, int h, int rot, int bright, bool clear_margins, int scr_w) {
    dmd_render_rows(dst, stride, g, mk, px, w, h, rot, bright, clear_margins, scr_w, 0, g->rows, NULL);
}

void dmd_sprites_reset(dmd_sprites_t *sc) {
    sc->pw = sc->ph = 0;
    sc->bright = -1;
    sc->row_id = 0;
    memset(sc->key, 0, sizeof sc->key);
    memset(sc->stamp, 0, sizeof sc->stamp);
}

// The sprite of colour c in dot shape v for the current dot row: a hit, or a slot not used by this row
// yet (rebuilt). NULL when 4 probes are all taken by this row (the caller draws that dot pixel by pixel).
static const uint16_t *sprite(dmd_sprites_t *sc, uint32_t c, int v, const dmd_masks_t *mk, const uint32_t *k) {
    const uint32_t want = c | (uint32_t)v << 24 | 1u << 26;
    const uint32_t h = (want * 2654435761u) >> sc->shift, m = (uint32_t)sc->nslots - 1;
    const size_t area = ((size_t)(sc->pw + 1) * (size_t)(sc->ph + 1) + 1) & ~(size_t)1;   // even: aligned rows
    int free = -1;
    for (uint32_t i = 0; i < 4; i++) {
        const uint32_t sl = (h + i) & m;
        if (sc->key[sl] == want) { sc->stamp[sl] = sc->row_id; return sc->px + sl * area; }
        if (free < 0 && sc->stamp[sl] != sc->row_id) free = (int)sl;
    }
    if (free < 0) return NULL;
    uint16_t lut[DMD_MASK_VALUES];
    for (int l = 0; l < DMD_MASK_VALUES; l++) lut[l] = dot565(c, k[l], l);
    const int n = (sc->pw + (v & 1)) * (sc->ph + (v >> 1));
    const uint8_t *mask = mk->m[v];
    uint16_t *sp = sc->px + (size_t)free * area;
    for (int i = 0; i < n; i++) sp[i] = lut[mask[i]];
    sc->key[free] = want;
    sc->stamp[free] = sc->row_id;
    return sp;
}

static inline uint32_t dot_colour(const uint32_t *px, int w, int h, int rot, int cx, int cy) {
    int x, y;
    switch (rot & 3) {
    case 1:  x = cy;         y = h - 1 - cx; break;
    case 2:  x = w - 1 - cx; y = h - 1 - cy; break;
    case 3:  x = w - 1 - cy; y = cx;         break;
    default: x = cx;         y = cy;         break;
    }
    return px[(size_t)y * (size_t)w + (size_t)x] & 0xFFFFFF;
}

void dmd_render_rows(uint16_t *dst, int stride, const dmd_geom_t *g, const dmd_masks_t *mk, const uint32_t *px,
                     int w, int h, int rot, int bright, bool clear_margins, int scr_w, int cy0, int cy1,
                     dmd_sprites_t *sc) {
    if (cy0 < 0) cy0 = 0;
    if (cy1 > g->rows) cy1 = g->rows;
    if (cy0 >= cy1 || g->pw + 1 > DMD_PITCH_MAX + 1) return;
    if (bright < 1) bright = 1;
    if (bright > 100) bright = 100;
    uint32_t k[DMD_MASK_VALUES];   // intensity per mask value: the core levels are at full coverage
    for (int l = 0; l < DMD_MASK_VALUES; l++)
        k[l] = (uint32_t)((l < DMD_MASK_LEVELS ? l : DMD_MASK_LEVELS - 1) * bright * 256 / ((DMD_MASK_LEVELS - 1) * 100));
    const int pw = g->pw, ph = g->ph;
    const bool sprites = sc && (pw + 1) * (ph + 1) <= DMD_SPRITE_AREA && g->cols <= DMD_MAX_W;
    if (sprites && (sc->pw != pw || sc->ph != ph || sc->bright != bright)) {   // a cache made for other dots
        dmd_sprites_reset(sc);
        sc->pw = pw;
        sc->ph = ph;
        sc->bright = bright;
        const int area = ((pw + 1) * (ph + 1) + 1) & ~1;   // as sprite() lays the slots out
        int bits = 0;
        while (bits < 11 && (2 << bits) * area <= DMD_SPRITE_PX) bits++;   // 2^bits slots
        sc->nslots = 1 << bits;
        sc->shift = 32 - bits;
    }
    const int y_base = dmd_cell_y(g, cy0);
    const bool aligned = !(g->ox & 1) && !(stride & 1) && !((uintptr_t)dst & 3);
    uint16_t lut[DMD_MASK_VALUES];
    uint32_t last = 0;
    memset(lut, 0, sizeof lut);   // matches last = 0 (black)
    for (int cy = cy0; cy < cy1; cy++) {
        const int y0 = dmd_cell_y(g, cy), ch = dmd_cell_y(g, cy + 1) - y0;
        const int vy = ch > ph ? 2 : 0;
        uint16_t *rowp = dst + (size_t)(y0 - y_base) * stride + g->ox;
        if (sprites) {
            // 1. the finished dot of every column (built on a cache miss)
            for (int cx = 0; cx < g->cols; cx++) {
                const int cw = dmd_cell_x(g, cx + 1) - dmd_cell_x(g, cx);
                const uint32_t c = dot_colour(px, w, h, rot, cx, cy);
                sc->col[cx] = c;
                sc->row[cx] = sprite(sc, c, (cw > pw ? 1 : 0) | vy, mk, k);
            }
            if (++sc->row_id == 0) {   // wrapped: no stale stamp may look current
                memset(sc->stamp, 0, sizeof sc->stamp);
                sc->row_id = 1;
            }
            // 2. the panel rows of this dot row, written in memory order
            for (int r = 0; r < ch; r++) {
                uint16_t *d = rowp + (size_t)r * stride;
                for (int cx = 0, x = 0; cx < g->cols; cx++) {
                    const int nx = dmd_cell_x(g, cx + 1), cw = nx - x;
                    const uint16_t *s = sc->row[cx];
                    if (s) {
                        s += r * cw;
                        if (aligned && !(x & 1) && !(cw & 1) && !(((uintptr_t)s) & 3)) {
                            const uint32_t *s32 = (const uint32_t *)s;
                            uint32_t *d32 = (uint32_t *)(d + x);
                            if (cw == 8) { d32[0] = s32[0]; d32[1] = s32[1]; d32[2] = s32[2]; d32[3] = s32[3]; }
                            else for (int i = 0; i < cw / 2; i++) d32[i] = s32[i];
                        } else {
                            for (int i = 0; i < cw; i++) d[x + i] = s[i];
                        }
                    } else {   // no sprite (a crowded row): its own table
                        if (sc->col[cx] != last) {
                            for (int l = 0; l < DMD_MASK_VALUES; l++) lut[l] = dot565(sc->col[cx], k[l], l);
                            last = sc->col[cx];
                        }
                        const uint8_t *mr = mk->m[(cw > pw ? 1 : 0) | vy] + r * cw;
                        for (int i = 0; i < cw; i++) d[x + i] = lut[mr[i]];
                    }
                    x = nx;
                }
            }
            continue;
        }
        for (int cx = 0, x = 0; cx < g->cols; cx++) {
            const int nx = dmd_cell_x(g, cx + 1), cw = nx - x;
            const uint32_t c = dot_colour(px, w, h, rot, cx, cy);
            if (c != last) {
                for (int l = 0; l < DMD_MASK_VALUES; l++) lut[l] = dot565(c, k[l], l);
                last = c;
            }
            const uint8_t *mr = mk->m[(cw > pw ? 1 : 0) | vy];
            uint16_t *cell = rowp + x;
            for (int r = 0; r < ch; r++, cell += stride, mr += cw)
                for (int i = 0; i < cw; i++) cell[i] = lut[mr[i]];
            x = nx;
        }
    }
    if (clear_margins) {
        const int right = g->ox + g->gw;
        const int rows = dmd_cell_y(g, cy1) - y_base;
        for (int r = 0; r < rows; r++) {
            uint16_t *row = dst + (size_t)r * stride;
            if (g->ox > 0) memset(row, 0, (size_t)g->ox * 2);
            if (right < scr_w) memset(row + right, 0, (size_t)(scr_w - right) * 2);
        }
    }
}

// ================================================================== text

// 5x7 glyphs: 7 rows of 5 bits (bit 4 = left column).
static const struct { char c; uint8_t r[7]; } kFont[] = {
    {'A', {14, 17, 17, 31, 17, 17, 17}}, {'B', {30, 17, 17, 30, 17, 17, 30}}, {'C', {14, 17, 16, 16, 16, 17, 14}},
    {'D', {28, 18, 17, 17, 17, 18, 28}}, {'E', {31, 16, 16, 30, 16, 16, 31}}, {'F', {31, 16, 16, 30, 16, 16, 16}},
    {'G', {14, 17, 16, 23, 17, 17, 15}}, {'H', {17, 17, 17, 31, 17, 17, 17}}, {'I', {14, 4, 4, 4, 4, 4, 14}},
    {'J', {7, 2, 2, 2, 2, 18, 12}},      {'K', {17, 18, 20, 24, 20, 18, 17}}, {'L', {16, 16, 16, 16, 16, 16, 31}},
    {'M', {17, 27, 21, 21, 17, 17, 17}}, {'N', {17, 17, 25, 21, 19, 17, 17}}, {'O', {14, 17, 17, 17, 17, 17, 14}},
    {'P', {30, 17, 17, 30, 16, 16, 16}}, {'Q', {14, 17, 17, 17, 21, 18, 13}}, {'R', {30, 17, 17, 30, 20, 18, 17}},
    {'S', {15, 16, 16, 14, 1, 1, 30}},   {'T', {31, 4, 4, 4, 4, 4, 4}},       {'U', {17, 17, 17, 17, 17, 17, 14}},
    {'V', {17, 17, 17, 17, 17, 10, 4}},  {'W', {17, 17, 17, 21, 21, 21, 10}}, {'X', {17, 17, 10, 4, 10, 17, 17}},
    {'Y', {17, 17, 10, 4, 4, 4, 4}},     {'Z', {31, 1, 2, 4, 8, 16, 31}},
    {'0', {14, 17, 19, 21, 25, 17, 14}}, {'1', {4, 12, 4, 4, 4, 4, 14}},      {'2', {14, 17, 1, 2, 4, 8, 31}},
    {'3', {31, 2, 4, 2, 1, 17, 14}},     {'4', {2, 6, 10, 18, 31, 2, 2}},     {'5', {31, 16, 30, 1, 1, 17, 14}},
    {'6', {6, 8, 16, 30, 17, 17, 14}},   {'7', {31, 1, 2, 4, 8, 8, 8}},       {'8', {14, 17, 17, 14, 17, 17, 14}},
    {'9', {14, 17, 17, 15, 1, 2, 12}},
    {'.', {0, 0, 0, 0, 0, 12, 12}},      {',', {0, 0, 0, 0, 12, 4, 8}},       {':', {0, 12, 12, 0, 12, 12, 0}},
    {'!', {4, 4, 4, 4, 4, 0, 4}},        {'?', {14, 17, 1, 2, 4, 0, 4}},      {'-', {0, 0, 0, 31, 0, 0, 0}},
    {'\'', {4, 4, 8, 0, 0, 0, 0}},       {'/', {1, 1, 2, 4, 8, 16, 16}},      {'+', {0, 4, 4, 31, 4, 4, 0}},
    {'%', {24, 25, 2, 4, 8, 19, 3}},
};

static const uint8_t *glyph(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    for (size_t i = 0; i < sizeof kFont / sizeof kFont[0]; i++)
        if (kFont[i].c == c) return kFont[i].r;
    return NULL;   // space and anything unknown
}

int dmd_text(uint32_t *px, int w, int h, const char *text, uint32_t color) {
    if (w < 1 || h < 1) return 0;
    memset(px, 0, (size_t)w * (size_t)h * 4);
    int lines = 1, len = 0, longest = 0;
    for (const char *p = text; *p; p++) {
        if (*p == '\n') { lines++; len = 0; continue; }
        if (++len > longest) longest = len;
    }
    if (!longest) return 0;
    const int tw = longest * 6 - 1, th = lines * 8 - 1;   // one column / row between glyphs and lines
    int s = w / tw;
    if (h / th < s) s = h / th;
    if (s < 1) return 0;
    const int y0 = (h - th * s) / 2;
    const char *p = text;
    for (int li = 0; li < lines; li++) {
        int n = 0;
        while (p[n] && p[n] != '\n') n++;
        const int x0 = (w - (n * 6 - 1) * s) / 2;
        for (int ci = 0; ci < n; ci++) {
            const uint8_t *gl = glyph(p[ci]);
            if (!gl) continue;
            for (int ry = 0; ry < 7; ry++)
                for (int rx = 0; rx < 5; rx++)
                    if (gl[ry] >> (4 - rx) & 1)
                        for (int sy = 0; sy < s; sy++) {
                            uint32_t *row = px + (size_t)(y0 + (li * 8 + ry) * s + sy) * w;
                            for (int sx = 0; sx < s; sx++) row[x0 + (ci * 6 + rx) * s + sx] = color & 0xFFFFFF;
                        }
        }
        p += n;
        if (*p == '\n') p++;
    }
    return s;
}

// ================================================================== FullDMD window

// Dark: every channel under about 22% (RGB565 values). The windows are black or nearly (a faint glass tint
// or a dark grey in some art); the art around them is not.
static bool dark565(uint16_t c) { return (c >> 11) < 7 && ((c >> 5) & 63) < 14 && (c & 31) < 7; }

// The search: `dark` says which pixels count; the result may touch the picture's top / bottom edge when
// edge_tb (a grill's housing can reach its rows' ends), never its sides.
static bool find_dark_rect(const uint16_t *px, int stride, int w, int h, bool (*dark)(uint16_t, int), int thr,
                           bool edge_tb, bool edge_lr, dmd_rect_t *out) {
    enum { B = 4 };
    if (!px || !out || w < 16 || h < 16 || w > DMD_WIN_MAX_W) return false;
    const int bw = w / B, bh = h / B;
    int16_t hist[DMD_WIN_MAX_W / B + 1], stk[DMD_WIN_MAX_W / B + 1];
    int best = 0, bx = 0, by = 0, bwid = 0, bhei = 0;
    for (int c = 0; c < bw; c++) hist[c] = 0;
    for (int r = 0; r < bh; r++) {
        for (int c = 0; c < bw; c++) {   // a block counts when all 16 of its pixels are dark
            bool d = true;
            for (int y = r * B; d && y < r * B + B; y++)
                for (int x = c * B; x < c * B + B; x++)
                    if (!dark(px[(size_t)y * (size_t)stride + (size_t)x], thr)) { d = false; break; }
            hist[c] = d ? (int16_t)(hist[c] + 1) : 0;
        }
        // largest rectangle under the histogram, each candidate cut to at least 2:1 (a DMD is wide)
        int sp = 0;
        for (int c = 0; c <= bw; c++) {
            const int hc = c < bw ? hist[c] : 0;
            while (sp && hist[stk[sp - 1]] >= hc) {
                const int top = stk[--sp];
                const int hh = hist[top];
                const int left = sp ? stk[sp - 1] + 1 : 0, wid = c - left;
                const int useh = wid >= 2 * hh ? hh : wid / 2;
                if (useh > 0 && wid * useh > best) {
                    best = wid * useh;
                    bx = left;
                    bwid = wid;
                    bhei = useh;
                    by = r - hh + 1 + (hh - useh) / 2;   // a cut one stays centred in the dark area
                }
            }
            if (c < bw) stk[sp++] = (int16_t)c;
        }
    }
    if (!best) return false;
    int x0 = bx * B, y0 = by * B, x1 = (bx + bwid) * B, y1 = (by + bhei) * B;   // [x0, x1) x [y0, y1)
    // grow each edge while the next pixel line along it is still dark (blocks cut up to B - 1 px)
    for (int k = 0; k < B && y0 > 0; k++) {
        bool d = true;
        for (int x = x0; d && x < x1; x++) d = dark(px[(size_t)(y0 - 1) * (size_t)stride + (size_t)x], thr);
        if (!d) break;
        y0--;
    }
    for (int k = 0; k < B && y1 < h; k++) {
        bool d = true;
        for (int x = x0; d && x < x1; x++) d = dark(px[(size_t)y1 * (size_t)stride + (size_t)x], thr);
        if (!d) break;
        y1++;
    }
    for (int k = 0; k < B && x0 > 0; k++) {
        bool d = true;
        for (int y = y0; d && y < y1; y++) d = dark(px[(size_t)y * (size_t)stride + (size_t)(x0 - 1)], thr);
        if (!d) break;
        x0--;
    }
    for (int k = 0; k < B && x1 < w; k++) {
        bool d = true;
        for (int y = y0; d && y < y1; y++) d = dark(px[(size_t)y * (size_t)stride + (size_t)x1], thr);
        if (!d) break;
        x1++;
    }
    if (x1 - x0 < w / 3 || y1 - y0 < h / 10) return false;
    // a real window sits inside the art: one touching the picture's edge is a dark background, and none
    // is wider than 6:1 (a DMD is 4:1; the frame leaves a little more height or width)
    if ((!edge_lr && (x0 == 0 || x1 == w)) || (!edge_tb && (y0 == 0 || y1 == h)) || x1 - x0 > 6 * (y1 - y0)) return false;
    out->x = x0;
    out->y = y0;
    out->w = x1 - x0;
    out->h = y1 - y0;
    return true;
}

static bool dark_fixed(uint16_t c, int thr) {
    (void)thr;
    return dark565(c);
}

bool dmd_find_window(const uint16_t *px, int w, int h, dmd_rect_t *out) {
    return find_dark_rect(px, w, w, h, dark_fixed, 0, false, false, out);
}

// Luminance of an RGB565 pixel, 0..255 (0.30 R + 0.59 G + 0.11 B).
static int lum565(uint16_t c) {
    const int r = (c >> 11) * 255 / 31, g = (c >> 5 & 63) * 255 / 63, b = (c & 31) * 255 / 31;
    return (r * 77 + g * 151 + b * 28) >> 8;
}
static bool dark_lum(uint16_t c, int thr) { return lum565(c) < thr; }

// Grows r while the next line along each side is still dark (lum < thr) for 90% of its length, up to `max`
// pixels a side: a housing's rounded corners and soft edges stop the block search a few pixels short.
static void grow_dark(const uint16_t *px, int w, int h, int thr, int max, dmd_rect_t *r) {
    for (int side = 0; side < 4; side++) {
        for (int k = 0; k < max; k++) {
            int n = 0, dark = 0;
            if (side < 2) {   // top, bottom: a row
                const int y = side == 0 ? r->y - 1 : r->y + r->h;
                if (y < 0 || y >= h) break;
                for (int x = r->x; x < r->x + r->w; x++, n++) dark += lum565(px[(size_t)y * (size_t)w + (size_t)x]) < thr;
            } else {          // left, right: a column
                const int x = side == 2 ? r->x - 1 : r->x + r->w;
                if (x < 0 || x >= w) break;
                for (int y = r->y; y < r->y + r->h; y++, n++) dark += lum565(px[(size_t)y * (size_t)w + (size_t)x]) < thr;
            }
            if (!n || dark * 10 < n * 9) break;
            if (side == 0) { r->y--; r->h++; }
            else if (side == 1) r->h++;
            else if (side == 2) { r->x--; r->w++; }
            else r->w++;
        }
    }
}

bool dmd_find_housing(const uint16_t *px, int w, int h, dmd_rect_t *out) {
    if (!px || !out || w < 16 || h < 16 || w > DMD_WIN_MAX_W) return false;
    // a backglass is stored unlit (dim) and housings are often dark grey: the lowest threshold that gives
    // a housing a DMD fits, centred across the grill and at least a third of its width, wins
    // then, when the housing runs into dark speakers at the sides, the same in the middle 70% of the grill
    // alone (where housings are), the result allowed to reach those columns
    for (int pass = 0; pass < 2; pass++) {
        const int x0 = pass ? w * 15 / 100 : 0, ww = pass ? w * 70 / 100 : w;
        for (int thr = 40; thr <= 100; thr += 15) {   // up to ~39%: brighter is the grill, not a housing
            dmd_rect_t r;
            if (!find_dark_rect(px + x0, w, ww, h, dark_lum, thr, true, pass == 1, &r)) continue;
            r.x += x0;
            const int cx = r.x + r.w / 2;
            if (r.w * 3 < w || cx < w * 35 / 100 || cx > w * 65 / 100 || r.w < 2 * r.h) continue;
            // the housing's whole opening: a little more tolerant, a few pixels a side (never into the speakers)
            grow_dark(px, w, h, thr + 25 < 125 ? thr + 25 : 125, 3 + h / 12, &r);
            *out = r;
            return true;
        }
    }
    return false;
}

// ================================================================== rebuilt DMD

bool dmd_scale2x_rows(const uint32_t *px, int w, int h, uint32_t *dst) {
    if (!px || !dst || w < 1 || h < 1 || w > DMD_MAX_W || 2 * h > DMD_MAX_H || (size_t)w * (size_t)(2 * h) > DMD_MAX_PX)
        return false;
    for (int r = 0; r < h; r++) {
        const uint32_t *row = px + (size_t)r * (size_t)w;
        const uint32_t *up = r ? row - w : row, *dn = r + 1 < h ? row + w : row;   // edges repeat
        uint32_t *t = dst + (size_t)(2 * r) * (size_t)w, *b = t + w;
        for (int c = 0; c < w; c++) {
            const uint32_t P = row[c], A = up[c], D = dn[c];
            const uint32_t C = c ? row[c - 1] : P, B = c + 1 < w ? row[c + 1] : P;
            const uint32_t UL = c ? up[c - 1] : A, UR = c + 1 < w ? up[c + 1] : A;
            const uint32_t DL = c ? dn[c - 1] : D, DR = c + 1 < w ? dn[c + 1] : D;
            // Scale2x (E0 E1 over E2 E3), but a corner is cut only on a real diagonal: when the dot in that
            // corner is another colour. A filled corner (the inside of an L, E, F, T, H: the DMD's fonts are
            // all right angles) stays square instead of getting a dot across it.
            const uint32_t e0 = (C == A && C != D && A != B && UL != C) ? C : P;
            const uint32_t e1 = (A == B && A != C && B != D && UR != A) ? A : P;
            const uint32_t e2 = (D == C && D != B && C != A && DL != C) ? C : P;
            const uint32_t e3 = (B == D && B != A && D != C && DR != B) ? B : P;
            t[c] = e0 ? e0 : e1;
            b[c] = e2 ? e2 : e3;
        }
    }
    return true;
}

bool dmd_rebuild(const uint32_t *px, int w, int h, int r0, int n, uint32_t *dst, uint32_t *tmp) {
    return dmd_rebuild_to(px, w, h, r0, n, dst, tmp, 2 * h);
}

bool dmd_rebuild_to(const uint32_t *px, int w, int h, int r0, int n, uint32_t *dst, uint32_t *tmp, int out_rows) {
    if (!px || !dst || !tmp || w < 1 || h < 1 || w > DMD_MAX_W || out_rows < h || out_rows > 2 * h ||
        out_rows > DMD_MAX_H || (size_t)w * (size_t)out_rows > DMD_MAX_PX || r0 < 0 || n < 1 || r0 + n > h)
        return false;
    const size_t W = (size_t)w;
    memcpy(tmp, px + (size_t)r0 * W, (size_t)n * W * sizeof *px);   // the rows to rebuild
    uint32_t *cur = tmp, *other = dst;
    while (2 * n <= out_rows) {   // double while it fits
        dmd_scale2x_rows(cur, w, n, other);
        uint32_t *t = cur;
        cur = other;
        other = t;
        n *= 2;
    }
    const int pad = (out_rows - n) / 2;
    if (cur == dst) {
        if (pad) memmove(dst + (size_t)pad * W, dst, (size_t)n * W * sizeof *dst);
    } else {
        memcpy(dst + (size_t)pad * W, cur, (size_t)n * W * sizeof *dst);
    }
    memset(dst, 0, (size_t)pad * W * sizeof *dst);
    memset(dst + (size_t)(pad + n) * W, 0, (size_t)(out_rows - pad - n) * W * sizeof *dst);
    return true;
}

void dmd_rebuild_rows(int h, int lit_lo, int lit_hi, int *r0, int *n) {
    if (h >= 8 && lit_lo >= h / 4 && lit_hi >= lit_lo && lit_hi < h / 4 + h / 2) {
        *r0 = h / 4;
        *n = h / 2;
    } else {
        *r0 = 0;
        *n = h;
    }
}

bool dmd_lit_rows(const uint32_t *px, int w, int h, int *lo, int *hi) {
    int a = -1, b = -1;
    for (int r = 0; px && r < h; r++) {
        const uint32_t *row = px + (size_t)r * (size_t)w;
        for (int c = 0; c < w; c++)
            if (row[c]) {
                if (a < 0) a = r;
                b = r;
                break;
            }
    }
    *lo = a;
    *hi = b;
    return a >= 0;
}

// ================================================================== ambient bands

// in -> out (n columns): each channel averaged over [c - rad, c + rad] (the edges count what they have)
static void blur_cols(const uint32_t *in, uint32_t *out, int n, int rad) {
    uint32_t r = 0, g = 0, b = 0;
    int lo = 0, hi = -1;
    for (int c = 0; c < n; c++) {
        const int want_hi = c + rad < n ? c + rad : n - 1, want_lo = c - rad > 0 ? c - rad : 0;
        while (hi < want_hi) { hi++; r += in[hi] >> 16 & 255; g += in[hi] >> 8 & 255; b += in[hi] & 255; }
        while (lo < want_lo) { r -= in[lo] >> 16 & 255; g -= in[lo] >> 8 & 255; b -= in[lo] & 255; lo++; }
        const uint32_t k = (uint32_t)(hi - lo + 1);
        out[c] = (r / k) << 16 | (g / k) << 8 | (b / k);
    }
}

// One band's colours: panel rows [cy0, cy0 + n), the weight rising towards the DMD's edge (the last of
// them when near_last, else the first).
static void ambient_half(const uint32_t *px, int w, int h, int rot, int cols, int cy0, int n, bool near_last,
                         uint32_t *out, uint32_t *scratch) {
    uint32_t wsum = 0;
    for (int i = 1; i <= n; i++) wsum += (uint32_t)i;
    for (int cx = 0; cx < cols; cx++) {
        uint32_t r = 0, g = 0, b = 0;
        for (int i = 0; i < n; i++) {
            const uint32_t c = dot_colour(px, w, h, rot, cx, cy0 + i);
            if (!c) continue;
            const uint32_t wt = near_last ? (uint32_t)(i + 1) : (uint32_t)(n - i);
            r += (c >> 16 & 255) * wt;
            g += (c >> 8 & 255) * wt;
            b += (c & 255) * wt;
        }
        // the area's average (unlit dots count as black) x 3.5: a scene a quarter lit throws full light
        uint32_t cr = r * 7 / (2 * wsum), cg = g * 7 / (2 * wsum), cb = b * 7 / (2 * wsum);
        if (cr > 255) cr = 255;
        if (cg > 255) cg = 255;
        if (cb > 255) cb = 255;
        scratch[cx] = cr << 16 | cg << 8 | cb;
    }
    blur_cols(scratch, out, cols, cols / 16 > 1 ? cols / 16 : 1);
    for (int cx = 0; cx < cols; cx++) {   // white / grey (little saturation): half the light
        const uint32_t c = out[cx], cr = c >> 16 & 255, cg = c >> 8 & 255, cb = c & 255;
        const uint32_t mx = cr > cg ? (cr > cb ? cr : cb) : (cg > cb ? cg : cb);
        const uint32_t mn = cr < cg ? (cr < cb ? cr : cb) : (cg < cb ? cg : cb);
        if (mx && (mx - mn) * 4 < mx) out[cx] = (cr / 2) << 16 | (cg / 2) << 8 | (cb / 2);
    }
}

void dmd_ambient(const uint32_t *px, int w, int h, int rot, uint32_t *top, uint32_t *bot, uint32_t *scratch) {
    if (!px || !top || !bot || !scratch || w < 1 || h < 1 || w > DMD_MAX_W || h > DMD_MAX_H) return;
    const int cols = rot & 1 ? h : w, rows = rot & 1 ? w : h, half = rows / 2 > 0 ? rows / 2 : 1;
    ambient_half(px, w, h, rot, cols, 0, half, false, top, scratch);            // upper half: row 0 is the edge
    ambient_half(px, w, h, rot, cols, rows - half, half, true, bot, scratch);   // lower half: its last row is
}

void dmd_ambient_band(const uint32_t *amb, int cols, int rows, bool above, int gap, uint32_t floor_rgb, uint32_t *out) {
    if (!amb || !out || cols < 1 || rows < 1) return;
    const uint32_t fr = floor_rgb >> 16 & 255, fg = floor_rgb >> 8 & 255, fb = floor_rgb & 255;
    uint32_t f = 141;   // 55% on the first lit row (x/256)
    for (int d = 0; d < rows; d++) {
        uint32_t *o = out + (size_t)(above ? rows - 1 - d : d) * (size_t)cols;
        if (d < gap) {   // the margin: unlit dots
            for (int c = 0; c < cols; c++) o[c] = floor_rgb;
            continue;
        }
        if (d > gap) f = f * 215 / 256;   // 0.84x per row further away
        for (int c = 0; c < cols; c++) {
            uint32_t r = (amb[c] >> 16 & 255) * f >> 8, g = (amb[c] >> 8 & 255) * f >> 8, b = (amb[c] & 255) * f >> 8;
            if (r < fr) r = fr;
            if (g < fg) g = fg;
            if (b < fb) b = fb;
            o[c] = r << 16 | g << 8 | b;
        }
    }
}

// ---- the second screen and previews

dmd_rect_t dmd_inset(dmd_rect_t r) {
    const int in = r.h * 3 / 100 > 2 ? r.h * 3 / 100 : 2;
    if (r.w > 2 * in && r.h > 2 * in) {
        r.x += in;
        r.y += in;
        r.w -= 2 * in;
        r.h -= 2 * in;
    }
    return r;
}

// A w x h picture in the box: its shape kept and centred, or the whole box when that stretches it by
// `stretch` % or less (0: never).
static dmd_rect_t s2_fit(int w, int h, dmd_rect_t box, int stretch) {
    dmd_rect_t r = box;
    if (w < 1 || h < 1 || box.w < 1 || box.h < 1) return r;
    if ((int64_t)w * box.h > (int64_t)h * box.w) {   // wider than the box: full width
        r.h = (int)((int64_t)box.w * h / w);
        if ((int64_t)(box.h - r.h) * 100 <= (int64_t)box.h * stretch) r.h = box.h;
    } else {
        r.w = (int)((int64_t)box.h * w / h);
        if ((int64_t)(box.w - r.w) * 100 <= (int64_t)box.w * stretch) r.w = box.w;
    }
    r.x = box.x + (box.w - r.w) / 2;
    r.y = box.y + (box.h - r.h) / 2;
    return r;
}

// The DMD in a window or a housing: its own shape, stretched by DMD_S2_DMD_STRETCH % at most to fill more of
// it, centred (a real DMD behind a wider glass: the housing's dark around it, never a squashed picture).
static dmd_rect_t s2_dmd(dmd_rect_t box, int dw, int dh) {
    dmd_rect_t r = dmd_inset(box);
    if (dw < 1 || dh < 1 || r.w < 1 || r.h < 1) return r;
    if ((int64_t)r.w * dh > (int64_t)r.h * dw) {   // wider than the DMD: its height, a little wider at most
        const int w = (int)((int64_t)r.h * dw * (100 + DMD_S2_DMD_STRETCH) / dh / 100);
        if (w < r.w) {
            r.x += (r.w - w) / 2;
            r.w = w;
        }
    } else {
        const int h = (int)((int64_t)r.w * dh * (100 + DMD_S2_DMD_STRETCH) / dw / 100);
        if (h < r.h) {
            r.y += (r.h - h) / 2;
            r.h = h;
        }
    }
    return r;
}

// A rectangle of a pw x ph picture, where the picture is drawn at `at`.
static dmd_rect_t s2_map(dmd_rect_t in, int pw, int ph, dmd_rect_t at) {
    dmd_rect_t r = {0, 0, 0, 0};
    if (pw < 1 || ph < 1) return r;
    r.x = at.x + (int)((int64_t)in.x * at.w / pw);
    r.y = at.y + (int)((int64_t)in.y * at.h / ph);
    r.w = (int)((int64_t)(in.x + in.w) * at.w / pw) - (r.x - at.x);
    r.h = (int)((int64_t)(in.y + in.h) * at.h / ph) - (r.y - at.y);
    return r;
}

bool dmd_s2_avail(int mode, const dmd_s2_in_t *in) {
    if (!in || in->sw < 16 || in->sh < 16) return false;
    const bool bg = in->bw > 0 && in->bh > in->grill;
    const bool art = in->aw > 0 && in->ah > 0 && in->window.w > 0 && in->window.h > 0;
    switch (mode) {
    case DMD_S2_KEEP: return true;
    case DMD_S2_BG: return bg;
    case DMD_S2_BG_DMD: return bg && in->dw > 0 && in->dh > 0;
    case DMD_S2_HOUSING: return bg && in->grill > 0 && in->housing.w > 0 && in->housing.h > 0;
    case DMD_S2_ART: return art;
    case DMD_S2_BG_ART: return bg && art;
    default: return false;
    }
}

bool dmd_s2_layout(int mode, const dmd_s2_in_t *in, dmd_s2_out_t *out) {
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (mode == DMD_S2_KEEP || !dmd_s2_avail(mode, in)) return false;
    const dmd_rect_t scr = {0, 0, in->sw, in->sh};
    const int bh = in->bh - in->grill;   // the backglass without its grill
    switch (mode) {
    case DMD_S2_BG:
        out->bg = s2_fit(in->bw, bh, scr, DMD_S2_STRETCH);
        break;
    case DMD_S2_BG_DMD: {
        // the DMD the whole width at its own shape (a 4:1 one on 1280: 320 tall), never over half the screen
        int band = (int)((int64_t)in->sw * in->dh / in->dw);
        if (band > in->sh / 2) band = in->sh / 2;
        const dmd_rect_t box = {0, 0, in->sw, in->sh - band};
        out->bg = s2_fit(in->bw, bh, box, DMD_S2_STRETCH);
        const int top = (in->sh - out->bg.h - band) / 2;   // the two together, centred
        out->bg.y = top;
        const dmd_rect_t d = {0, top + out->bg.h, in->sw, band};
        out->dmd = d;
        break;
    }
    case DMD_S2_HOUSING:
        out->bg = s2_fit(in->bw, in->bh, scr, 0);   // never stretched: the speakers stay round
        out->grill = in->grill > 0;
        out->dmd = s2_dmd(s2_map(in->housing, in->bw, in->bh, out->bg), in->dw, in->dh);
        break;
    case DMD_S2_ART:
        out->art = s2_fit(in->aw, in->ah, scr, DMD_S2_STRETCH);
        out->bg = out->art;   // under the art, out of sight
        out->dmd = s2_dmd(s2_map(in->window, in->aw, in->ah, out->art), in->dw, in->dh);
        break;
    case DMD_S2_BG_ART: {
        // both the whole width, one above the other, smaller together when taller than the screen
        const int64_t hb = (int64_t)in->sw * bh / in->bw, ha = (int64_t)in->sw * in->ah / in->aw;
        int w = in->sw;
        if (hb + ha > in->sh) w = (int)((int64_t)in->sw * in->sh / (hb + ha));
        const int h1 = (int)((int64_t)w * bh / in->bw), h2 = (int)((int64_t)w * in->ah / in->aw);
        const int x = (in->sw - w) / 2, y = (in->sh - h1 - h2) / 2;
        const dmd_rect_t b = {x, y, w, h1}, a = {x, y + h1, w, h2};
        out->bg = b;
        out->art = a;
        out->dmd = s2_dmd(s2_map(in->window, in->aw, in->ah, a), in->dw, in->dh);
        break;
    }
    default: return false;
    }
    return true;
}

static bool clip_rect(dmd_rect_t r, int dw, int dh, int *x0, int *y0, int *x1, int *y1) {
    *x0 = r.x < 0 ? 0 : r.x;
    *y0 = r.y < 0 ? 0 : r.y;
    *x1 = r.x + r.w > dw ? dw : r.x + r.w;
    *y1 = r.y + r.h > dh ? dh : r.y + r.h;
    return r.w > 0 && r.h > 0 && *x0 < *x1 && *y0 < *y1;
}

void dmd_fill565(uint16_t *dst, int dw, int dh, dmd_rect_t r, uint16_t c) {
    int x0, y0, x1, y1;
    if (!dst || !clip_rect(r, dw, dh, &x0, &y0, &x1, &y1)) return;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) dst[(size_t)y * (size_t)dw + (size_t)x] = c;
}

void dmd_blit565(uint16_t *dst, int dw, int dh, dmd_rect_t dr, const uint16_t *src, int sstride, dmd_rect_t sr) {
    int x0, y0, x1, y1;
    if (!dst || !src || sr.w < 1 || sr.h < 1 || !clip_rect(dr, dw, dh, &x0, &y0, &x1, &y1)) return;
    for (int y = y0; y < y1; y++) {
        const int sy = sr.y + (int)((int64_t)(y - dr.y) * sr.h / dr.h);
        const uint16_t *s = src + (size_t)sy * (size_t)sstride;
        uint16_t *d = dst + (size_t)y * (size_t)dw;
        for (int x = x0; x < x1; x++) d[x] = s[sr.x + (int)((int64_t)(x - dr.x) * sr.w / dr.w)];
    }
}

void dmd_blit_frame(uint16_t *dst, int dw, int dh, dmd_rect_t dr, const uint32_t *px, int w, int h) {
    int x0, y0, x1, y1;
    if (!dst || !px || w < 1 || h < 1 || !clip_rect(dr, dw, dh, &x0, &y0, &x1, &y1)) return;
    const bool gaps = dr.w >= 3 * w && dr.h >= 3 * h;   // dots big enough to show apart
    for (int y = y0; y < y1; y++) {
        const int64_t fy = (int64_t)(y - dr.y) * h;
        const int r = (int)(fy / dr.h);
        const bool ygap = gaps && (int)((fy + h) / dr.h) != r;   // the cell's last row
        uint16_t *d = dst + (size_t)y * (size_t)dw;
        for (int x = x0; x < x1; x++) {
            const int64_t fx = (int64_t)(x - dr.x) * w;
            const int c = (int)(fx / dr.w);
            if (ygap || (gaps && (int)((fx + w) / dr.w) != c)) { d[x] = 0; continue; }
            const uint32_t v = px[(size_t)r * (size_t)w + (size_t)c];
            d[x] = v ? (uint16_t)(((v >> 19) & 0x1F) << 11 | ((v >> 10) & 0x3F) << 5 | ((v >> 3) & 0x1F)) : 0x1082;
        }
    }
}
