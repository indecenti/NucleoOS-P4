// libFuzzer target for nv_dl_core: a .meta4 from any server and a job record from the card never read
// out of bounds, and what they accept keeps the invariants the downloader relies on: an https mirror
// list, piece hashes that cover the size exactly, a record that formats and reads back the same, an
// app path that stays under its root.
#include "nv_dl_core.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    if (n < 1) return 0;
    const char *x = reinterpret_cast<const char *>(d + 1);
    const size_t xn = n - 1;
    switch (d[0] % 3) {
    case 0: {
        nvdl_meta4_t m;
        const int r = nvdl_meta4_parse(x, xn, &m, nullptr, 0);
        if (r == NVDL_OK) {
            if (!m.size || m.size > NVDL_SIZE_MAX || m.n_urls < 1 || m.n_urls > NVDL_URLS_MAX) abort();
            for (int i = 0; i < m.n_urls; i++)
                if (strncasecmp(m.urls[i], "https://", 8) || strlen(m.urls[i]) >= NVDL_URL_MAX || strpbrk(m.urls[i], " \r\n\t")) abort();
            if (m.piece_len && m.n_pieces != (m.size + m.piece_len - 1) / m.piece_len) abort();
            if (m.n_pieces < 100000) {                     // second pass fills exactly that many hashes
                std::vector<uint8_t> pc((size_t)m.n_pieces * 20 + 1);
                nvdl_meta4_t m2;
                if (nvdl_meta4_parse(x, xn, &m2, pc.data(), pc.size() - 1) != NVDL_OK || m2.n_pieces != m.n_pieces) abort();
                if (m.n_pieces && nvdl_meta4_parse(x, xn, &m2, pc.data(), pc.size() - 21) != NVDL_ETOOBIG) abort();
            }
        }
        break;
    }
    case 1: {
        nvdl_job_t j;
        if (nvdl_job_parse(x, xn, &j)) {
            char buf[8192];
            const int k = nvdl_job_format(&j, buf, sizeof buf);
            nvdl_job_t b;
            if (k <= 0 || !nvdl_job_parse(buf, (size_t)k, &b)) abort();
            if (b.id != j.id || b.size != j.size || b.verified != j.verified || b.state != j.state ||
                b.n_urls != j.n_urls || strcmp(b.dest, j.dest) || strcmp(b.owner, j.owner)) abort();
            if (nvdl_resume_offset(&j) > j.size) abort();
        }
        break;
    }
    default: {
        const std::string g(x, strnlen(x, xn));
        char out[NVDL_PATH_MAX];
        if (nvdl_guest_path("/sdcard/home", g.c_str(), out, sizeof out)) {
            const std::string o(out);
            if (o.rfind("/sdcard/home/", 0) != 0 || o.find("/../") != std::string::npos || o.find("//") != std::string::npos ||
                o.find('\\') != std::string::npos || o.back() == '/' || o.back() == '.' ||
                (o.size() >= 3 && o.compare(o.size() - 3, 3, "/..") == 0))
                abort();
        }
        char pn[64];
        const uint32_t np = (uint32_t)(xn % 700), i = (uint32_t)(xn % 31);
        if (nvdl_part_name("/s/w.zim", i, np, pn, sizeof pn) && (i >= np || strncmp(pn, "/s/w.zim", 8))) abort();
        break;
    }
    }
    return 0;
}
