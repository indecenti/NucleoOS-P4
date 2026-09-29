// libFuzzer target for nv_ota_manifest::parse/message: whatever a hostile manifest server sends,
// an accepted field set must decode consistently and yield a message with exactly its 4 separators.
#include "nv_ota_manifest.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace nv_ota_manifest;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    if (n < 8) return 0;
    double size;
    memcpy(&size, d, sizeof size);
    std::string rest(reinterpret_cast<const char *>(d + 8), n - 8);
    // three NUL-separated strings: version, sha256, sig
    std::string f[3];
    size_t pos = 0;
    for (int i = 0; i < 3; i++) {
        const size_t e = rest.find('\0', pos);
        f[i] = rest.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        if (e == std::string::npos) break;
        pos = e + 1;
    }
    Signed s;
    if (!parse(f[0].c_str(), f[1].c_str(), size, f[2].c_str(), 4718592, &s)) return 0;
    if (s.size < 1 || s.size > 4718592 || s.sig_len < 8 || s.sig_len > kSigMax) abort();
    if (strcmp(s.version, f[0].c_str()) || strcmp(s.sha256_hex, f[1].c_str())) abort();
    char msg[160];
    const size_t len = message(s, msg, sizeof msg);
    if (!len || len != strlen(msg)) abort();
    int nl = 0;
    for (size_t i = 0; i < len; i++) nl += msg[i] == '\n';
    if (nl != 4) abort();
    return 0;
}
