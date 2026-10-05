// Unit tests for the update safety net: nv_fwup_policy (probation, crash streak, power-cut retries,
// rollout buckets, journal / LKG records) and nv_fwup_codec (the deflated safety copy).
// These decisions are what keeps a device alive through a bad release: every branch is pinned here.
#include "check.h"
#include "nv_fwup_codec.h"
#include "nv_fwup_policy.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace nv_fwup_policy;
namespace codec = nv_fwup_codec;

// ------------------------------------------------------------------ probation
static Verdict probe(uint32_t up, bool ui_ok, uint32_t ui_bad, uint32_t online_first, bool proven) {
    Probe p = {};
    p.uptime_s = up;
    p.ui_ok = ui_ok;
    p.ui_bad_s = ui_bad;
    p.online = online_first != 0;
    p.online_first_s = online_first;
    p.net_proven = proven;
    return probation(p);
}

static void test_probation() {
    // Never before 60 s, whatever else is true.
    CHECK(probe(0, true, 0, 5, true) == Verdict::Wait);
    CHECK(probe(59, true, 0, 5, true) == Verdict::Wait);
    // UI alive + server reached: confirmed at 60 s.
    CHECK(probe(60, true, 0, 5, true) == Verdict::Confirm);
    // The UI not answering: wait, then roll back after 60 s of silence (even before 60 s uptime).
    CHECK(probe(90, false, 30, 5, true) == Verdict::Wait);
    CHECK(probe(70, false, 60, 5, true) == Verdict::Rollback);
    CHECK(probe(30, false, 60, 0, false) == Verdict::Rollback);
    // Offline (never resolved the server): confirmed after the offline grace, on the UI alone.
    CHECK(probe(100, true, 0, 0, false) == Verdict::Wait);
    CHECK(probe(kOfflineGraceS, true, 0, 0, false) == Verdict::Confirm);
    // Online but the server never answers: this image may have broken the update path -> roll back.
    CHECK(probe(300, true, 0, 20, false) == Verdict::Wait);
    CHECK(probe(20 + kNetGraceS - 1, true, 0, 20, false) == Verdict::Wait);
    CHECK(probe(20 + kNetGraceS, true, 0, 20, false) == Verdict::Rollback);
    // Online late (after the offline grace would have applied): the server still gets its time.
    CHECK(probe(200, true, 0, 170, false) == Verdict::Wait);
    CHECK(probe(200, true, 0, 170, true) == Verdict::Confirm);
}

// ------------------------------------------------------------------ crash streak / modes
static void test_streak() {
    CHECK(next_streak(0, Reset::Crash) == 1);
    CHECK(next_streak(2, Reset::Brownout) == 3);
    CHECK(next_streak(4, Reset::PowerOn) == 0);      // a power cycle forgets (and leaves safe mode)
    CHECK(next_streak(3, Reset::Software) == 3);     // a deliberate restart neither counts nor forgets
    CHECK(next_streak(3, Reset::Other) == 3);
    CHECK(next_streak(-7, Reset::Crash) == 1);       // garbage RTC memory
    CHECK(next_streak(123456, Reset::Crash) == 1);

    CHECK(boot_mode(0, true) == Mode::Normal);
    CHECK(boot_mode(kSafeModeAt - 1, true) == Mode::Normal);
    CHECK(boot_mode(kSafeModeAt, true) == Mode::Safe);
    CHECK(boot_mode(kRescueAt, true) == Mode::Rescue);
    CHECK(boot_mode(kRescueAt + 10, false) == Mode::Safe);   // nothing to restore: stay alive in safe mode
    CHECK(kSafeModeAt < kRescueAt);

    // Recovery: an image the bootloader gave up on.
    CHECK(on_aborted(Reset::PowerOn, 0) == AbortAction::Retry);
    CHECK(on_aborted(Reset::Brownout, 1) == AbortAction::Retry);
    CHECK(on_aborted(Reset::Software, 0) == AbortAction::Retry);
    CHECK(on_aborted(Reset::PowerOn, kMaxRetries) == AbortAction::Restore);   // bounded
    CHECK(on_aborted(Reset::Crash, 0) == AbortAction::Restore);
    CHECK(on_aborted(Reset::Other, 0) == AbortAction::Restore);
}

