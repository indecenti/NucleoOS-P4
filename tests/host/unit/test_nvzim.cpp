// nvzim (ports/zim/nvzim): the ZIM reader of the Biblioteca app, the zim tool and ANIMA, over the
// synthetic files of corpus/nvzim (tests/host/gen_zim.py writes them and unit/nvzim_fixtures.h).
// Every entry is checked through every way of reaching it, with the default configuration and with
// tiny caches; then split files, an in-memory backend, the old format and damaged copies.
// The real-world check is ports/zim/tools/diff_libzim.py (against libzim, on Kiwix files).
#include "nvzim.h"
#include "nvzim_md5.h"

#include "check.h"
#include "nvzim_fixtures.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static std::vector<uint8_t> load(const std::string &path) {
    std::vector<uint8_t> d;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return d;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + n);
    fclose(f);
    return d;
}

static void save(const std::string &path, const uint8_t *d, size_t n) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return;
    fwrite(d, 1, n, f);
    fclose(f);
}

static std::string md5hex(const uint8_t *d, size_t n) {
    nvzim_md5_t m;
    uint8_t o[16];
    nvzim_md5_init(&m);
    nvzim_md5_update(&m, d, n);
    nvzim_md5_final(&m, o);
    char s[33];
    for (int i = 0; i < 16; i++) snprintf(s + 2 * i, 3, "%02x", o[i]);
    return s;
}

static std::string tmpdir() {
    const char *t = getenv("TMPDIR");
    return std::string(t && *t ? t : "/tmp");
}

// Every fixture entry of `file`: by index, by path, its redirect target, its content.
static void check_entries(nvzim_t *z, const char *file) {
    int n = 0;
    for (const NvzimFx &fx : kNvzimFx) {
        if (strcmp(fx.file, file)) continue;
        n++;
        nvzim_entry_t e, f;
        CHECK(nvzim_entry_at(z, fx.index, &e) == NVZIM_OK);
        CHECK(e.index == fx.index && e.ns == fx.ns && !strcmp(e.path, fx.path) && !strcmp(e.title, fx.title));
        CHECK(nvzim_find(z, fx.ns, fx.path, &f) == NVZIM_OK && f.index == fx.index);
        CHECK(nvzim_is_redirect(&e) == fx.redirect);
        if (fx.redirect) {
            nvzim_entry_t t;
            CHECK(nvzim_entry_at(z, e.target, &t) == NVZIM_OK);
            CHECK(std::string(1, t.ns) + "/" + t.path == fx.target);
            nvzim_blob_t b;
            CHECK(nvzim_read(z, &e, &b) == NVZIM_ENOTFOUND);   // redirects have no content
            continue;
        }
        nvzim_blob_t b;
        const int r = nvzim_read(z, &e, &b);
        CHECK(r == NVZIM_OK);
        if (r) { fprintf(stderr, "  read %c/%s: %s\n", e.ns, e.path, nvzim_strerror(r)); continue; }
        CHECK(b.size == fx.size);
        CHECK(md5hex(b.data, (size_t)b.size) == fx.md5);
        nvzim_blob_release(z, &b);
        CHECK(b.data == nullptr && b.owned_ == nullptr);
    }
    CHECK(n > 0);
}

static void check_misses(nvzim_t *z) {
    nvzim_entry_t e;
    CHECK(nvzim_find(z, 'C', "", &e) == NVZIM_ENOTFOUND);
    CHECK(nvzim_find(z, 'C', "Article_0005", &e) == NVZIM_ENOTFOUND);   // between two paths
    CHECK(nvzim_find(z, 'C', "zzz", &e) == NVZIM_ENOTFOUND);
    CHECK(nvzim_find(z, '\x01', "a", &e) == NVZIM_ENOTFOUND);          // before every namespace
    CHECK(nvzim_find(z, '\x7f', "a", &e) == NVZIM_ENOTFOUND);          // after
    CHECK(nvzim_find(z, 'C', nullptr, &e) == NVZIM_ENOTFOUND);
    std::string longp(NVZIM_PATH_MAX + 10, 'a');
    CHECK(nvzim_find(z, 'C', longp.c_str(), &e) == NVZIM_ENOTFOUND);
    CHECK(nvzim_entry_at(z, 0xFFFFFFFFu, &e) == NVZIM_ENOTFOUND);
}

