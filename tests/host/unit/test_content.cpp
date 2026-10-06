// Unit tests for system content packs: the "nucleoos-data-v2" pack.sig (nv_store_pkg), the tree index,
// the ustar reader and the swap-recovery table (nv_store_tree). tools/content writes what these parse:
// a drift makes every content install fail, so the canonical shapes are pinned here.
#include "check.h"
#include "nv_store_pkg.h"
#include "nv_store_tree.h"
#include "nv_content_plan.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace nv_store_pkg;
namespace tr = nv_store_tree;

static const std::string H1(64, 'a'), H2(64, 'b');
static const std::string kSig = "sig " + std::string(140, 'c') + "\n";
static const std::string U = " https://github.com/indecenti/nucleoos-p4-store/releases/download/content-2026.10/";

static std::string v2(const std::string &lines, const std::string &dest = "data/anima") {
    return "nucleoos-data-v2\nanima-core-it\n2026.10.1\n" + dest + "\n" + lines + kSig;
}

static bool dok(const std::string &t, DataPack *d = nullptr) {
    static std::unique_ptr<DataPack> tmp(new DataPack);
    return parse_data(t.data(), t.size(), d ? d : tmp.get());
}

// ---- a minimal ustar writer, byte-compatible with Python's tarfile.USTAR_FORMAT ----
static void put_octal(char *f, size_t n, uint64_t v) {
    std::snprintf(f, n, "%0*llo", (int)(n - 1), (unsigned long long)v);
}
static std::string tar_header(const std::string &path, uint64_t size, char type, const char *magic = "ustar\0" "00",
                              bool break_sum = false) {
    char h[512] = {};
    std::string name = path, prefix;
    if (name.size() > 100) {                       // split at a '/' like tarfile does
        const size_t cut = name.rfind('/', name.size() - 1 - 0);
        prefix = name.substr(0, cut);
        name = name.substr(cut + 1);
    }
    memcpy(h, name.data(), name.size());
    put_octal(h + 100, 8, type == '5' ? 0755 : 0644);
    put_octal(h + 108, 8, 0);
    put_octal(h + 116, 8, 0);
    put_octal(h + 124, 12, size);
    put_octal(h + 136, 12, 0);
    h[156] = type;
    memcpy(h + 257, magic, 8);
    memcpy(h + 345, prefix.data(), prefix.size());
    memset(h + 148, ' ', 8);
    unsigned sum = 0;
    for (int i = 0; i < 512; i++) sum += (unsigned char)h[i];
    if (break_sum) sum++;
    std::snprintf(h + 148, 8, "%06o", sum);
    h[155] = ' ';
    return std::string(h, 512);
}
static std::string tar_file(const std::string &path, const std::string &body) {
    std::string s = tar_header(path, body.size(), '0') + body;
    s.append((512 - body.size() % 512) % 512, '\0');
    return s;
}
static std::string tar_end() { return std::string(1024 + 512 * 18, '\0'); }   // 2 zero blocks + record pad

struct Collect : tr::Sink {
    std::map<std::string, std::string> files;
    std::vector<std::string> dirs;
    std::string cur;
    uint64_t want = 0;
    bool refuse = false;
    bool dir(const char *p) override { dirs.push_back(p); return true; }
    bool begin(const char *p, uint64_t size) override { cur = p; want = size; files[cur] = ""; return !refuse; }
    bool data(const uint8_t *p, size_t n) override { files[cur].append((const char *)p, n); return true; }
    bool end() override { return files[cur].size() == want; }
};

static bool untar(const std::string &a, Collect *c, size_t chunk = 7) {
    tr::TarReader r;
    for (size_t i = 0; i < a.size(); i += chunk) {
        const size_t k = a.size() - i < chunk ? a.size() - i : chunk;
        if (!r.feed((const uint8_t *)a.data() + i, k, *c)) return false;
    }
    return r.finished();
}