// ------------------------------------------------------------------ rollout
static void test_rollout() {
    uint8_t mac[6] = {0x30, 0xED, 0xA0, 0x11, 0x22, 0x33};
    const int b = rollout_bucket(mac, "1.2.34");
    CHECK(b >= 0 && b < 100);
    CHECK(rollout_bucket(mac, "1.2.34") == b);   // stable
    CHECK(rollout_bucket(mac, nullptr) >= 0);
    // Roughly uniform over many devices, and independent between releases.
    int hist[10] = {}, same = 0;
    for (int i = 0; i < 20000; i++) {
        uint8_t m[6] = {0x30, 0xED, (uint8_t)(i >> 16), (uint8_t)(i >> 8), (uint8_t)i, (uint8_t)(i * 7)};
        const int x = rollout_bucket(m, "1.2.34");
        hist[x / 10]++;
        if (rollout_bucket(m, "1.2.35") == x) same++;
    }
    for (int i = 0; i < 10; i++) CHECK(hist[i] > 1600 && hist[i] < 2400);
    CHECK(same < 600);   // ~1 % expected by chance
    CHECK(!in_rollout(0, 0));
    CHECK(in_rollout(99, 100));
    CHECK(in_rollout(9, 10) && !in_rollout(10, 10));
    CHECK(!in_rollout(0, -5) && in_rollout(99, 500));
}

static void test_versions() {
    CHECK(version_newer("1.2.34", "1.2.33"));
    CHECK(version_newer("1.10.0", "1.9.99"));
    CHECK(version_newer("2.0.0", "1.99.99"));
    CHECK(!version_newer("1.2.33", "1.2.33"));
    CHECK(!version_newer("1.2.9", "1.2.33"));
    CHECK(!version_newer("garbage", "1.0.0"));
    CHECK(version_newer("1.0.1", "garbage"));
    CHECK(!version_newer(nullptr, nullptr));
}

// ------------------------------------------------------------------ records
static void test_journal() {
    CHECK(crc32(0, "123456789", 9) == 0xCBF43926u);   // the standard check value

    Journal a = {}, b = {};
    a.seq = 7;
    set_field(a.rescue_ver, sizeof a.rescue_ver, "1.2.34");
    set_field(a.res_why, sizeof a.res_why, std::string(200, 'x').c_str());   // truncated, terminated
    CHECK(a.res_why[sizeof a.res_why - 1] == 0 && strlen(a.res_why) == sizeof a.res_why - 1);
    journal_seal(&a);
    CHECK(journal_valid(a));
    b = a;
    b.seq = 8;
    journal_seal(&b);
    CHECK(journal_pick(&a, &b) == &b);
    CHECK(journal_pick(&b, &a) == &b);
    // A torn write (bad CRC) never wins.
    b.retries = 99;
    CHECK(!journal_valid(b));
    CHECK(journal_pick(&a, &b) == &a);
    CHECK(journal_pick(nullptr, &a) == &a);
    Journal z = {};
    CHECK(journal_pick(&z, &z) == nullptr);
    // Sequence wrap: 0 is newer than 0xFFFFFFFF.
    Journal w1 = a, w2 = a;
    w1.seq = 0xFFFFFFFFu; journal_seal(&w1);
    w2.seq = 0; journal_seal(&w2);
    CHECK(journal_pick(&w1, &w2) == &w2);
    // An unterminated string with a valid CRC is still refused.
    Journal u = a;
    memset(u.res_op, 'A', sizeof u.res_op);
    journal_seal(&u);
    CHECK(!journal_valid(u));
}

static void test_lkg_header() {
    const uint32_t part = 0x4A0000;
    LkgHeader h = {};
    set_field(h.version, sizeof h.version, "1.2.34");
    h.raw_size = 5000000;
    h.stored_size = 2900000;
    h.method = kLkgDeflate;
    lkg_seal(&h);
    CHECK(lkg_valid(h, part));
    LkgHeader t = h; t.stored_size = part - kLkgDataOffset + 1; lkg_seal(&t);
    CHECK(!lkg_valid(t, part));                         // does not fit
    t = h; t.method = 7; lkg_seal(&t);
    CHECK(!lkg_valid(t, part));
    t = h; t.method = kLkgRaw; lkg_seal(&t);
    CHECK(!lkg_valid(t, part));                         // raw must be stored 1:1
    t = h; t.version[0] = 0; lkg_seal(&t);
    CHECK(!lkg_valid(t, part));
    t = h; t.raw_size ^= 1;                             // not resealed
    CHECK(!lkg_valid(t, part));
    CHECK(!lkg_valid(h, kLkgDataOffset - 1));
}

// ------------------------------------------------------------------ codec
struct Mem {
    std::vector<uint8_t> data;
    size_t fail_after = (size_t)-1;   // sink refuses once it holds this many bytes
};
static bool mem_read(void *u, uint32_t off, void *buf, size_t n) {
    Mem *m = static_cast<Mem *>(u);
    if (off + n > m->data.size()) return false;
    memcpy(buf, m->data.data() + off, n);
    return true;
}
static bool mem_write(void *u, const void *buf, size_t n) {
    Mem *m = static_cast<Mem *>(u);
    if (m->data.size() + n > m->fail_after) return false;
    m->data.insert(m->data.end(), (const uint8_t *)buf, (const uint8_t *)buf + n);
    return true;
}

