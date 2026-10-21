// nv_jpeg_util.h: the ABI v19 JPEG import's header probe (which streams the P4's hardware decoder takes)
// and the area-average RGB565 scaler (exact output size). The JPEG bytes come from the app.
#include "check.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
extern "C" {
#include "nv_jpeg_util.h"
}

// SOI, an APP0 segment, then a frame header: marker, precision, size, components (id, HV, Tq).
static std::vector<uint8_t> jpeg(int sof, int prec, int w, int h, std::vector<int> hv) {
    std::vector<uint8_t> v = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0};
    const int len = 8 + 3 * (int)hv.size();
    v.insert(v.end(), {0xFF, (uint8_t)sof, (uint8_t)(len >> 8), (uint8_t)len, (uint8_t)prec, (uint8_t)(h >> 8),
                       (uint8_t)h, (uint8_t)(w >> 8), (uint8_t)w, (uint8_t)hv.size()});
    for (size_t i = 0; i < hv.size(); i++) v.insert(v.end(), {(uint8_t)(i + 1), (uint8_t)hv[i], 0});
    v.insert(v.end(), {0xFF, 0xDA, 0x00, 0x08, 1, 1, 0, 0, 63, 0});
    return v;
}

static int probe(const std::vector<uint8_t> &v, nv_jpeg_hdr_t *h) { return nv_jpeg_probe(v.data(), v.size(), h); }

static std::vector<uint8_t> file(const char *path) {
    std::vector<uint8_t> v;
    FILE *f = std::fopen(path, "rb");
    if (!f) return v;
    int c;
    while ((c = std::fgetc(f)) != EOF) v.push_back((uint8_t)c);
    std::fclose(f);
    return v;
}

