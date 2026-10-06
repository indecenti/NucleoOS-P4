// libFuzzer target for system content packs: whatever a hostile host serves as a v2 pack.sig, a tree
// index or a tree archive, what gets accepted stays inside its folder and adds up.
#include "nv_store_pkg.h"
#include "nv_store_tree.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>

using namespace nv_store_pkg;
namespace tr = nv_store_tree;

static bool unsafe(const char *p) {
    if (!p[0] || p[0] == '/') return true;
    for (const char *s = p; *s; s++) {
        if (*s == ' ' || *s == '\\') return true;
        if ((s == p || s[-1] == '/') && s[0] == '.' && (s[1] == '/' || s[1] == 0 || (s[1] == '.' && (s[2] == '/' || s[2] == 0))))
            return true;
    }
    return false;
}

static void check_pack(const uint8_t *d, size_t n) {
    static std::unique_ptr<DataPack> p(new DataPack);
    if (!parse_data(reinterpret_cast<const char *>(d), n, p.get())) return;
    if (p->n < 1 || p->n > kDataMax || p->signed_len >= n || (p->format != 1 && p->format != 2)) abort();
    bool dest = false;
    for (const char *const *l = p->format == 2 ? kContentDests : kDataDests; *l; l++) dest |= !strcmp(p->dest, *l);
    if (!dest) abort();
    for (int i = 0; i < p->n; i++) {
        const DataPart &q = p->parts[i];
        if (!memchr(q.name, '\0', sizeof q.name) || !memchr(q.url, '\0', sizeof q.url) || !memchr(q.url2, '\0', sizeof q.url2)) abort();
        if (strncmp(q.url, "https://", 8) || (q.url2[0] && strncmp(q.url2, "https://", 8)) || q.size == 0) abort();
        if (p->format == 1 && (q.kind != 'f' || q.url2[0] || strchr(q.name, '/'))) abort();
        if (strcmp(q.name, ".") && unsafe(q.name)) abort();
        if (!strcmp(q.name, ".") && q.kind == 'f') abort();
        if (q.kind == 'i' && (i + 1 >= p->n || strcmp(p->parts[i + 1].name, q.name) || q.size > kTreeIndexMax)) abort();
        if ((q.kind == 't' || q.kind == 'u') && (i == 0 || p->parts[i - 1].kind != 'i')) abort();
    }
}

static void check_index(const uint8_t *d, size_t n) {
    static std::unique_ptr<tr::Index> ix(new tr::Index);
    if (!tr::parse_index(reinterpret_cast<const char *>(d), n, ix.get())) return;
    if (ix->n < 1 || ix->n > tr::kIndexMax) abort();
    tr::Entry e, prev;
    for (int i = 0; i < ix->n; i++) {
        if (!tr::index_entry(*ix, i, &e) || unsafe(e.path) || e.size > tr::kFileMax) abort();
        if (i && strcmp(prev.path, e.path) >= 0) abort();
        if (tr::index_find(*ix, e.path) != i) abort();
        prev = e;
    }
}

struct Check : tr::Sink {
    uint64_t want = 0, got = 0;
    bool open = false;
    bool dir(const char *p) override { if (open || unsafe(p)) abort(); return true; }
    bool begin(const char *p, uint64_t size) override {
        if (open || unsafe(p) || strlen(p) >= (size_t)tr::kPathMax) abort();
        open = true; want = size; got = 0; return true;
    }
    bool data(const uint8_t *, size_t k) override { if (!open || got + k > want) abort(); got += k; return true; }
    bool end() override { if (!open || got != want) abort(); open = false; return true; }
};

static void check_tar(const uint8_t *d, size_t n) {
    tr::TarReader r;
    Check c;
    size_t i = 0, step = 1;
    while (i < n) {                                   // uneven chunks: headers split across feeds
        const size_t k = n - i < step ? n - i : step;
        if (!r.feed(d + i, k, c)) return;
        i += k;
        step = step * 3 % 1031 + 1;
    }
    if (r.finished() && c.open) abort();
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    check_pack(d, n);
    check_index(d, n);
    check_tar(d, n);
    // the swap table: total over its 16 states, never two renames onto one name
    if (n) {
        const unsigned a = tr::swap_recover(d[0] & 1, d[0] & 2, d[0] & 4, d[0] & 8);
        if ((a & tr::SWAP_OLD_TO_DIR) && (a & (tr::SWAP_NEW_TO_DIR | tr::SWAP_RM_OLD))) abort();
    }
    return 0;
}
