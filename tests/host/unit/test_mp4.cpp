// vp_mp4: moov parsing + keyframe seek, on well-formed and hostile boxes.
#include "check.h"
#include "vp_mp4.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

using Bytes = std::vector<uint8_t>;
static void be32(Bytes &b, uint32_t v) { for (int i = 3; i >= 0; i--) b.push_back((uint8_t)(v >> (8 * i))); }
static void be16(Bytes &b, uint16_t v) { b.push_back((uint8_t)(v >> 8)); b.push_back((uint8_t)v); }
static void cat(Bytes &b, const Bytes &x) { b.insert(b.end(), x.begin(), x.end()); }
static Bytes box(const char *type, const Bytes &payload, uint32_t size_override = 0) {
    Bytes b; be32(b, size_override ? size_override : (uint32_t)(8 + payload.size()));
    b.insert(b.end(), type, type + 4); cat(b, payload); return b;
}
static Bytes full(uint32_t verflags) { Bytes b; be32(b, verflags); return b; }

// One video trak: 4 samples in 2 chunks (2 per chunk), keyframes 1 and 3 (1-based), 30 fps.
static Bytes video_trak(const Bytes &stss_payload) {
    Bytes hdlr = full(0); be32(hdlr, 0); hdlr.insert(hdlr.end(), {'v','i','d','e'}); for (int i = 0; i < 3; i++) be32(hdlr, 0); hdlr.push_back(0);
    Bytes mdhd = full(0); be32(mdhd, 0); be32(mdhd, 0); be32(mdhd, 3000); be32(mdhd, 400); be32(mdhd, 0);
    Bytes avcc = {1, 66, 0, 30, 0xFF, 0xE1}; be16(avcc, 4); cat(avcc, {0x67, 66, 0, 30}); avcc.push_back(1); be16(avcc, 2); cat(avcc, {0x68, 0xCE});
    Bytes avc1(78, 0); cat(avc1, box("avcC", avcc));
    Bytes stsd = full(0); be32(stsd, 1); cat(stsd, box("avc1", avc1));
    Bytes stts = full(0); be32(stts, 1); be32(stts, 4); be32(stts, 100);   // 100/3000 s = 33 ms
    Bytes stsc = full(0); be32(stsc, 1); be32(stsc, 1); be32(stsc, 2); be32(stsc, 1);
    Bytes stsz = full(0); be32(stsz, 0); be32(stsz, 4); for (uint32_t s : {1000u, 200u, 900u, 150u}) be32(stsz, s);
    Bytes stco = full(0); be32(stco, 2); be32(stco, 4096); be32(stco, 9000);
    Bytes stbl = box("stsd", stsd); cat(stbl, box("stts", stts)); cat(stbl, box("stsc", stsc));
    cat(stbl, box("stsz", stsz)); cat(stbl, box("stco", stco)); cat(stbl, box("stss", stss_payload));
    Bytes minf = box("stbl", stbl);
    Bytes mdia = box("hdlr", hdlr); cat(mdia, box("mdhd", mdhd)); cat(mdia, box("minf", minf));
    return box("trak", box("mdia", mdia));
}
static Bytes stss(std::initializer_list<uint32_t> keys) { Bytes b = full(0); be32(b, (uint32_t)keys.size()); for (uint32_t k : keys) be32(b, k); return b; }
static Bytes moov_of(std::initializer_list<Bytes> traks) { Bytes p; for (auto &t : traks) cat(p, t); return box("moov", p); }

// Parse from an exactly-sized heap copy, so ASan flags any read past the moov buffer.
static void parse(const Bytes &moov, vp_track_t *V, vp_track_t *A) {
    uint8_t *m = (uint8_t *)malloc(moov.size() ? moov.size() : 1);
    if (!moov.empty()) memcpy(m, moov.data(), moov.size());
    *V = {}; *A = {};
    vp_mp4_parse_moov(m, (uint32_t)moov.size(), V, A);
    V->sps = V->pps = nullptr;   // they point into m
    free(m);
}

int main() {
    vp_track_t V, A;

    // --- well-formed
    parse(moov_of({video_trak(stss({1, 3}))}), &V, &A);
    CHECK(V.present && V.nsamp == 4 && !A.present);
    CHECK(V.tbl[0].offset == 4096 && V.tbl[0].size == 1000);
    CHECK(V.tbl[1].offset == 5096 && V.tbl[1].size == 200);
    CHECK(V.tbl[2].offset == 9000 && V.tbl[3].offset == 9900);
    CHECK(V.period_ms == 33 && V.nal_len_size == 4 && V.sps_len == 4 && V.pps_len == 2);
    CHECK(V.sync_count == 2 && V.sync[0] == 0 && V.sync[1] == 2);
    CHECK(vp_mp4_seek_video_index(&V, 0) == 0);
    CHECK(vp_mp4_seek_video_index(&V, 70) == 2);      // target 2 -> keyframe 2
    CHECK(vp_mp4_seek_video_index(&V, 999999) == 2);  // clamped to the last sample, nearest keyframe
    CHECK(vp_mp4_seek_video_index(&V, -5) == 0);
    vp_mp4_track_free(&V);

    // --- hostile stss (regression): entry 0 becomes 0xFFFFFFFF after the 1-based shift, and indices
    // past the table; the seek must still land inside the sample table
    parse(moov_of({video_trak(stss({0, 0x7FFFFFFF, 2}))}), &V, &A);
    CHECK(V.present);
    for (int want : {0, 33, 70, 1000, 5000}) CHECK(vp_mp4_seek_video_index(&V, want) < V.nsamp);
    vp_mp4_track_free(&V);
    parse(moov_of({video_trak(stss({4000000000u}))}), &V, &A);
    CHECK(vp_mp4_seek_video_index(&V, 100) < V.nsamp);
    vp_mp4_track_free(&V);

    // --- hostile box size (regression): 0xFFFFFFF8 made `p + size` wrap on the 32-bit device, skip
    // the clamp and walk the parser off the buffer. Parsing must stay inside (ASan) and still work.
    // The lying box follows the trak at moov level, so parse_moov's walk has to step over it.
    parse(moov_of({video_trak(stss({1})), box("free", Bytes(16, 0), 0xFFFFFFF8u)}), &V, &A);
    CHECK(V.present && V.nsamp == 4);
    vp_mp4_track_free(&V);
    parse(moov_of({box("free", Bytes(16, 0), 0xFFFFFFF0u), video_trak(stss({1}))}), &V, &A);
    vp_mp4_track_free(&V);

    // --- two video traks: the first wins, the second must not leak or overwrite its tables
    parse(moov_of({video_trak(stss({1})), video_trak(stss({1, 2}))}), &V, &A);
    CHECK(V.present && V.sync_count == 1);
    vp_mp4_track_free(&V);

    // --- truncations of the valid moov at every length: never a crash
    Bytes whole = moov_of({video_trak(stss({1, 3}))});
    for (size_t n = 0; n <= whole.size(); n++) {
        Bytes t(whole.begin(), whole.begin() + n);
        parse(t, &V, &A);
        if (V.present) CHECK(V.nsamp <= 4);
        vp_mp4_track_free(&V); vp_mp4_track_free(&A);
    }

    return TEST_DONE("mp4");
}
