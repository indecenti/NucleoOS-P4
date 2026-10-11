// libFuzzer target for nvzim (ports/zim/nvzim): a ZIM file comes from the Internet, so any bytes
// must give errors, never an out-of-bounds access, a leak, a hang or an unbounded allocation. The
// input is the whole file, read through the in-memory backend with small limits; every API runs on
// what it finds. Seeds: corpus/nvzim (tests/host/gen_zim.py).
#include "nvzim.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

struct Mem { const uint8_t *d; size_t n; };

static long mem_read(void *ctx, uint64_t off, void *buf, size_t n) {
    const Mem *m = static_cast<const Mem *>(ctx);
    if (off > m->n) return -1;
    const size_t k = n < m->n - off ? n : (size_t)(m->n - off);
    memcpy(buf, m->d + off, k);
    return (long)k;
}

static void touch(const nvzim_blob_t &b) {   // the blob must be readable to its last byte
    volatile uint8_t x = 0;
    if (b.size) x ^= b.data[0] ^ b.data[b.size - 1];
    (void)x;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    Mem m{d, n};
    nvzim_config_t c = {};
    c.read = mem_read;
    c.ctx = &m;
    c.size = n;
    c.cache_pages = 4;
    c.clusters = 2;
    c.cluster_max = 1u << 20;
    c.blob_max = 256u << 10;
    c.tree_depth = 4;
    nvzim_t *z = nullptr;
    if (nvzim_open(&z, "fuzz", &c) != NVZIM_OK) {
        if (z) abort();
        return 0;
    }
    const nvzim_info_t *info = nvzim_get_info(z);
    nvzim_entry_t e, f;
    nvzim_blob_t b;
    const uint32_t count = info->entry_count;
    const uint32_t step = count > 64 ? count / 64 : 1;
    for (uint32_t i = 0; i < count; i += step) {
        if (nvzim_entry_at(z, i, &e) != NVZIM_OK) continue;
        if (strlen(e.path) >= NVZIM_PATH_MAX || strlen(e.title) >= NVZIM_TITLE_MAX) abort();
        if (nvzim_find(z, e.ns, e.path, &f) == NVZIM_OK && (f.ns != e.ns || strcmp(f.path, e.path))) abort();
        (void)nvzim_mime_name(z, e.mime);
        if (nvzim_resolve(z, &e) != NVZIM_OK) continue;
        if (nvzim_read(z, &e, &b) == NVZIM_OK) {
            if (b.size > c.blob_max) abort();
            touch(b);
            nvzim_blob_release(z, &b);
        }
    }
    char buf[64];
    nvzim_metadata(z, "Title", buf, sizeof buf);
    if (strlen(buf) >= sizeof buf) abort();
    if (nvzim_main_page(z, &e) == NVZIM_OK && nvzim_read(z, &e, &b) == NVZIM_OK) nvzim_blob_release(z, &b);
    const int tc = nvzim_title_count(z);
    if (tc > 0) {
        uint32_t pos;
        if (nvzim_title_lower_bound(z, "M", &pos) == NVZIM_OK && pos > (uint32_t)tc) abort();
        for (int k = 0; k < 8; k++) nvzim_title_at(z, (uint32_t)((uint64_t)tc * k / 8), &e);
        nvzim_entry_t s[4];
        const int ns = nvzim_suggest(z, "a", s, 4);
        if (ns > 4) abort();
        nvzim_random(z, 7, &e);
    }
    nvzim_verify(z, nullptr, nullptr);
    nvzim_close(z);
    return 0;
}
