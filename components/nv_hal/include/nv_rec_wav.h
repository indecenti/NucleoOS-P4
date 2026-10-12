// nv_rec_wav — the pure parts of the voice recorder (nv_audio.cpp), host-tested in
// tests/host/unit/test_recwav.cpp.
//
// Why a ring: the I2S DMA holds ~30 ms of capture, and an SD write can stall for hundreds of
// milliseconds (FAT allocation, the card's own housekeeping). When the task that reads the mic
// also wrote the file, every such stall lost audio (a 40 s take came out as 34 s). Now the mic
// task only fills NvRecRing; a writer task drains it to the card in large blocks.
//
// Why a 512-byte header: the SD driver DMAs straight from the buffer only when the buffer is
// cache-line aligned and the file offset is on a sector; otherwise it bounces one sector per
// command, too slow for 96 KB/s. A "JUNK" chunk (the standard WAV padding chunk, skipped by every
// reader) puts the samples at offset 512.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Single producer (the mic task), single consumer (the writer task). The capacity is a power of
// two, so the running byte counters may wrap.
struct NvRecRing {
    uint8_t *buf = nullptr;
    size_t   cap = 0;
    std::atomic<size_t>   head{0};      // bytes ever stored (producer)
    std::atomic<size_t>   tail{0};      // bytes ever taken (consumer)
    std::atomic<uint32_t> dropped{0};   // bytes refused because the ring was full

    // `n` must be a power of two.
    bool init(uint8_t *mem, size_t n) {
        if (!mem || !n || (n & (n - 1))) return false;
        buf = mem;
        cap = n;
        head.store(0);
        tail.store(0);
        dropped.store(0);
        return true;
    }
    size_t used() const { return head.load(std::memory_order_acquire) - tail.load(std::memory_order_acquire); }

    // Producer: all of `n` bytes or none (a chunk is dropped whole, so samples stay aligned).
    bool push(const void *src, size_t n) {
        const size_t h = head.load(std::memory_order_relaxed);
        const size_t t = tail.load(std::memory_order_acquire);
        if (n > cap - (h - t)) { dropped.fetch_add((uint32_t)n, std::memory_order_relaxed); return false; }
        const size_t at = h & (cap - 1), first = n < cap - at ? n : cap - at;
        memcpy(buf + at, src, first);
        memcpy(buf, (const uint8_t *)src + first, n - first);
        head.store(h + n, std::memory_order_release);
        return true;
    }

    // Consumer: up to `n` bytes out; returns how many.
    size_t pop(void *dst, size_t n) {
        const size_t t = tail.load(std::memory_order_relaxed);
        const size_t h = head.load(std::memory_order_acquire);
        size_t k = h - t;
        if (k > n) k = n;
        const size_t at = t & (cap - 1), first = k < cap - at ? k : cap - at;
        memcpy(dst, buf + at, first);
        memcpy((uint8_t *)dst + first, buf, k - first);
        tail.store(t + k, std::memory_order_release);
        return k;
    }
};

constexpr size_t kNvWavHeader = 512;   // where the samples start in a take

// The 512-byte header of a PCM take holding `data_bytes` of samples: RIFF, fmt, JUNK, data.
inline void nv_wav_header512(uint8_t out[kNvWavHeader], uint32_t rate, uint16_t ch, uint16_t bits,
                             uint32_t data_bytes) {
    auto w16 = [&](size_t o, uint16_t v) { out[o] = (uint8_t)v; out[o + 1] = (uint8_t)(v >> 8); };
    auto w32 = [&](size_t o, uint32_t v) { for (int i = 0; i < 4; i++) out[o + i] = (uint8_t)(v >> (8 * i)); };
    memset(out, 0, kNvWavHeader);
    memcpy(out, "RIFF", 4);
    w32(4, (uint32_t)(kNvWavHeader - 8) + data_bytes);
    memcpy(out + 8, "WAVE", 4);
    memcpy(out + 12, "fmt ", 4);
    w32(16, 16);
    w16(20, 1);                                   // PCM
    w16(22, ch);
    w32(24, rate);
    w32(28, rate * ch * bits / 8);                // byte rate
    w16(32, (uint16_t)(ch * bits / 8));           // block align
    w16(34, bits);
    memcpy(out + 36, "JUNK", 4);
    w32(40, (uint32_t)(kNvWavHeader - 8 - 44));   // up to the data chunk header at 504
    memcpy(out + kNvWavHeader - 8, "data", 4);
    w32(kNvWavHeader - 4, data_bytes);
}

// Walk the chunks of a WAV's first `n` bytes: the byte rate, where the samples start and how many
// bytes the header says there are. False when there is no fmt + data. Strides come from the file,
// so a size that would overrun or wrap ends the walk.
inline bool nv_wav_find_data(const uint8_t *h, size_t n, uint32_t *byterate, uint32_t *data_off,
                             uint32_t *data_len) {
    if (!h || n < 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return false;
    auto r32 = [&](size_t o) { return (uint32_t)h[o] | (uint32_t)h[o + 1] << 8 | (uint32_t)h[o + 2] << 16 | (uint32_t)h[o + 3] << 24; };
    uint32_t br = 0;
    size_t off = 12;
    while (off + 8 <= n) {
        const uint32_t csz = r32(off + 4);
        if (!memcmp(h + off, "fmt ", 4) && off + 20 <= n) br = r32(off + 16);
        if (!memcmp(h + off, "data", 4)) {
            if (!br) return false;
            if (byterate) *byterate = br;
            if (data_off) *data_off = (uint32_t)(off + 8);
            if (data_len) *data_len = csz;
            return true;
        }
        if (csz > n) return false;
        off += 8 + csz + (csz & 1);
    }
    return false;
}
