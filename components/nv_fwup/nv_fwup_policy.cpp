// nv_fwup_policy — see nv_fwup_policy.h.
#include "nv_fwup_policy.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace nv_fwup_policy {

Verdict probation(const Probe &p) {
    if (p.ui_bad_s >= kUiHangS) return Verdict::Rollback;   // a frozen screen is a failed update
    if (p.uptime_s < kMinUptimeS || !p.ui_ok) return Verdict::Wait;
    if (p.net_proven) return Verdict::Confirm;
    if (p.online_first_s == 0) return p.uptime_s >= kOfflineGraceS ? Verdict::Confirm : Verdict::Wait;
    return p.uptime_s >= p.online_first_s + kNetGraceS ? Verdict::Rollback : Verdict::Wait;
}

int next_streak(int prev, Reset r) {
    if (prev < 0 || prev > 1000) prev = 0;   // garbage from a cold RTC memory
    switch (r) {
        case Reset::PowerOn:  return 0;
        case Reset::Crash:
        case Reset::Brownout: return prev + 1;   // cleared by kStableS of normal uptime
        case Reset::Software:
        case Reset::Other:    return prev;
    }
    return prev;
}

Mode boot_mode(int streak, bool has_rescue_source) {
    if (streak >= kRescueAt && has_rescue_source) return Mode::Rescue;
    if (streak >= kSafeModeAt) return Mode::Safe;
    return Mode::Normal;
}

AbortAction on_aborted(Reset r, int retries_done) {
    const bool not_its_fault = r == Reset::PowerOn || r == Reset::Brownout || r == Reset::Software;
    return (not_its_fault && retries_done < kMaxRetries) ? AbortAction::Retry : AbortAction::Restore;
}

int rollout_bucket(const uint8_t mac[6], const char *version) {
    // FNV-1a over the MAC and the version: stable per release, independent between releases (the
    // same devices are not always the first ones).
    uint32_t h = 2166136261u;
    for (int i = 0; i < 6; i++) { h ^= mac[i]; h *= 16777619u; }
    for (const char *s = version ? version : ""; *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; }
    h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12;   // spread the low bits
    return (int)(h % 100u);
}

bool in_rollout(int bucket, int rollout_pct) {
    if (rollout_pct < 0) rollout_pct = 0;
    if (rollout_pct > 100) rollout_pct = 100;
    return bucket < rollout_pct;
}

bool version_newer(const char *cand, const char *cur) {
    int a[3] = {0, 0, 0}, b[3] = {0, 0, 0};
    if (cand) sscanf(cand, "%d.%d.%d", &a[0], &a[1], &a[2]);
    if (cur) sscanf(cur, "%d.%d.%d", &b[0], &b[1], &b[2]);
    for (int i = 0; i < 3; i++) if (a[i] != b[i]) return a[i] > b[i];
    return false;
}

uint32_t crc32(uint32_t crc, const void *data, size_t n) {
    const uint8_t *p = static_cast<const uint8_t *>(data);
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

namespace {
template <size_t N> bool terminated(const char (&s)[N]) { return memchr(s, 0, N) != nullptr; }
}  // namespace

void set_field(char *dst, size_t cap, const char *src) {
    if (!dst || !cap) return;
    memset(dst, 0, cap);
    if (src) {
        const size_t n = strnlen(src, cap - 1);
        memcpy(dst, src, n);
    }
}

void journal_seal(Journal *j) {
    j->magic = kJournalMagic;
    j->crc = crc32(0, j, offsetof(Journal, crc));
}

bool journal_valid(const Journal &j) {
    return j.magic == kJournalMagic && j.crc == crc32(0, &j, offsetof(Journal, crc)) &&
           terminated(j.rescue_ver) && terminated(j.rescue_why) && terminated(j.retry_ver) &&
           terminated(j.res_op) && terminated(j.res_from) && terminated(j.res_to) && terminated(j.res_why) &&
           terminated(j.no_rescue_ver);
}

const Journal *journal_pick(const Journal *a, const Journal *b) {
    const bool va = a && journal_valid(*a), vb = b && journal_valid(*b);
    if (va && vb) return (int32_t)(a->seq - b->seq) >= 0 ? a : b;   // wrap-safe
    return va ? a : vb ? b : nullptr;
}

void lkg_seal(LkgHeader *h) {
    h->magic = kLkgMagic;
    h->format = kLkgFormat;
    h->crc = crc32(0, h, offsetof(LkgHeader, crc));
}

bool lkg_valid(const LkgHeader &h, uint32_t part_size) {
    if (h.magic != kLkgMagic || h.format != kLkgFormat) return false;
    if (h.crc != crc32(0, &h, offsetof(LkgHeader, crc))) return false;
    if (!terminated(h.version) || !h.version[0]) return false;
    if (h.method != kLkgRaw && h.method != kLkgDeflate) return false;
    if (!h.raw_size || !h.stored_size || part_size < kLkgDataOffset) return false;
    if (h.stored_size > part_size - kLkgDataOffset) return false;
    if (h.method == kLkgRaw && h.stored_size != h.raw_size) return false;
    return true;
}

}  // namespace nv_fwup_policy
