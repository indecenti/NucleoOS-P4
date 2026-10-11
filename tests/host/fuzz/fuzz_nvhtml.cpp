// libFuzzer target for nvhtml (ports/zim/nvhtml): article pages come from downloaded ZIM files, so
// any bytes must convert without out-of-bounds access, leaks or hangs, under every option, and the
// output must respect the length cap. Seeds: corpus/nvhtml.
#include "nvhtml.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    if (n < 1) return 0;
    const char *h = reinterpret_cast<const char *>(d + 1);
    const size_t hn = n - 1;
    nvhtml_opts_t o = {};
    o.intro = d[0] & 1;
    o.no_title = d[0] & 2;
    o.back_matter = d[0] & 4;
    o.section = (d[0] & 8) ? "storia" : nullptr;
    o.max = (d[0] & 16) ? (size_t)(d[0] >> 5) * 40 + 1 : 0;
    size_t len = 0;
    char *t = nvhtml_text(h, hn, &o, &len);
    if (!t) return 0;
    if (strlen(t) != len) abort();
    if (o.max && len > o.max + 6) abort();
    free(t);
    t = nvhtml_sections(h, hn, &len);
    if (t && strlen(t) != len) abort();
    free(t);
    t = nvhtml_latin1_to_utf8(h, hn, &len);
    if (t && len > 2 * hn) abort();
    free(t);
    return 0;
}
