// Tiny RIFF/AVI writer for the tests: enough structure to drive vp_avi (hdrl/avih/strl/movi/idx1,
// OpenDML indx + ix00), with every size field overridable to build hostile files.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using Bytes = std::vector<uint8_t>;

inline void put32(Bytes &b, uint32_t v) { for (int i = 0; i < 4; i++) b.push_back((uint8_t)(v >> (8 * i))); }
inline void put16(Bytes &b, uint16_t v) { b.push_back((uint8_t)v); b.push_back((uint8_t)(v >> 8)); }
inline void putid(Bytes &b, const char *id) { b.insert(b.end(), id, id + 4); }
inline void cat(Bytes &b, const Bytes &x) { b.insert(b.end(), x.begin(), x.end()); }

// chunk: id + size + payload (+ pad byte). size_override != ~0u writes a lying size field.
inline Bytes chunk(const char *id, const Bytes &payload, uint32_t size_override = ~0u) {
    Bytes b; putid(b, id); put32(b, size_override != ~0u ? size_override : (uint32_t)payload.size());
    cat(b, payload); if (payload.size() & 1) b.push_back(0);
    return b;
}
inline Bytes list(const char *type, const Bytes &children, uint32_t size_override = ~0u) {
    Bytes p; putid(p, type); cat(p, children);
    return chunk("LIST", p, size_override);
}

inline Bytes avih(uint32_t frames, uint32_t w, uint32_t h) {
    Bytes b; put32(b, 100000); put32(b, 0); put32(b, 0); put32(b, 0x10); put32(b, frames); put32(b, 0);
    put32(b, 2); put32(b, 0); put32(b, w); put32(b, h); for (int i = 0; i < 4; i++) put32(b, 0);
    return chunk("avih", b);
}
inline Bytes strh(const char *type, const char *handler, uint32_t scale, uint32_t rate) {
    Bytes b; putid(b, type); putid(b, handler); put32(b, 0); put32(b, 0); put32(b, 0);
    put32(b, scale); put32(b, rate); put32(b, 0); put32(b, 0); put32(b, 0); put32(b, 0); put32(b, 0);
    for (int i = 0; i < 4; i++) put16(b, 0);
    return chunk("strh", b);
}
inline Bytes strf_vids(uint32_t w, uint32_t h) {
    Bytes b; put32(b, 40); put32(b, w); put32(b, h); put16(b, 1); put16(b, 24); putid(b, "MJPG");
    for (int i = 0; i < 5; i++) put32(b, 0);
    return chunk("strf", b);
}
inline Bytes strf_auds(uint16_t ch, uint32_t rate) {
    Bytes b; put16(b, 1); put16(b, ch); put32(b, rate); put32(b, rate * ch * 2); put16(b, (uint16_t)(ch * 2)); put16(b, 16);
    return chunk("strf", b);
}

// A playable MJPEG+PCM AVI: `frames` video chunks of `vsize` bytes interleaved with audio chunks, and an
// idx1 whose offsets are relative to the 'movi' fourcc (the common convention). Returns the file.
struct AviLayout { size_t movi_fourcc = 0; std::vector<size_t> vpay, apay; };
inline Bytes make_avi(int frames, uint32_t vsize, bool with_idx1, AviLayout *lay = nullptr) {
    Bytes hdrl = avih((uint32_t)frames, 64, 48);
    cat(hdrl, list("strl", [] { Bytes s = strh("vids", "MJPG", 1, 10); cat(s, strf_vids(64, 48)); return s; }()));
    cat(hdrl, list("strl", [] { Bytes s = strh("auds", "\0\0\0\0", 1, 8000); cat(s, strf_auds(1, 8000)); return s; }()));
    Bytes riff; putid(riff, "AVI "); cat(riff, list("hdrl", hdrl));
    const size_t movi_list_at = 12 + riff.size() - 4;   // absolute offset of the movi LIST header
    Bytes movi, idx;
    AviLayout L; L.movi_fourcc = movi_list_at + 8;
    for (int i = 0; i < frames; i++) {
        Bytes v(vsize, (uint8_t)(0xA0 + i)); v[0] = 0xFF; v[1] = 0xD8;
        const size_t rel = 4 + movi.size();              // from the 'movi' fourcc
        L.vpay.push_back(L.movi_fourcc + rel + 8);
        cat(movi, chunk("00dc", v));
        putid(idx, "00dc"); put32(idx, 0x10); put32(idx, (uint32_t)rel); put32(idx, vsize);
        Bytes a(160, 0x11);
        const size_t arel = 4 + movi.size();
        L.apay.push_back(L.movi_fourcc + arel + 8);
        cat(movi, chunk("01wb", a));
        putid(idx, "01wb"); put32(idx, 0); put32(idx, (uint32_t)arel); put32(idx, 160);
    }
    cat(riff, list("movi", movi));
    if (with_idx1) cat(riff, chunk("idx1", idx));
    Bytes file; putid(file, "RIFF"); put32(file, (uint32_t)riff.size()); cat(file, riff);
    if (lay) *lay = L;
    return file;
}
