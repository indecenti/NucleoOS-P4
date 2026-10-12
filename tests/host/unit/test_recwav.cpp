// Unit tests for nv_rec_wav.h (the voice recorder's ring and WAV header): the ring keeps every byte
// in order across wraps and under a concurrent producer/consumer, refuses whole chunks when full and
// counts them; the 512-byte header puts the samples on a sector and is read back by the chunk walker
// (as by every WAV reader), old 44-byte takes still parse, and hostile chunk sizes end the walk.
#include "check.h"
#include "nv_rec_wav.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

static void test_ring_basic() {
    std::vector<uint8_t> mem(64);
    NvRecRing r;
    CHECK(!r.init(mem.data(), 48));          // not a power of two
    CHECK(r.init(mem.data(), 64));
    uint8_t a[40], b[64];
    for (int i = 0; i < 40; i++) a[i] = (uint8_t)i;
    CHECK(r.push(a, 40) && r.used() == 40);
    CHECK(!r.push(a, 30) && r.dropped.load() == 30 && r.used() == 40);   // whole chunk refused
    CHECK(r.pop(b, 25) == 25 && !memcmp(b, a, 25) && r.used() == 15);
    CHECK(r.push(a, 40) && r.used() == 55);   // wraps the end of the buffer
    CHECK(r.pop(b, 64) == 55);
    CHECK(!memcmp(b, a + 25, 15) && !memcmp(b + 15, a, 40));
    CHECK(r.pop(b, 8) == 0 && r.used() == 0);
}

// The running counters wrap (size_t on the 32-bit P4): masking keeps the place right.
static void test_ring_counter_wrap() {
    std::vector<uint8_t> mem(16);
    NvRecRing r;
    r.init(mem.data(), 16);
    r.head.store((size_t)0 - 8);
    r.tail.store((size_t)0 - 8);
    uint8_t a[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}, b[12] = {};
    CHECK(r.push(a, 12) && r.used() == 12);
    CHECK(r.pop(b, 12) == 12 && !memcmp(a, b, 12) && r.used() == 0);
}

// A producer and a consumer on two threads, a ring far smaller than the stream: every byte the
// producer stored comes out once and in order, and stored + dropped = offered.
static void test_ring_threads() {
    std::vector<uint8_t> mem(1 << 12);
    NvRecRing r;
    r.init(mem.data(), mem.size());
    const uint32_t chunks = 20000, csz = 96;
    std::atomic<bool> done{false};
    uint64_t stored = 0;
    std::thread prod([&] {
        uint8_t c[csz];
        for (uint32_t k = 0; k < chunks; k++) {
            memcpy(c, &k, 4);                 // chunk index, then a pattern from it
            for (uint32_t i = 4; i < csz; i++) c[i] = (uint8_t)(k * 31 + i);
            if (r.push(c, csz)) stored += csz;
            if (k % 64 == 0) std::this_thread::yield();
        }
        done = true;
    });
    std::vector<uint8_t> out;
    out.reserve((size_t)chunks * csz);
    uint8_t blk[1000];
    for (;;) {
        const bool fin = done.load();
        const size_t n = r.pop(blk, sizeof blk);
        out.insert(out.end(), blk, blk + n);
        if (!n && fin && !r.used()) break;
    }
    prod.join();
    CHECK(out.size() == stored);
    CHECK(stored + r.dropped.load() == (uint64_t)chunks * csz);
    bool ordered = out.size() % csz == 0;
    uint32_t last = 0;
    for (size_t o = 0; ordered && o < out.size(); o += csz) {
        uint32_t k;
        memcpy(&k, &out[o], 4);
        if (o && k <= last) ordered = false;
        for (uint32_t i = 4; ordered && i < csz; i++) if (out[o + i] != (uint8_t)(k * 31 + i)) ordered = false;
        last = k;
    }
    CHECK(ordered);
}

static void test_header() {
    uint8_t h[kNvWavHeader];
    nv_wav_header512(h, 48000, 1, 16, 96000 * 3);
    uint32_t br = 0, off = 0, len = 0;
    CHECK(nv_wav_find_data(h, sizeof h, &br, &off, &len));
    CHECK(br == 96000 && off == 512 && len == 96000u * 3);
    CHECK(!memcmp(h + 36, "JUNK", 4) && !memcmp(h + 504, "data", 4));
    const uint32_t riff = h[4] | h[5] << 8 | h[6] << 16 | (uint32_t)h[7] << 24;
    CHECK(riff == 504 + 96000u * 3);           // file size - 8
    nv_wav_header512(h, 48000, 1, 16, 0);      // a take still being written: no length yet
    CHECK(nv_wav_find_data(h, sizeof h, &br, &off, &len) && off == 512 && len == 0);

    // A take of the old recorder: 44-byte header.
    uint8_t o[44] = {'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0, 1,0, 1,0,
                     0x80,0xBB,0,0, 0,0x77,1,0, 2,0, 16,0, 'd','a','t','a', 0x10,0x27,0,0};
    CHECK(nv_wav_find_data(o, sizeof o, &br, &off, &len) && br == 96000 && off == 44 && len == 10000);

    // Hostile or cut headers end the walk.
    uint8_t bad[64];
    memcpy(bad, o, 44);
    memcpy(bad + 36, "LIST", 4);
    bad[40] = 0xFF; bad[41] = 0xFF; bad[42] = 0xFF; bad[43] = 0xFF;   // a chunk size that would wrap
    CHECK(!nv_wav_find_data(bad, sizeof bad, &br, &off, &len));
    CHECK(!nv_wav_find_data(o, 30, &br, &off, &len));             // data chunk past the bytes read
    CHECK(!nv_wav_find_data(nullptr, 44, &br, &off, &len));
    memcpy(bad, "RIFX", 4);
    CHECK(!nv_wav_find_data(bad, sizeof bad, &br, &off, &len));
}

int main() {
    test_ring_basic();
    test_ring_counter_wrap();
    test_ring_threads();
    test_header();
    return TEST_DONE("recwav");
}