int main() {
    nv_jpeg_hdr_t h;
    // what the hardware takes, with the MCU it writes
    CHECK(probe(jpeg(0xC0, 8, 1000, 1383, {0x22, 0x11, 0x11}), &h) == NV_JPEG_OK);
    CHECK(h.w == 1000 && h.h == 1383 && h.nf == 3 && h.mcu_w == 16 && h.mcu_h == 16);
    CHECK(probe(jpeg(0xC0, 8, 640, 480, {0x21, 0x11, 0x11}), &h) == NV_JPEG_OK && h.mcu_w == 16 && h.mcu_h == 8);
    CHECK(probe(jpeg(0xC0, 8, 33, 17, {0x11, 0x11, 0x11}), &h) == NV_JPEG_OK && h.mcu_w == 8 && h.mcu_h == 8);
    CHECK(probe(jpeg(0xC0, 8, 4096, 4096, {0x11}), &h) == NV_JPEG_OK && h.nf == 1 && h.mcu_w == 8);
    CHECK(probe(jpeg(0xC0, 8, 64, 64, {0x22}), &h) == NV_JPEG_E_UNSUP);   // grey with a 2x2 "MCU"
    // what it doesn't: the size is still reported when a frame header was read
    CHECK(probe(jpeg(0xC2, 8, 800, 600, {0x22, 0x11, 0x11}), &h) == NV_JPEG_E_UNSUP && h.w == 800 && h.h == 600);
    CHECK(probe(jpeg(0xC1, 8, 800, 600, {0x22, 0x11, 0x11}), &h) == NV_JPEG_E_UNSUP);   // extended
    CHECK(probe(jpeg(0xC9, 8, 800, 600, {0x22, 0x11, 0x11}), &h) == NV_JPEG_E_UNSUP);   // arithmetic
    CHECK(probe(jpeg(0xC0, 12, 800, 600, {0x22, 0x11, 0x11}), &h) == NV_JPEG_E_UNSUP);  // 12-bit
    CHECK(probe(jpeg(0xC0, 8, 800, 600, {0x11, 0x11, 0x11, 0x11}), &h) == NV_JPEG_E_UNSUP);   // CMYK
    CHECK(probe(jpeg(0xC0, 8, 800, 600, {0x11, 0x11}), &h) == NV_JPEG_E_UNSUP);
    CHECK(probe(jpeg(0xC0, 8, 800, 600, {0x12, 0x11, 0x11}), &h) == NV_JPEG_E_UNSUP);   // 1x2 luma
    CHECK(probe(jpeg(0xC0, 8, 800, 600, {0x22, 0x21, 0x11}), &h) == NV_JPEG_E_UNSUP);   // subsampled chroma
    CHECK(probe(jpeg(0xC0, 8, 4097, 600, {0x22, 0x11, 0x11}), &h) == NV_JPEG_E_UNSUP);
    CHECK(probe(jpeg(0xC0, 8, 0, 600, {0x22, 0x11, 0x11}), &h) == NV_JPEG_E_UNSUP);
    // malformed: never read past the end
    std::vector<uint8_t> v = jpeg(0xC0, 8, 1000, 1383, {0x22, 0x11, 0x11});
    for (size_t n = 0; n < 32; n++) CHECK(nv_jpeg_probe(v.data(), n, &h) != NV_JPEG_OK);
    v[1] = 0xD9;
    CHECK(probe(v, &h) == NV_JPEG_E_DECODE);                               // not SOI
    v = jpeg(0xC0, 8, 1000, 1383, {0x22, 0x11, 0x11});
    v[4] = 0xFF; v[5] = 0xFF;                                              // APP0 longer than the data
    CHECK(probe(v, &h) == NV_JPEG_E_DECODE);
    v = jpeg(0xC0, 8, 1000, 1383, {0x22, 0x11, 0x11});
    v[22] = 0x00; v[23] = 0x08;                                            // SOF length short of 3 comps
    CHECK(probe(v, &h) == NV_JPEG_E_DECODE);
    // a progressive file: FF C0 inside its scan data must not be taken for a frame header
    v = jpeg(0xC2, 8, 64, 64, {0x22, 0x11, 0x11});
    v.insert(v.end(), {0xFF, 0xC0, 0x00, 0x11, 8, 0, 8, 0, 8, 3, 1, 0x22, 0, 2, 0x11, 0, 3, 0x11, 0});
    CHECK(probe(v, &h) == NV_JPEG_E_UNSUP && h.w == 64);
    // SOS before any frame header, then nothing
    const std::vector<uint8_t> sos = {0xFF, 0xD8, 0xFF, 0xDA, 0, 8, 1, 1, 0, 0, 63, 0, 0xFF, 0xC0};
    CHECK(probe(sos, &h) == NV_JPEG_E_DECODE);
    // real files written by Pillow (corpus/jpeg, tests/host/gen_seeds.py)
    struct { const char *f; int want, w, hh; } real[] = {
        {"corpus/jpeg/base420.jpg", NV_JPEG_OK, 96, 64}, {"corpus/jpeg/base444.jpg", NV_JPEG_OK, 96, 64},
        {"corpus/jpeg/gray.jpg", NV_JPEG_OK, 96, 64},     {"corpus/jpeg/prog.jpg", NV_JPEG_E_UNSUP, 96, 64},
        {"corpus/jpeg/cmyk.jpg", NV_JPEG_E_UNSUP, 96, 64},
    };
    for (const auto &r : real) {
        const std::vector<uint8_t> d = file(r.f);
        CHECK(!d.empty());
        if (d.empty()) { std::fprintf(stderr, "  missing %s\n", r.f); continue; }
        CHECK(probe(d, &h) == r.want && h.w == r.w && h.h == r.hh);
    }

    // scaler: exact size, means, enlarging repeats
    std::vector<uint32_t> acc(nv_scale_scratch_words(4096));
    {
        std::vector<uint16_t> src(1008 * 1392, 0x1234), dst(496 * 685, 0);
        nv_scale_area(src.data(), 2, 1000, 1383, 1008, dst.data(), 496, 685, acc.data());
        bool same = true;
        for (uint16_t p : dst) same = same && p == 0x1234;
        CHECK(same);
    }
    {   // black/white checkerboard halved: every output pixel is the mid grey of each channel
        std::vector<uint16_t> src(64 * 64), dst(32 * 32);
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++) src[y * 64 + x] = ((x ^ y) & 1) ? 0xFFFF : 0x0000;
        nv_scale_area(src.data(), 2, 64, 64, 64, dst.data(), 32, 32, acc.data());
        bool mid = true;
        for (uint16_t p : dst) mid = mid && (p >> 11) == 16 && ((p >> 5) & 63) == 32 && (p & 31) == 16;
        CHECK(mid);
    }
    {   // a left half red, right half blue picture keeps its halves; pitch wider than the picture
        std::vector<uint16_t> src(40 * 10, 0x07E0), dst(5 * 3);
        for (int y = 0; y < 10; y++)
            for (int x = 0; x < 30; x++) src[y * 40 + x] = x < 15 ? 0xF800 : 0x001F;
        nv_scale_area(src.data(), 2, 30, 10, 40, dst.data(), 5, 3, acc.data());
        CHECK(dst[0] == 0xF800 && dst[1] == 0xF800 && dst[3] == 0x001F && dst[4] == 0x001F);
        CHECK(dst[2] != 0xF800 && dst[2] != 0x001F);   // the middle column straddles: a blend
        bool green = false;                             // the padding beyond 30 is never read
        for (uint16_t p : dst) green = green || p == 0x07E0;
        CHECK(!green);
    }
    {   // 2x2 -> 5x3: every pixel comes from one source pixel
        const uint16_t src[4] = {1, 2, 3, 4};
        uint16_t dst[15];
        nv_scale_area(src, 2, 2, 2, 2, dst, 5, 3, acc.data());
        bool from_src = true;
        for (uint16_t p : dst) from_src = from_src && p >= 1 && p <= 4;
        CHECK(from_src && dst[0] == 1 && dst[4] == 2 && dst[10] == 3 && dst[14] == 4);
    }
    {   // grey: a mean of grey levels, made grey RGB565; uniform stays exact
        std::vector<uint8_t> src(24 * 8, 200);
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 16; x++) src[y * 24 + x] = (x & 1) ? 255 : 0;   // pitch 24
        std::vector<uint16_t> dst(8 * 4);
        nv_scale_area(src.data(), 1, 16, 8, 24, dst.data(), 8, 4, acc.data());
        const uint16_t g128 = (uint16_t)((128 >> 3) << 11 | (128 >> 2) << 5 | (128 >> 3));
        bool ok = true;
        for (uint16_t p : dst) ok = ok && p == g128;   // (0 + 255) / 2 rounds to 128
        CHECK(ok);
        std::vector<uint8_t> flat(100 * 100, 77);
        std::vector<uint16_t> d2(33 * 7);
        nv_scale_area(flat.data(), 1, 100, 100, 100, d2.data(), 33, 7, acc.data());
        const uint16_t g77 = (uint16_t)((77 >> 3) << 11 | (77 >> 2) << 5 | (77 >> 3));
        ok = true;
        for (uint16_t p : d2) ok = ok && p == g77;
        CHECK(ok);
    }
    {   // the biggest downscale (sum near the 32-bit edge): 4096x4096 white grey -> 1x1 stays white
        std::vector<uint8_t> white((size_t)4096 * 4096, 255);
        uint16_t one = 0;
        nv_scale_area(white.data(), 1, 4096, 4096, 4096, &one, 1, 1, acc.data());
        CHECK(one == 0xFFFF);
    }
    {   // every path against a brute-force mean (same rounding): the packed RGB565 path (<= 32 pixels per
        // output), the 3-channel one (more), enlarging, grey
        const int cases[][5] = {{1000, 1383, 496, 685, 2}, {640, 480, 100, 70, 2}, {37, 23, 120, 77, 2},
                                {333, 111, 64, 20, 2}, {1000, 1383, 496, 685, 1}, {300, 200, 31, 17, 1}};
        uint32_t seed = 12345;
        for (const auto &c : cases) {
            const int sw = c[0], sh = c[1], dw = c[2], dh = c[3], bpp = c[4], pitch = sw + 3;
            std::vector<uint8_t> src((size_t)pitch * sh * bpp);
            for (auto &b : src) { seed = seed * 1103515245u + 12345u; b = (uint8_t)(seed >> 16); }
            std::vector<uint16_t> out((size_t)dw * dh);
            nv_scale_area(src.data(), bpp, sw, sh, pitch, out.data(), dw, dh, acc.data());
            int bad = 0;
            for (int oy = 0; oy < dh; oy++) {
                int y0 = oy * sh / dh, y1 = (oy + 1) * sh / dh;
                if (y1 <= y0) y1 = y0 + 1;
                for (int ox = 0; ox < dw; ox++) {
                    int x0 = ox * sw / dw, x1 = (ox + 1) * sw / dw;
                    if (x1 <= x0) x1 = x0 + 1;
                    uint32_t r = 0, g = 0, b = 0;
                    for (int y = y0; y < y1; y++)
                        for (int x = x0; x < x1; x++) {
                            if (bpp == 2) {
                                const uint32_t v = src[((size_t)y * pitch + x) * 2] | src[((size_t)y * pitch + x) * 2 + 1] << 8;
                                r += v >> 11; g += (v >> 5) & 63; b += v & 31;
                            } else {
                                g += src[(size_t)y * pitch + x];
                            }
                        }
                    const uint32_t k = (1u << 24) / (uint32_t)((x1 - x0) * (y1 - y0));
                    auto m = [k](uint32_t s) { return (s * k + (1u << 23)) >> 24; };
                    const uint16_t want = bpp == 2 ? (uint16_t)(m(r) << 11 | m(g) << 5 | m(b))
                                                   : (uint16_t)((m(g) >> 3) << 11 | (m(g) >> 2) << 5 | (m(g) >> 3));
                    if (out[(size_t)oy * dw + ox] != want) bad++;
                }
            }
            CHECK(bad == 0);
            if (bad) std::fprintf(stderr, "  %dx%d -> %dx%d bpp %d: %d pixels differ\n", sw, sh, dw, dh, bpp, bad);
        }
    }
    {   // rows come in non-decreasing order, each in range (the firmware streams them into internal RAM
        // by bands), shrinking and enlarging; a NULL row stops the scale
        struct Rows { std::vector<uint16_t> px; int sw, sh, last, bad, calls, stop_at; };
        auto fn = [](void *ctx, int y) -> const void * {
            Rows *r = (Rows *)ctx;
            if (y < r->last || y < 0 || y >= r->sh) r->bad++;
            r->last = y;
            if (++r->calls == r->stop_at) return nullptr;
            return r->px.data() + (size_t)y * r->sw;
        };
        const int sizes[][4] = {{1000, 1383, 496, 685}, {7, 5, 31, 23}, {640, 3, 3, 640}, {1, 1, 9, 9}};
        for (const auto &s : sizes) {
            Rows r{std::vector<uint16_t>((size_t)s[0] * s[1], 0x5555), s[0], s[1], -1, 0, 0, -1};
            std::vector<uint16_t> out((size_t)s[2] * s[3]);
            CHECK(nv_scale_area_rows(fn, &r, 2, s[0], s[1], out.data(), s[2], s[3], acc.data()));
            CHECK(r.bad == 0 && r.calls >= s[1]);
        }
        Rows r{std::vector<uint16_t>(64 * 64), 64, 64, -1, 0, 0, 10};
        std::vector<uint16_t> out(16 * 16);
        CHECK(!nv_scale_area_rows(fn, &r, 2, 64, 64, out.data(), 16, 16, acc.data()));
    }
    return TEST_DONE("jpeg");
}
