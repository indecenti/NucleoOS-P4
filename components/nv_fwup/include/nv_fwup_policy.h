// nv_fwup_policy — the decisions that keep a device alive through updates, as pure code (no ESP-IDF),
// shared by NucleoOS (nv_ota) and the recovery app, unit-tested and fuzzed in tests/host.
//
//   * probation: when a freshly installed system has proven itself (confirm), must be rolled back,
//     or is still being watched;
//   * crash streak: consecutive abnormal resets -> normal boot, safe mode, or a rescue request;
//   * aborted image: a system the bootloader gave up on is retried when the reset was not its fault
//     (power cut, brownout, a deliberate restart), restored from the safety copy otherwise;
//   * staged rollout: which percentage bucket this device falls in for a given release;
//   * the on-flash records (journal and last-known-good header) in the `assets` partition.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace nv_fwup_policy {

// ------------------------------------------------------------------ resets
enum class Reset : uint8_t {
    PowerOn,    // cold start, power cut, external reset pin
    Software,   // esp_restart(): a deliberate restart (user, updater, a watchdog of ours)
    Crash,      // panic, abort, interrupt/task watchdog, CPU lockup
    Brownout,   // supply dipped (speaker peaks on weak USB power)
    Other,      // deep sleep wake, unknown
};

// ------------------------------------------------------------------ probation (fresh image)
// What the system knows about itself while its image is still PENDING_VERIFY.
struct Probe {
    uint32_t uptime_s;       // since boot
    bool     ui_ok;          // the UI task answered the last probe (true when there is no UI)
    uint32_t ui_bad_s;       // how long the UI has been failing its probes without a break
    bool     online;         // the update server's name resolves (DNS works: we are on the internet)
    uint32_t online_first_s; // uptime when it first resolved (0 = never)
    bool     net_proven;     // the update server answered (HTTP over TLS) during this boot
};

enum class Verdict : uint8_t { Wait, Confirm, Rollback };

constexpr uint32_t kMinUptimeS     = 60;    // never confirm sooner (the 1.1.57 lesson)
constexpr uint32_t kUiHangS        = 60;    // UI unresponsive this long while on probation -> roll back
constexpr uint32_t kOfflineGraceS  = 180;   // not on the internet by then: judge on what we can see
constexpr uint32_t kNetGraceS      = 600;   // on the internet, yet the update server never answered:
                                            // this image may have broken the update path itself
                                            // (TLS, HTTP, Wi-Fi) -> roll back while we still can
// Wait / Confirm / Rollback. A device that cannot reach the update server must not keep an image
// that might never update again; one that is simply offline (no DNS) is judged on the rest.
Verdict probation(const Probe &p);

// ------------------------------------------------------------------ crash streak
constexpr int kStableS        = 180;   // a normal boot that lives this long clears the streak
constexpr int kSafeModeAt     = 3;     // consecutive abnormal resets -> safe mode
constexpr int kRescueAt       = 5;     // ... -> ask recovery for the safety copy (if there is one)

// Streak after a reset of kind `r` that ended the previous run; `prev` = the stored streak. Only
// crashes and brownouts count; a deliberate restart neither counts nor forgets; a cold start forgets
// everything (power-cycling is also how a user gets out of safe mode).
int next_streak(int prev, Reset r);

enum class Mode : uint8_t { Normal, Safe, Rescue };
// `has_rescue_source`: a safety copy of a DIFFERENT version than the running one exists.
Mode boot_mode(int streak, bool has_rescue_source);

// ------------------------------------------------------------------ aborted image (recovery)
constexpr int kMaxRetries = 2;   // fresh image retried this many times after not-its-fault resets
enum class AbortAction : uint8_t { Retry, Restore };
AbortAction on_aborted(Reset r, int retries_done);

// ------------------------------------------------------------------ staged rollout
// 0..99, stable per (device MAC, release version): a release published at rollout N installs
// itself on devices whose bucket < N. Nothing identifying leaves the device.
int rollout_bucket(const uint8_t mac[6], const char *version);
bool in_rollout(int bucket, int rollout_pct);   // rollout_pct outside 0..100 is clamped