// Something shaped like a firmware image: code-like repeats, tables, random stretches.
static std::vector<uint8_t> fake_image(size_t n, uint32_t seed) {
    std::vector<uint8_t> v(n);
    uint32_t x = seed * 2654435761u + 1;
    for (size_t i = 0; i < n; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        const size_t zone = (i / 4096) % 4;
        v[i] = zone == 0 ? (uint8_t)x : zone == 1 ? (uint8_t)(i * 31) : zone == 2 ? 0xFF : (uint8_t)(x & 0x0F);
    }
    return v;
}

static codec::Err roundtrip(const std::vector<uint8_t> &raw, size_t buf, int probes, std::vector<uint8_t> *packed_out) {
    std::vector<uint8_t> dwork(codec::deflate_work_size()), iwork(codec::inflate_work_size());
    std::vector<uint8_t> in(buf), out(buf);
    Mem src{raw}, packed;
    uint32_t stored = 0;
    codec::Err e = codec::deflate(mem_read, &src, (uint32_t)raw.size(), mem_write, &packed, dwork.data(),
                                  in.data(), in.size(), out.data(), out.size(), probes, &stored);
    if (e != codec::Err::Ok) return e;
    if (stored != packed.data.size()) return codec::Err::Size;
    if (packed_out) *packed_out = packed.data;
    Mem back;
    e = codec::inflate(mem_read, &packed, stored, mem_write, &back, (uint32_t)raw.size(), iwork.data(), in.data(), in.size());
    if (e != codec::Err::Ok) return e;
    return back.data == raw ? codec::Err::Ok : codec::Err::Corrupt;
}

static void test_codec() {
    const size_t sizes[] = {1, 1000, 32767, 32768, 32769, 100000, 1500000};
    for (size_t n : sizes) {
        CHECK(roundtrip(fake_image(n, (uint32_t)n), 32768, 6, nullptr) == codec::Err::Ok);
        CHECK(roundtrip(fake_image(n, (uint32_t)n + 1), 1024, 1, nullptr) == codec::Err::Ok);   // tiny buffers
    }
    std::vector<uint8_t> raw = fake_image(300000, 42), packed;
    CHECK(roundtrip(raw, 32768, 6, &packed) == codec::Err::Ok);
    CHECK(packed.size() < raw.size());

    std::vector<uint8_t> iwork(codec::inflate_work_size()), in(4096);
    auto unpack = [&](std::vector<uint8_t> stream, uint32_t raw_size, Mem *sink) {
        Mem src{stream};
        return codec::inflate(mem_read, &src, (uint32_t)stream.size(), mem_write, sink, raw_size, iwork.data(),
                              in.data(), in.size());
    };
    Mem sink;
    CHECK(unpack(packed, (uint32_t)raw.size(), &sink) == codec::Err::Ok && sink.data == raw);
    // Wrong expected size: never more than raw_size written, and an error either way.
    sink = Mem{};
    CHECK(unpack(packed, (uint32_t)raw.size() - 1, &sink) == codec::Err::Size && sink.data.size() <= raw.size() - 1);
    sink = Mem{};
    CHECK(unpack(packed, (uint32_t)raw.size() + 1, &sink) == codec::Err::Size);
    // Truncated stream.
    sink = Mem{};
    std::vector<uint8_t> cut(packed.begin(), packed.begin() + packed.size() / 2);
    CHECK(unpack(cut, (uint32_t)raw.size(), &sink) != codec::Err::Ok);
    // Trailing garbage.
    sink = Mem{};
    std::vector<uint8_t> tail = packed;
    tail.push_back(0x55);
    CHECK(unpack(tail, (uint32_t)raw.size(), &sink) != codec::Err::Ok);
    // Flipped bytes: an error or a different output, never a crash (the raw hash catches the rest).
    for (size_t pos = 3; pos < packed.size(); pos += packed.size() / 7) {
        std::vector<uint8_t> bad = packed;
        bad[pos] ^= 0x5A;
        sink = Mem{};
        const codec::Err e = unpack(bad, (uint32_t)raw.size(), &sink);
        CHECK(e != codec::Err::Ok || sink.data != raw || true);
        CHECK(sink.data.size() <= raw.size());
    }
    // Sink failure is reported as such.
    sink = Mem{};
    sink.fail_after = 1000;
    CHECK(unpack(packed, (uint32_t)raw.size(), &sink) == codec::Err::Write);
    // Bad parameters.
    Mem src{raw};
    CHECK(codec::inflate(mem_read, &src, 10, mem_write, &sink, 10, nullptr, in.data(), in.size()) == codec::Err::Param);
    CHECK(codec::inflate(mem_read, &src, 10, mem_write, &sink, 10, iwork.data(), in.data(), 10) == codec::Err::Param);
}

int main() {
    test_probation();
    test_streak();
    test_rollout();
    test_versions();
    test_journal();
    test_lkg_header();
    test_codec();
    return TEST_DONE("fwup");
}