int main() {
    std::unique_ptr<DataPack> d(new DataPack);

    // ---- v2 pack.sig ----
    const std::string core = v2(H1 + " 3145764 f anima-it-encoder.bin" + U + "anima-it-encoder.bin\n" +
                                H2 + " 28160 f anima-it-akb5.bin" + U + "anima-it-akb5.bin\n" +
                                H1 + " 5120 i akb5" + U + "akb5.idx\n" +
                                H2 + " 87000000 t akb5" + U + "akb5.tar" + U + "akb5-mirror.tar\n" +
                                H1 + " 900 i skills" + U + "skills.idx\n" +
                                H2 + " 40960 u skills" + U + "skills.tar\n" +
                                H1 + " 500000 f learned/facets.it.jsonl" + U + "facets.it.jsonl\n");
    CHECK(dok(core, d.get()));
    CHECK(d->format == 2 && !strcmp(d->dest, "data/anima") && d->n == 7);
    CHECK(d->parts[0].kind == 'f' && d->parts[2].kind == 'i' && d->parts[3].kind == 't' && d->parts[5].kind == 'u');
    CHECK(!strcmp(d->parts[3].name, "akb5") && !strncmp(d->parts[3].url2, "https://", 8) && d->parts[2].url2[0] == 0);
    CHECK(!strcmp(d->parts[6].name, "learned/facets.it.jsonl"));
    CHECK(d->signed_len == core.size() - kSig.size());

    // the web companion: one tree that is the dest itself
    const std::string web = v2(H1 + " 70000 i ." + U + "web.idx\n" + H2 + " 8000000 t ." + U + "web.tar\n", "web");
    CHECK(dok(web, d.get()) && !strcmp(d->dest, "web") && d->n == 2);

    // dests: v2 list only, relative to /sdcard
    CHECK(dok(v2(H1 + " 10 f a.exe" + U + "a\n", "nucleos/drivers")));
    CHECK(dok(v2(H1 + " 10 f it/index.bin" + U + "a\n", "data/tts")));
    CHECK(!dok(v2(H1 + " 10 f a" + U + "a\n", "anima")));                 // a v1 dest
    CHECK(!dok(v2(H1 + " 10 f a" + U + "a\n", "apps")));
    CHECK(!dok(v2(H1 + " 10 f a" + U + "a\n", "")));
    CHECK(!dok(v2(H1 + " 10 f a" + U + "a\n", "../web")));

    // kinds and pairing
    CHECK(!dok(v2(H1 + " 10 x a" + U + "a\n")));                          // unknown kind
    CHECK(!dok(v2(H1 + " 10 f . " + U.substr(1) + "a\n")));               // a file can't be "."
    CHECK(!dok(v2(H2 + " 10 t akb5" + U + "a\n")));                       // tree without its index
    CHECK(!dok(v2(H1 + " 10 i akb5" + U + "a\n")));                       // index without its tree
    CHECK(!dok(v2(H1 + " 10 i akb5" + U + "a\n" + H1 + " 10 f akb5" + U + "a\n")));
    CHECK(!dok(v2(H1 + " 10 i akb5" + U + "a\n" + H2 + " 10 t akb6" + U + "a\n")));   // other name
    CHECK(!dok(v2(H1 + " 10 i a" + U + "a\n" + H2 + " 10 t a" + U + "a\n" + H2 + " 10 t a" + U + "a\n")));
    CHECK(!dok(v2(H1 + " 600000 i a" + U + "a\n" + H2 + " 10 t a" + U + "a\n")));      // index over 512 KB
    CHECK(dok(v2(H1 + " 524288 i a" + U + "a\n" + H2 + " 10 t a" + U + "a\n")));

    // overlaps
    CHECK(!dok(v2(H1 + " 10 f a" + U + "a\n" + H2 + " 10 f b" + U + "b\n" + H1 + " 10 f a" + U + "a\n")));
    CHECK(!dok(v2(H1 + " 10 i . " + U.substr(1) + "x\n" + H2 + " 10 t ." + U + "y\n" + H1 + " 10 f a" + U + "a\n")));
    CHECK(!dok(v2(H1 + " 10 i akb5" + U + "x\n" + H2 + " 10 t akb5" + U + "y\n" + H1 + " 10 f akb5/x.bin" + U + "a\n")));
    CHECK(dok(v2(H1 + " 10 i akb5" + U + "x\n" + H2 + " 10 t akb5" + U + "y\n" + H1 + " 10 f akb5x.bin" + U + "a\n")));

    // multi-part file, as in v1
    CHECK(dok(v2(H1 + " 2000000000 f big.bin" + U + "big.001\n" + H2 + " 5 f big.bin" + U + "big.002\n"), d.get()) &&
          d->n == 2 && data_total(*d) == 2000000005ull);

    // names: paths, no staging names
    CHECK(!dok(v2(H1 + " 10 f a.part" + U + "a\n")));
    CHECK(!dok(v2(H1 + " 10 f a.new" + U + "a\n")));
    CHECK(!dok(v2(H1 + " 10 f x/a.old" + U + "a\n")));
    CHECK(!dok(v2(H1 + " 10 f a.tmp" + U + "a\n")));
    CHECK(!dok(v2(H1 + " 10 f ../a" + U + "a\n")));
    CHECK(!dok(v2(H1 + " 10 f /a" + U + "a\n")));
    CHECK(!dok(v2(H1 + " 10 f a/b/c/d" + U + "a\n")));                     // over 3 segments
    CHECK(dok(v2(H1 + " 10 f a/b/c" + U + "a\n")));

    // urls
    CHECK(!dok(v2(H1 + " 10 f a http://x/a\n")));
    CHECK(!dok(v2(H1 + " 10 f a" + U + "a" + U + "b" + U + "c\n")));      // at most one mirror
    CHECK(!dok(v2(H1 + " 10 f a" + U + "a http://mirror/a\n")));
    CHECK(!dok(v2(H1 + " 10 f a" + U + "a \n")));

    // v1 is untouched: no kind column, no mirror, no v2 dest, one-segment names under 48 chars
    const std::string v1h = "nucleoos-data-v1\nwiki-it-top\n2026.7\nanima/kb\n";
    CHECK(dok(v1h + H1 + " 10 wiki-it.akb6" + U + "a\n" + kSig, d.get()) && d->format == 1 && d->parts[0].kind == 'f');
    CHECK(!dok(v1h + H1 + " 10 f wiki-it.akb6" + U + "a\n" + kSig));       // "f" read as the name, then junk
    CHECK(!dok(v1h + H1 + " 10 a.akb6" + U + "a" + U + "b\n" + kSig));
    CHECK(!dok(v1h + H1 + " 10 a/b" + U + "a\n" + kSig));
    CHECK(!dok("nucleoos-data-v1\nx\n1\nweb\n" + H1 + " 10 a" + U + "a\n" + kSig));
    CHECK(!dok(v1h + H1 + " 10 " + std::string(48, 'n') + U + "a\n" + kSig));
    CHECK(dok(v1h + H1 + " 10 " + std::string(47, 'n') + U + "a\n" + kSig));
    CHECK(!dok("nucleoos-data-v3\nx\n1\nweb\n" + H1 + " 10 f a" + U + "a\n" + kSig));

    // ---- tree paths ----
    CHECK(tr::path_ok("index.html", 10) && tr::path_ok("apps/anima/local/engine.js", 26));
    CHECK(tr::path_ok("a+b.js", 6) && !tr::path_ok("a b", 3) && !tr::path_ok("a//b", 4) && !tr::path_ok("/a", 2));
    CHECK(!tr::path_ok("a/../b", 6) && !tr::path_ok("./a", 3) && !tr::path_ok("a/", 2) && !tr::path_ok("", 0));
    CHECK(tr::path_ok("1/2/3/4/5/6/7/8", 15) && !tr::path_ok("1/2/3/4/5/6/7/8/9", 17));
    const std::string longp(159, 'x');
    CHECK(tr::path_ok(longp.c_str(), 159) && !tr::path_ok((longp + "x").c_str(), 160));

    // ---- index ----
    {
        static tr::Index ix;
        const std::string idx = "nucleoos-tree-v1\n" + H1 + " 12 apps/a.js\n" + H2 + " 0 empty\n" + H1 + " 5 index.html\n";
        CHECK(tr::parse_index(idx.data(), idx.size(), &ix) && ix.n == 3);
        tr::Entry e;
        CHECK(tr::index_entry(ix, 0, &e) && !strcmp(e.path, "apps/a.js") && e.size == 12 && e.sha256[0] == 0xaa);
        CHECK(tr::index_entry(ix, 1, &e) && !strcmp(e.path, "empty") && e.size == 0 && e.sha256[0] == 0xbb);
        CHECK(!tr::index_entry(ix, 3, &e));
        CHECK(tr::index_find(ix, "index.html") == 2 && tr::index_find(ix, "apps/a.js") == 0);
        CHECK(tr::index_find(ix, "apps") == -1 && tr::index_find(ix, "zzz") == -1 && tr::index_find(ix, "") == -1);
        CHECK(tr::index_total(ix) == 17);
        auto bad = [&](const std::string &body) { const std::string t = "nucleoos-tree-v1\n" + body; return !tr::parse_index(t.data(), t.size(), &ix); };
        CHECK(bad(""));                                                    // no files
        CHECK(bad(H1 + " 1 b\n" + H1 + " 1 a\n"));                         // unsorted
        CHECK(bad(H1 + " 1 a\n" + H1 + " 1 a\n"));                         // duplicate
        CHECK(bad(H1 + " 1 a\n" + H1 + " 1 a.b\n" + H1 + " 1 a/b\n"));     // "a" is a file and a folder
        CHECK(bad(H1 + " 01 a\n"));
        CHECK(bad(H1 + " 1 a"));                                           // no final LF
        CHECK(bad(H1 + " 1 ../a\n"));
        CHECK(bad(H1 + " 4294967296 a\n"));
        CHECK(!bad(H1 + " 4294967295 a\n"));
        const std::string nohead = H1 + " 1 a\n";
        CHECK(!tr::parse_index(nohead.data(), nohead.size(), &ix));
    }

    // ---- ustar ----
    {
        const std::string body(1000, 'z');
        const std::string deep = "apps/anima/local/" + std::string(90, 'q') + "/engine.js";   // needs the prefix field
        const std::string a = tar_header("./", 0, '5') + tar_header("./apps/", 0, '5') + tar_file("./apps/a.js", "hello world!") +
                              tar_file("index.html", "<html>") + tar_file("empty", "") + tar_file(deep, body) + tar_end();
        for (size_t chunk : {1u, 7u, 512u, 513u, 100000u}) {
            Collect c;
            CHECK(untar(a, &c, chunk));
            CHECK(c.files.size() == 4 && c.files["apps/a.js"] == "hello world!" && c.files["index.html"] == "<html>");
            CHECK(c.files.count("empty") && c.files["empty"].empty() && c.files[deep] == body);
            CHECK(c.dirs.size() == 1 && c.dirs[0] == "apps");
        }
        Collect c;
        CHECK(untar(tar_file("a", "x") + std::string(1024, '\0'), &c));     // exactly two zero blocks
        CHECK(!untar(tar_file("a", "x") + std::string(512, '\0'), &c));      // one: not finished
        CHECK(!untar(tar_file("a", "x"), &c));                               // no end marker
        CHECK(!untar(tar_file("a", "xyz").substr(0, 514) + tar_end(), &c));  // truncated body
        CHECK(!untar(tar_file("a", "x") + std::string(512, '\0') + tar_file("b", "y") + tar_end(), &c));   // after end
        CHECK(!untar(tar_header("a", 0, '2') + tar_end(), &c));              // symlink
        CHECK(!untar(tar_header("a", 0, '1') + tar_end(), &c));              // hard link
        CHECK(!untar(tar_header("a", 10, 'x') + tar_end(), &c));             // pax header
        CHECK(!untar(tar_header("a", 10, 'L') + tar_end(), &c));             // GNU long name
        CHECK(!untar(tar_header("a", 0, '0', "ustar\0" "00", true) + tar_end(), &c));   // bad checksum
        CHECK(!untar(tar_header("a", 0, '0', "notar\0" "00") + tar_end(), &c));
        CHECK(untar(tar_header("a", 0, '0', "ustar  \0") + tar_end(), &c));  // GNU magic
        CHECK(!untar(tar_file("../a", "x") + tar_end(), &c));
        CHECK(!untar(tar_file("/etc/a", "x") + tar_end(), &c));
        CHECK(!untar(tar_file("a/./b", "x") + tar_end(), &c));
        CHECK(!untar(tar_header("d", 5, '5') + tar_end(), &c));              // a folder with a size
        std::string pad = tar_file("a", "x");
        pad[513] = 'J';                                                      // padding must be zero
        CHECK(!untar(pad + tar_end(), &c));
        Collect no;
        no.refuse = true;
        CHECK(!untar(tar_file("a", "x") + tar_end(), &no));                  // the sink says no
    }

    // ---- interrupted swap ----
    {
        using namespace nv_store_tree;
        // journal: the new tree was complete
        CHECK(swap_recover(true, true, true, false) == (SWAP_DIR_TO_OLD | SWAP_NEW_TO_DIR | SWAP_RM_OLD));
        CHECK(swap_recover(true, true, true, true) == (SWAP_RM_STALE | SWAP_DIR_TO_OLD | SWAP_NEW_TO_DIR | SWAP_RM_OLD));
        CHECK(swap_recover(true, false, true, true) == (SWAP_NEW_TO_DIR | SWAP_RM_OLD));
        CHECK(swap_recover(true, false, true, false) == SWAP_NEW_TO_DIR);
        CHECK(swap_recover(true, true, false, true) == SWAP_RM_OLD);
        CHECK(swap_recover(true, true, false, false) == 0);
        CHECK(swap_recover(true, false, false, true) == SWAP_OLD_TO_DIR);
        CHECK(swap_recover(true, false, false, false) == 0);
        // no journal: an extraction that never finished, or a delete that never finished
        CHECK(swap_recover(false, true, true, false) == SWAP_RM_NEW);
        CHECK(swap_recover(false, false, true, false) == SWAP_RM_NEW);
        CHECK(swap_recover(false, true, false, true) == SWAP_RM_OLD);
        CHECK(swap_recover(false, false, false, true) == SWAP_OLD_TO_DIR);
        CHECK(swap_recover(false, true, false, false) == 0);
        // whatever the state, D exists afterwards when some version of it did
        for (int s = 0; s < 16; s++) {
            const bool j = s & 1, dir = s & 2, nw = s & 4, old = s & 8;
            const unsigned a = swap_recover(j, dir, nw, old);
            bool D = dir, N = nw, O = old;
            if (a & SWAP_RM_STALE) { CHECK(O); O = false; }
            if (a & SWAP_DIR_TO_OLD) { CHECK(D && !O); O = true; D = false; }
            if (a & SWAP_NEW_TO_DIR) { CHECK(N && !D); D = true; N = false; }
            if (a & SWAP_OLD_TO_DIR) { CHECK(O && !D); D = true; O = false; }
            if (a & SWAP_RM_NEW) { CHECK(N); N = false; }
            if (a & SWAP_RM_OLD) { CHECK(O); O = false; }
            CHECK(!O && !N);                                                 // nothing left behind
            CHECK(D == (dir || nw || old) || (!j && !dir && !old && nw));     // only an unfinished extraction is dropped
        }
    }

    // ---- the content service's decisions (nv_content_plan) ----
    {
        namespace pl = nv_content_plan;
        CHECK(pl::newer("2026.10.2", "2026.10.1") && pl::newer("2026.11", "2026.10.9") && pl::newer("2027.1.1", "2026.12.31"));
        CHECK(!pl::newer("2026.10.1", "2026.10.1") && !pl::newer("2026.10", "2026.10.0") && !pl::newer("", "1"));
        CHECK(pl::newer("1", "") && pl::newer("2026.10.1.1", "2026.10.1"));

        const pl::Pack web_old{"sys-web", "2026.10.2", "2026.10.1", "*", "2026.10.2"};
        CHECK(pl::state(web_old) == pl::UPDATE);
        CHECK(pl::auto_update(web_old, false));                       // installed + too old + store has it
        CHECK(!pl::auto_update(web_old, true));                       // changed on the device: never overwritten
        const pl::Pack web_missing{"sys-web", "2026.10.2", "", "*", "2026.10.2"};
        CHECK(pl::state(web_missing) == pl::MISSING && !pl::auto_update(web_missing, false));   // the owner decides
        const pl::Pack web_unpublished{"sys-web", "2026.10.1", "2026.10.1", "*", "2026.10.2"};
        CHECK(!pl::auto_update(web_unpublished, false) && pl::requirement_unmet(web_unpublished));   // G6: wait, don't loop
        const pl::Pack web_ok{"sys-web", "2026.10.2", "2026.10.2", "*", "2026.10.1"};
        CHECK(pl::state(web_ok) == pl::OK && !pl::auto_update(web_ok, false) && !pl::requirement_unmet(web_ok));
        const pl::Pack dict_old{"dict-it", "2026.11.1", "2026.10.1", "it", ""};
        CHECK(pl::state(dict_old) == pl::UPDATE && !pl::auto_update(dict_old, false));   // not required: only offered
        const pl::Pack gone{"x", "", "", "*", ""};
        CHECK(pl::state(gone) == pl::UNAVAILABLE);
        const pl::Pack kept{"x", "", "1.0", "*", ""};
        CHECK(pl::state(kept) == pl::OK);                              // installed, no longer listed: stays

        const pl::Pack es{"dict-es", "1", "", "es,it", ""};
        CHECK(pl::recommended(es, "es") && pl::recommended(es, "it") && !pl::recommended(es, "en") && !pl::recommended(es, "e"));
        CHECK(pl::recommended(web_old, "de") && !pl::recommended(pl::Pack{"d", "1", "", "", ""}, "it"));

        CHECK(pl::backoff_s(0) == 0 && pl::backoff_s(1) == 60 && pl::backoff_s(2) == 300 && pl::backoff_s(3) == 900);
        CHECK(pl::backoff_s(4) == 3600 && pl::backoff_s(40) == 3600);
    }

    // real packs written by tools/content/build.py (store_sign.content_pack_text + sign_text)
    auto slurp = [](const char *path, std::string *out) {
        FILE *f = fopen(path, "rb");
        if (!f) return false;
        char b[65536];
        size_t n;
        out->clear();
        while ((n = fread(b, 1, sizeof b, f)) > 0) out->append(b, n);
        fclose(f);
        return true;
    };
    std::string text;
    CHECK(slurp("corpus/content/pack-sys-web", &text));
    CHECK(parse_data(text.data(), text.size(), d.get()) && d->format == 2 && !strcmp(d->dest, "web") &&
          d->n == 2 && d->parts[0].kind == 'i' && d->parts[1].kind == 't' && !strcmp(d->parts[1].name, "."));
    CHECK(slurp("corpus/content/pack-anima-core-it", &text));
    CHECK(parse_data(text.data(), text.size(), d.get()) && !strcmp(d->dest, "data/anima") && d->n == 11);

    // a real tree: Python's tarfile (USTAR_FORMAT) + store_sign.tree_index_text, read by the device code
    {
        static tr::Index ix;
        std::string idx, tar;
        CHECK(slurp("corpus/content/skills.idx", &idx) && slurp("corpus/content/skills.tar", &tar));
        CHECK(tr::parse_index(idx.data(), idx.size(), &ix) && ix.n >= 5);
        Collect c;
        CHECK(untar(tar, &c, 4096));
        CHECK((int)c.files.size() == ix.n);
        for (int i = 0; i < ix.n; i++) {
            tr::Entry e;
            CHECK(tr::index_entry(ix, i, &e) && c.files.count(e.path) && c.files[e.path].size() == e.size);
        }
    }
    return TEST_DONE("content");
}