// ------------------------------------------------------------------ versions
// "A.B.C" strictly newer than cur (missing parts count as 0).
bool version_newer(const char *cand, const char *cur);

// ------------------------------------------------------------------ CRC-32 (IEEE, zlib's)
uint32_t crc32(uint32_t crc, const void *data, size_t n);

// ------------------------------------------------------------------ journal record
// Two 4 KB sectors at the start of `assets`, written alternately; the valid one with the highest
// seq wins, so a power cut during a write leaves the previous record intact.
constexpr uint32_t kJournalMagic = 0x314A564E;   // "NVJ1"
struct Journal {
    uint32_t magic;
    uint32_t seq;
    // system -> recovery
    uint8_t  rescue;            // 1: restore the safety copy (honoured only while rescue_ver runs)
    uint8_t  install_flags;     // kInstall* bits for the armed update
    uint8_t  pad0[2];
    char     rescue_ver[32];
    char     rescue_why[48];
    // recovery bookkeeping: power-cut retries of a fresh image
    char     retry_ver[32];
    uint32_t retries;
    // recovery -> system: what happened before this boot (shown once, then cleared)
    uint8_t  res_set;
    uint8_t  res_ok;
    uint8_t  pad1[2];
    char     res_op[12];        // "install" | "rollback" | "restore" | "rescue" | "retry"
    char     res_from[32];
    char     res_to[32];
    char     res_why[48];
    // recovery -> system: a rescue for this version found nothing to restore; don't ask again
    char     no_rescue_ver[32];
    uint32_t crc;               // over everything above
};
static_assert(sizeof(Journal) == 4 + 4 + 4 + 32 + 48 + 32 + 4 + 4 + 12 + 32 + 32 + 48 + 32 + 4, "Journal layout");

constexpr uint8_t kInstallNeedSafetyCopy = 0x01;   // hands-free update: no safety copy -> don't install

void journal_seal(Journal *j);                                   // magic + crc
bool journal_valid(const Journal &j);                            // magic, crc, NUL-terminated strings
// The newer of two sectors' records (nullptr when neither is valid).
const Journal *journal_pick(const Journal *a, const Journal *b);
// Bounded copy into a fixed field, always NUL-terminated.
void set_field(char *dst, size_t cap, const char *src);

// ------------------------------------------------------------------ last-known-good header
// One 4 KB sector after the journal; the compressed image follows at kLkgDataOffset. The header is
// written last, so a power cut while saving leaves "no copy", never a half copy that looks whole.
constexpr uint32_t kLkgMagic       = 0x4B4C564E;   // "NVLK"
constexpr uint32_t kLkgFormat      = 1;
constexpr uint32_t kSector         = 4096;
constexpr uint32_t kJournalOffset0 = 0;
constexpr uint32_t kJournalOffset1 = kSector;
constexpr uint32_t kLkgHeaderOffset = 2 * kSector;
constexpr uint32_t kLkgDataOffset  = 3 * kSector;
enum : uint32_t { kLkgRaw = 0, kLkgDeflate = 1 };
struct LkgHeader {
    uint32_t magic;
    uint32_t format;
    char     version[32];
    uint32_t raw_size;
    uint8_t  raw_sha256[32];
    uint32_t stored_size;
    uint8_t  stored_sha256[32];
    uint32_t method;
    uint32_t saved_by;          // recovery version that wrote it (major*10000+minor*100+patch)
    uint32_t crc;
};
static_assert(sizeof(LkgHeader) == 4 + 4 + 32 + 4 + 32 + 4 + 32 + 4 + 4 + 4, "LkgHeader layout");
void lkg_seal(LkgHeader *h);
// Valid header whose stored bytes fit in a partition of `part_size`.
bool lkg_valid(const LkgHeader &h, uint32_t part_size);

}  // namespace nv_fwup_policy