static void check_new(nvzim_t *z) {
    const nvzim_info_t *i = nvzim_get_info(z);
    CHECK(i->major == 6 && i->minor == 3 && i->new_scheme && i->article_ns == 'C');
    CHECK(i->entry_count == kNvzimNewEntries && i->has_checksum);
    CHECK(!strcmp(nvzim_mime_name(z, 2), "text/html") && !strcmp(nvzim_mime_name(z, 999), ""));
    check_entries(z, "new.zim");
    check_misses(z);

    nvzim_entry_t e;
    CHECK(nvzim_find_article(z, "Article_001", &e) == NVZIM_OK && e.ns == 'C');
    CHECK(nvzim_find_article(z, "C/Article_001", &e) == NVZIM_OK && !strcmp(e.path, "Article_001"));
    CHECK(nvzim_find_article(z, "M/Title", &e) == NVZIM_OK && e.ns == 'M');
    CHECK(nvzim_find_article(z, "Q/nothing", &e) == NVZIM_ENOTFOUND);
    // redirects: a chain is followed, a loop is reported
    CHECK(nvzim_find(z, 'C', "R1", &e) == NVZIM_OK && nvzim_resolve(z, &e) == NVZIM_OK && !strcmp(e.path, "Article_001"));
    CHECK(nvzim_find(z, 'C', "LoopA", &e) == NVZIM_OK && nvzim_resolve(z, &e) == NVZIM_ELOOP);
    CHECK(nvzim_main_page(z, &e) == NVZIM_OK && !strcmp(e.path, "Main_Page"));
    // metadata
    char buf[64];
    CHECK(nvzim_metadata(z, "Title", buf, sizeof buf) == 15 && !strcmp(buf, "nvzim test file"));
    CHECK(nvzim_metadata(z, "Title", buf, 6) == 15 && !strcmp(buf, "nvzim"));
    CHECK(nvzim_metadata(z, "Nope", buf, sizeof buf) == NVZIM_ENOTFOUND && buf[0] == 0);
    CHECK(nvzim_metadata(z, "Illustration_48x48@1", buf, sizeof buf) == 17 && !memcmp(buf, "\x89PNG", 4));

    // the >2 MiB cluster, out of order: the last blob first (the buffer grows and decoding starts
    // over), then the first, then one in the middle; then a raw blob and back
    const char *order[] = {"Article_199", "Article_150", "Article_175", "img/pic_03.png", "Article_160", "wide/zst_3.txt",
                           "wide/raw_2.bin", "img/empty.bin", "Article_000", "Article_199"};
    for (const char *p : order) {
        CHECK(nvzim_find(z, 'C', p, &e) == NVZIM_OK);
        nvzim_blob_t b;
        CHECK(nvzim_read(z, &e, &b) == NVZIM_OK);
        bool ok = false;
        for (const NvzimFx &fx : kNvzimFx)
            if (!strcmp(fx.file, "new.zim") && !strcmp(fx.path, p)) ok = b.size == fx.size && md5hex(b.data, (size_t)b.size) == fx.md5;
        CHECK(ok);
        nvzim_blob_release(z, &b);
    }

    // titles: the v1 listing, in byte order
    const int tc = nvzim_title_count(z);
    CHECK(tc > 200);
    std::string prev;
    for (int p = 0; p < tc; p++) {
        CHECK(nvzim_title_at(z, (uint32_t)p, &e) == NVZIM_OK);
        CHECK(e.ns == 'C' && std::string(e.title) >= prev);
        prev = e.title;
    }
    CHECK(nvzim_title_at(z, (uint32_t)tc, &e) == NVZIM_ENOTFOUND);
    uint32_t pos;
    CHECK(nvzim_title_lower_bound(z, "Article 1", &pos) == NVZIM_OK);
    CHECK(nvzim_title_at(z, pos, &e) == NVZIM_OK && !strcmp(e.title, "Article 100"));
    CHECK(nvzim_title_lower_bound(z, "", &pos) == NVZIM_OK && pos == 0);
    CHECK(nvzim_title_lower_bound(z, "\xff", &pos) == NVZIM_OK && pos == (uint32_t)tc);

    // suggestions: case variants, accents, duplicates dropped, exact match first
    nvzim_entry_t s[10];
    int n = nvzim_suggest(z, "citt\xc3\xa0", s, 10);
    CHECK(n == 2 && !strcmp(s[0].title, "citt\xc3\xa0 minuscola") && !strcmp(s[1].title, "Citt\xc3\xa0 di prova"));
    n = nvzim_suggest(z, "\xc3\xa9lan", s, 10);   // é -> É
    CHECK(n == 1 && !strcmp(s[0].title, "\xc3\x89lan"));
    n = nvzim_suggest(z, "diabete", s, 10);       // three titles, one article
    CHECK(n == 1);
    n = nvzim_suggest(z, "diabete mellito", s, 10);
    CHECK(n == 1 && !strcmp(s[0].title, "Diabete mellito"));
    n = nvzim_suggest(z, "article 01", s, 3);
    CHECK(n == 3 && !strcmp(s[0].title, "Article 010"));
    n = nvzim_suggest(z, "IPHONE", s, 10);
    CHECK(n == 0);   // no variant reaches "iPhone": documented limit of the byte-ordered listing
    n = nvzim_suggest(z, "iphone", s, 10);
    CHECK(n == 0);
    n = nvzim_suggest(z, "iP", s, 10);
    CHECK(n == 1 && !strcmp(s[0].title, "iPhone"));
    CHECK(nvzim_suggest(z, "", s, 10) == 0 && nvzim_suggest(z, "x", s, 0) == 0);

    // a sequential scan sees every entry of a namespace, in path order, and stops when asked
    struct Scan { int n; std::string prev; bool ordered; int stop; };
    auto cb = [](void *ctx, const nvzim_entry_t *x) {
        Scan *sc = static_cast<Scan *>(ctx);
        if (std::string(x->path) <= sc->prev && sc->n) sc->ordered = false;
        sc->prev = x->path;
        return ++sc->n == sc->stop ? 1 : 0;
    };
    int c_entries = 0, m_entries = 0;
    for (const NvzimFx &fx : kNvzimFx)
        if (!strcmp(fx.file, "new.zim")) { c_entries += fx.ns == 'C'; m_entries += fx.ns == 'M'; }
    Scan sc = {0, "", true, -1};
    CHECK(nvzim_scan(z, 'C', cb, &sc) == c_entries && sc.n == c_entries && sc.ordered);
    sc = {0, "", true, -1};
    CHECK(nvzim_scan(z, 'M', cb, &sc) == m_entries && sc.ordered);
    sc = {0, "", true, 7};
    CHECK(nvzim_scan(z, 'C', cb, &sc) == 7 && sc.n == 7);
    CHECK(nvzim_scan(z, 'Q', nullptr, nullptr) == 0);

    // random articles are content entries
    for (uint32_t seed = 0; seed < 50; seed++) {
        CHECK(nvzim_random(z, seed, &e) == NVZIM_OK);
        CHECK(!nvzim_is_redirect(&e) && e.ns == 'C');
    }
    CHECK(nvzim_verify(z, nullptr, nullptr) == NVZIM_OK);
}

