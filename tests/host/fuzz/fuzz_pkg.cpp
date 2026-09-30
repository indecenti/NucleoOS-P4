// libFuzzer target for nv_store_pkg::parse: whatever a hostile store serves as package.sig, an
// accepted package has safe, sorted, NUL-terminated paths and a signed span inside the input.
#include "nv_store_pkg.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>

using namespace nv_store_pkg;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    static std::unique_ptr<Package> p(new Package);
    if (!parse(reinterpret_cast<const char *>(d), n, p.get())) return 0;
    if (p->n_files < 1 || p->n_files > kMaxFiles || p->signed_len >= n) abort();
    if (p->sig_len < 8 || p->sig_len > kSigMax) abort();
    for (int i = 0; i < p->n_files; i++) {
        const File &f = p->files[i];
        if (!memchr(f.path, '\0', sizeof f.path) || !path_ok(f.path) || f.size > kFileMax) abort();
        if (strstr(f.path, "..") && !strstr(f.path, "...")) {
            // ".." is only legal inside a longer segment name ("a..b"), never as a whole segment
            for (const char *s = f.path; (s = strstr(s, "..")); s++)
                if ((s == f.path || s[-1] == '/') && (s[2] == '\0' || s[2] == '/')) abort();
        }
        if (i && strcmp(p->files[i - 1].path, f.path) >= 0) abort();
        if (find(*p, f.path) != &f) abort();
    }
    return 0;
}
