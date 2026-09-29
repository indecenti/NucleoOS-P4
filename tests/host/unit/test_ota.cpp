// Unit tests for nv_ota_manifest: field validation and the exact signed message (must match
// tools/ota_sign.py message(); a drift makes every signed update fail on the device).
#include "check.h"
#include "nv_ota_manifest.h"

#include <cmath>
#include <cstring>
#include <string>

using namespace nv_ota_manifest;

static const char *kSha = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
static const std::string kSig(142, 'a');   // 71 bytes, a typical DER ECDSA P-256 length

static bool ok(const char *ver, const char *sha, double size, const char *sig, Signed *s = nullptr) {
    Signed tmp;
    return parse(ver, sha, size, sig, 4718592, s ? s : &tmp);
}

int main() {
    Signed s;
    CHECK(ok("1.1.123", kSha, 3786976, kSig.c_str(), &s));
    CHECK(s.size == 3786976 && s.sig_len == 71 && s.sha256[0] == 0x00 && s.sha256[31] == 0xff);
    char msg[160];
    const size_t n = message(s, msg, sizeof msg);
    CHECK(n == strlen(msg));
    CHECK(std::string(msg) == std::string("nucleoos-ota-v1\n1.1.123\n") + kSha + "\n3786976\n");
    CHECK(message(s, msg, 20) == 0);   // too small: refused, not truncated

    // Version: charset and length; no separators can be smuggled into the signed message.
    CHECK(ok("1.2.0-rc1+build_7", kSha, 1, kSig.c_str()));
    CHECK(!ok("1.2.0\n9.9.9", kSha, 1, kSig.c_str()));
    CHECK(!ok("1.2 0", kSha, 1, kSig.c_str()));
    CHECK(!ok("", kSha, 1, kSig.c_str()));
    CHECK(!ok(std::string(32, '1').c_str(), kSha, 1, kSig.c_str()));
    CHECK(ok(std::string(31, '1').c_str(), kSha, 1, kSig.c_str()));

    // sha256: exactly 64 lowercase hex digits.
    CHECK(!ok("1", "00112233445566778899AABBCCDDEEFF00112233445566778899aabbccddeeff", 1, kSig.c_str()));
    CHECK(!ok("1", std::string(63, 'a').c_str(), 1, kSig.c_str()));
    CHECK(!ok("1", std::string(65, 'a').c_str(), 1, kSig.c_str()));
    CHECK(!ok("1", std::string(64, 'g').c_str(), 1, kSig.c_str()));

    // size: integer in 1..max.
    CHECK(!ok("1", kSha, 0, kSig.c_str()));
    CHECK(!ok("1", kSha, -5, kSig.c_str()));
    CHECK(!ok("1", kSha, 1.5, kSig.c_str()));
    CHECK(!ok("1", kSha, NAN, kSig.c_str()));
    CHECK(!ok("1", kSha, INFINITY, kSig.c_str()));
    CHECK(!ok("1", kSha, 4718593, kSig.c_str()));
    CHECK(ok("1", kSha, 4718592, kSig.c_str()));
    CHECK(!ok("1", kSha, 1e300, kSig.c_str()));

    // signature: even-length lowercase hex, 8..kSigMax bytes.
    CHECK(!ok("1", kSha, 1, "abc"));
    CHECK(!ok("1", kSha, 1, std::string(14, 'a').c_str()));
    CHECK(ok("1", kSha, 1, std::string(16, 'a').c_str()));
    CHECK(ok("1", kSha, 1, std::string(kSigMax * 2, 'a').c_str()));
    CHECK(!ok("1", kSha, 1, std::string(kSigMax * 2 + 2, 'a').c_str()));
    CHECK(!ok("1", kSha, 1, (std::string(140, 'a') + "zz").c_str()));

    CHECK(!ok(nullptr, kSha, 1, kSig.c_str()));
    CHECK(!ok("1", nullptr, 1, kSig.c_str()));
    CHECK(!ok("1", kSha, 1, nullptr));
    return TEST_DONE("ota");
}