static void check_old(nvzim_t *z) {
    const nvzim_info_t *i = nvzim_get_info(z);
    CHECK(i->major == 5 && !i->new_scheme && i->article_ns == 'A' && i->entry_count == kNvzimOldEntries);
    check_entries(z, "old.zim");
    nvzim_entry_t e;
    CHECK(nvzim_find_article(z, "Page_07.html", &e) == NVZIM_OK && e.ns == 'A');
    CHECK(nvzim_main_page(z, &e) == NVZIM_OK && !strcmp(e.path, "Accueil.html"));
    CHECK(nvzim_find(z, '-', "favicon", &e) == NVZIM_OK && nvzim_resolve(z, &e) == NVZIM_OK && e.ns == 'I');
    // the header's title index, restricted to the article namespace
    const int tc = nvzim_title_count(z);
    CHECK(tc == 41);
    std::string prev;
    for (int p = 0; p < tc; p++) {
        CHECK(nvzim_title_at(z, (uint32_t)p, &e) == NVZIM_OK && e.ns == 'A' && std::string(e.title) >= prev);
        prev = e.title;
    }
    nvzim_entry_t s[5];
    CHECK(nvzim_suggest(z, "page 3", s, 5) == 5 && !strcmp(s[0].title, "Page 30"));
    CHECK(nvzim_verify(z, nullptr, nullptr) == NVZIM_OK);
}

static nvzim_t *open_file(const std::string &path, const nvzim_config_t *cfg = nullptr) {
    nvzim_t *z = nullptr;
    const int r = nvzim_open(&z, path.c_str(), cfg);
    CHECK(r == NVZIM_OK && z);
    if (r) fprintf(stderr, "  open %s: %s\n", path.c_str(), nvzim_strerror(r));
    return r ? nullptr : z;
}

struct Mem { const std::vector<uint8_t> *d; };
static long mem_read(void *ctx, uint64_t off, void *buf, size_t n) {
    const std::vector<uint8_t> &d = *static_cast<Mem *>(ctx)->d;
    if (off > d.size()) return -1;
    const size_t k = n < d.size() - off ? n : (size_t)(d.size() - off);
    memcpy(buf, d.data() + off, k);
    return (long)k;
}

int main() {
    const std::string dir = "corpus/nvzim/";
    const std::vector<uint8_t> newz = load(dir + "new.zim"), oldz = load(dir + "old.zim");
    CHECK(!newz.empty() && !oldz.empty());

    // MD5 itself (RFC 1321 test vectors)
    CHECK(md5hex((const uint8_t *)"", 0) == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(md5hex((const uint8_t *)"abc", 3) == "900150983cd24fb0d6963f7d28e17f72");
    const char *alpha = "12345678901234567890123456789012345678901234567890123456789012345678901234567890";
    CHECK(md5hex((const uint8_t *)alpha, 80) == "57edf4a22be3c955ac49da2e2107b67a");

    // defaults, then the tightest configuration (every cache and memo at its minimum)
    if (nvzim_t *z = open_file(dir + "new.zim")) { check_new(z); nvzim_close(z); }
    nvzim_config_t tight = {};
    tight.cache_pages = 1;
    tight.clusters = 1;
    tight.tree_depth = 1;
    if (nvzim_t *z = open_file(dir + "new.zim", &tight)) { check_new(z); nvzim_close(z); }
    nvzim_config_t deep = {};
    deep.tree_depth = 20;
    deep.clusters = 4;
    if (nvzim_t *z = open_file(dir + "new.zim", &deep)) {
        check_new(z);
        check_new(z);   // the second round runs on memoised searches
        CHECK(nvzim_get_stats(z)->tree_hits > 0);
        nvzim_close(z);
    }
    // a cluster limit below the big cluster: that article is refused, the others still read
    nvzim_config_t small = {};
    small.cluster_max = 1u << 20;
    if (nvzim_t *z = open_file(dir + "new.zim", &small)) {
        nvzim_entry_t e;
        nvzim_blob_t b;
        CHECK(nvzim_find(z, 'C', "Article_199", &e) == NVZIM_OK);
        const int r = nvzim_read(z, &e, &b);
        CHECK(r == NVZIM_EUNSUPPORTED);
        if (r != NVZIM_EUNSUPPORTED) fprintf(stderr, "  big cluster over the limit: %s\n", nvzim_strerror(r));
        CHECK(nvzim_find(z, 'C', "Article_001", &e) == NVZIM_OK && nvzim_read(z, &e, &b) == NVZIM_OK);
        nvzim_blob_release(z, &b);
        nvzim_close(z);
    }
    if (nvzim_t *z = open_file(dir + "old.zim")) { check_old(z); nvzim_close(z); }

    // split files: name.zimaa, name.zimab, ... at awkward boundaries, opened by either name
    const std::string base = tmpdir() + "/nvzim_split.zim";
    const size_t cuts[] = {0, 81, 5000, 17777, newz.size()};
    for (int k = 0; k < 4; k++) {
        const std::string part = base + "a" + char('a' + k);
        save(part, newz.data() + cuts[k], cuts[k + 1] - cuts[k]);
    }
    remove(base.c_str());
    if (nvzim_t *z = open_file(base)) {
        CHECK(nvzim_get_info(z)->parts == 4);
        check_new(z);
        nvzim_close(z);
    }
    if (nvzim_t *z = open_file(base + "aa")) { check_new(z); nvzim_close(z); }
    for (int k = 0; k < 4; k++) remove((base + "a" + char('a' + k)).c_str());

    // an in-memory backend (what the fuzzer uses)
    Mem mem{&newz};
    nvzim_config_t mc = {};
    mc.read = mem_read;
    mc.ctx = &mem;
    mc.size = newz.size();
    if (nvzim_t *z = open_file("memory", &mc)) { check_new(z); nvzim_close(z); }

    // missing and damaged files
    nvzim_t *z = nullptr;
    CHECK(nvzim_open(&z, "/nonexistent/x.zim", nullptr) == NVZIM_ENOTFOUND && !z);
    std::vector<uint8_t> bad = newz;
    bad[0] ^= 1;
    Mem bm{&bad};
    mc.ctx = &bm;
    mc.size = bad.size();
    CHECK(nvzim_open(&z, "bad", &mc) == NVZIM_EFORMAT && !z);
    bad = newz;
    bad[4] = 7;   // major version 7
    CHECK(nvzim_open(&z, "bad", &mc) == NVZIM_EUNSUPPORTED && !z);
    bad = newz;
    bad[bad.size() / 2] ^= 0x55;   // one byte in the middle: open works, the checksum does not
    if (nvzim_open(&z, "bad", &mc) == NVZIM_OK) {
        CHECK(nvzim_verify(z, nullptr, nullptr) == NVZIM_EMISMATCH);
        nvzim_close(z);
    }
    auto cancel = [](void *, uint64_t, uint64_t) { return 1; };
    mc.ctx = &mem;
    mc.size = newz.size();
    if (nvzim_open(&z, "memory", &mc) == NVZIM_OK) {
        CHECK(nvzim_verify(z, cancel, nullptr) == NVZIM_ECANCELED);
        nvzim_close(z);
    }
    // every truncation of the file, and scattered byte flips: errors, never a crash (ASan/UBSan)
    for (size_t cut = 0; cut < newz.size(); cut += cut < 200 ? 1 : 997) {
        bad.assign(newz.begin(), newz.begin() + (long)cut);
        mc.ctx = &bm;
        mc.size = bad.size();
        if (nvzim_open(&z, "cut", &mc) == NVZIM_OK) {
            nvzim_entry_t e;
            nvzim_blob_t b;
            for (uint32_t i = 0; i < nvzim_get_info(z)->entry_count; i += 7)
                if (nvzim_entry_at(z, i, &e) == NVZIM_OK && nvzim_read(z, &e, &b) == NVZIM_OK) nvzim_blob_release(z, &b);
            nvzim_close(z);
        }
    }
    uint32_t rng = 1;
    for (int round = 0; round < 300; round++) {
        bad = newz;
        for (int k = 0; k < 4; k++) {
            rng = rng * 1103515245u + 12345u;
            bad[(rng >> 8) % bad.size()] ^= (uint8_t)(rng >> 24 | 1);
        }
        mc.ctx = &bm;
        mc.size = bad.size();
        if (nvzim_open(&z, "flip", &mc) != NVZIM_OK) continue;
        nvzim_entry_t e, s[4];
        nvzim_blob_t b;
        for (uint32_t i = 0; i < nvzim_get_info(z)->entry_count; i += 3) {
            if (nvzim_entry_at(z, i, &e)) continue;
            nvzim_find(z, e.ns, e.path, &e);
            if (nvzim_resolve(z, &e) == NVZIM_OK && nvzim_read(z, &e, &b) == NVZIM_OK) nvzim_blob_release(z, &b);
        }
        nvzim_suggest(z, "art", s, 4);
        nvzim_random(z, (uint32_t)round, &e);
        nvzim_main_page(z, &e);
        nvzim_close(z);
    }
    return TEST_DONE("nvzim");
}
