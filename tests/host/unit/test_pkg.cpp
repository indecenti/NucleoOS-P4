// Unit tests for nv_store_pkg: the package.sig line format written by tools/store_sign.py. A drift
// between the two makes every store install fail, so the canonical shape is pinned here.
#include "check.h"
#include "nv_store_pkg.h"

#include <cstring>
#include <memory>
#include <string>

using namespace nv_store_pkg;

static const std::string H1(64, 'a'), H2(64, 'b');
static const std::string kSig = "sig " + std::string(140, 'c') + "\n";   // 70-byte DER-ish

static std::string pkg(const std::string &files, const std::string &sig = kSig,
                       const std::string &head = "nucleoos-app-v1\nmeteo\n1.0\n") {
    return head + files + sig;
}

static bool ok(const std::string &t, Package *p = nullptr) {
    static std::unique_ptr<Package> tmp(new Package);
    return parse(t.data(), t.size(), p ? p : tmp.get());
}

int main() {
    std::unique_ptr<Package> p(new Package);
    const std::string good = pkg(H1 + " 19008 app.wasm\n" + H2 + " 12 img/a.565\n" + H1 + " 235 manifest.json\n");
    CHECK(ok(good, p.get()));
    CHECK(!strcmp(p->id, "meteo") && !strcmp(p->version, "1.0") && p->n_files == 3);
    CHECK(p->files[0].size == 19008 && p->files[0].sha256[0] == 0xaa && p->files[1].sha256[31] == 0xbb);
    CHECK(p->signed_len == good.size() - kSig.size());      // the signature covers all but its line
    CHECK(p->sig_len == 70 && p->sig[0] == 0xcc);
    CHECK(find(*p, "img/a.565") == &p->files[1]);
    CHECK(find(*p, "img/b.565") == nullptr && find(*p, nullptr) == nullptr);

    // a Lua app ("engine" package): its bundle app.lpk is a signed file like any other ("wasi" 1.3
    // installs it from the package), sorted among the others
    const std::string lua = pkg(H1 + " 7260 app.lpk\n" + H2 + " 900 icon.z\n" + H1 + " 812 manifest.json\n");
    CHECK(ok(lua, p.get()) && p->n_files == 3);
    CHECK(find(*p, "app.lpk") == &p->files[0] && p->files[0].size == 7260);
    CHECK(find(*p, "app.wasm") == nullptr && find(*p, "../app.lpk") == nullptr);

    // header
    CHECK(!ok(pkg(H1 + " 1 app.wasm\n", kSig, "nucleoos-app-v2\nmeteo\n1.0\n")));
    CHECK(!ok(pkg(H1 + " 1 app.wasm\n", kSig, "nucleoos-app-v1\nme/teo\n1.0\n")));
    CHECK(!ok(pkg(H1 + " 1 app.wasm\n", kSig, "nucleoos-app-v1\nmeteo\n1.0-rc\n")));
    CHECK(!ok(pkg(H1 + " 1 app.wasm\n", kSig, "nucleoos-app-v1\r\nmeteo\n1.0\n")));   // CRLF
    CHECK(!ok(pkg(H1 + " 1 app.wasm\n", kSig, "nucleoos-app-v1\n\n1.0\n")));

    // file lines
    CHECK(!ok(pkg("")));                                                   // no files
    CHECK(!ok(pkg(std::string(64, 'A') + " 1 app.wasm\n")));                // uppercase hex
    CHECK(!ok(pkg(H1.substr(1) + " 1 app.wasm\n")));
    CHECK(!ok(pkg(H1 + " 01 app.wasm\n")));                                 // non-canonical size
    CHECK(ok(pkg(H1 + " 0 app.wasm\n")));
    CHECK(!ok(pkg(H1 + " 25165825 app.wasm\n")));                           // over kFileMax
    CHECK(ok(pkg(H1 + " 25165824 app.wasm\n")));
    CHECK(!ok(pkg(H1 + " 1  app.wasm\n")));
    CHECK(!ok(pkg(H1 + " 1\n")));
    CHECK(!ok(pkg(H1 + " 1 b\n" + H1 + " 1 a\n")));                         // unsorted
    CHECK(!ok(pkg(H1 + " 1 a\n" + H1 + " 1 a\n")));                         // duplicate
    CHECK(!ok(pkg(H1 + " 1 ../x\n")));
    CHECK(!ok(pkg(H1 + " 1 /x\n")));
    CHECK(!ok(pkg(H1 + " 1 a//b\n")));
    CHECK(!ok(pkg(H1 + " 1 a/./b\n")));
    CHECK(!ok(pkg(H1 + " 1 a/b/c/d\n")));                                   // 3 directory levels
    CHECK(ok(pkg(H1 + " 1 a/b/c\n")));
    CHECK(!ok(pkg(H1 + " 1 a\\b\n")));
    CHECK(!ok(pkg(H1 + " 1 " + std::string(64, 'x') + "\n")));
    CHECK(ok(pkg(H1 + " 1 " + std::string(63, 'x') + "\n")));
    CHECK(!ok(pkg(H1 + " 1 a" + std::string(1, '\0') + "b\n")));

    // signature line
    CHECK(!ok(pkg(H1 + " 1 a\n", "")));
    CHECK(!ok(pkg(H1 + " 1 a\n", "sig \n")));
    CHECK(!ok(pkg(H1 + " 1 a\n", "sig " + std::string(141, 'c') + "\n")));   // odd length
    CHECK(!ok(pkg(H1 + " 1 a\n", "sig " + std::string(146, 'c') + "\n")));   // > 72 bytes
    CHECK(!ok(pkg(H1 + " 1 a\n", "sig " + std::string(140, 'C') + "\n")));
    CHECK(!ok(pkg(H1 + " 1 a\n", "sig " + std::string(140, 'c'))));          // no final LF
    CHECK(!ok(pkg(H1 + " 1 a\n", kSig + "x\n")));                             // trailing data

    // limits
    std::string many;
    char name[16];
    for (int i = 0; i < kMaxFiles; i++) { snprintf(name, sizeof name, "f%04d", i); many += H1 + " 1 " + name + "\n"; }
    CHECK(ok(pkg(many), p.get()) && p->n_files == kMaxFiles);
    snprintf(name, sizeof name, "f%04d", kMaxFiles);
    CHECK(!ok(pkg(many + H1 + " 1 " + name + "\n")));
    CHECK(!parse(good.data(), kTextMax + 1, p.get()));

    // A real package.sig written by tools/store_sign.py (run from tests/host).
    if (FILE *f = fopen("corpus/pkg/meteo", "rb")) {
        static char buf[kTextMax];
        const size_t n = fread(buf, 1, sizeof buf, f);
        fclose(f);
        CHECK(parse(buf, n, p.get()) && !strcmp(p->id, "meteo") && find(*p, "manifest.json"));
        CHECK(p->sig_len >= 68 && p->sig_len <= 72);
    } else {
        CHECK(!"corpus/pkg/meteo missing");
    }

    // path_ok directly
    CHECK(path_ok("img/x.565") && path_ok("a") && path_ok("a.b-c_d"));
    CHECK(!path_ok("") && !path_ok(".") && !path_ok("..") && !path_ok("a/") && !path_ok("a b"));

    // ---- data packs (pack.sig, "nucleoos-data-v1") ----
    {
        std::unique_ptr<DataPack> d(new DataPack);
        const std::string head = "nucleoos-data-v1\nwiki-it-top\n2026.7\nanima/kb\n";
        const std::string url = " https://github.com/indecenti/nucleoos-p4-store/releases/download/kb-2026.7/";
        auto dp = [&](const std::string &body, const std::string &h = "") { return (h.empty() ? head : h) + body + kSig; };
        auto dok = [&](const std::string &t) { return parse_data(t.data(), t.size(), d.get()); };

        const std::string one = dp(H1 + " 23800000 wikipedia_it_top.akb6" + url + "wikipedia_it_top.akb6\n");
        CHECK(dok(one) && d->n == 1 && !strcmp(d->id, "wiki-it-top") && !strcmp(d->dest, "anima/kb"));
        CHECK(d->parts[0].size == 23800000 && !strcmp(d->parts[0].name, "wikipedia_it_top.akb6"));
        CHECK(!strncmp(d->parts[0].url, "https://github.com/", 19) && d->signed_len == one.size() - kSig.size());

        // parts: consecutive lines with the same name make one file (> 4 GB in all is refused)
        const std::string parts = dp(H1 + " 2000000000 big.akb6" + url + "big.akb6.001\n" +
                                     H2 + " 1500000000 big.akb6" + url + "big.akb6.002\n" +
                                     H1 + " 10 small.tsv" + url + "small.tsv\n");
        CHECK(dok(parts) && d->n == 3 && data_total(*d) == 3500000010ull);
        CHECK(!dok(dp(H1 + " 3000000000 big.akb6" + url + "a\n" + H2 + " 3000000000 big.akb6" + url + "b\n")));
        // a name that comes back after another file: refused (parts must be contiguous)
        CHECK(!dok(dp(H1 + " 10 a.tsv" + url + "1\n" + H1 + " 10 b.tsv" + url + "2\n" + H1 + " 10 a.tsv" + url + "3\n")));

        // where it may land, and what may be fetched
        CHECK(!dok(dp(H1 + " 10 a.tsv" + url + "a\n", "nucleoos-data-v1\nx\n1\napps\n")));        // not a data dest
        CHECK(!dok(dp(H1 + " 10 a.tsv" + url + "a\n", "nucleoos-data-v1\nx\n1\n../etc\n")));
        CHECK(!dok(dp(H1 + " 10 ../a.tsv" + url + "a\n")));                                        // name: one segment
        CHECK(!dok(dp(H1 + " 10 kb/a.tsv" + url + "a\n")));
        CHECK(!dok(dp(H1 + " 10 a.tsv http://example.com/a\n")));                                  // https only
        CHECK(!dok(dp(H1 + " 10 a.tsv https://exa mple.com/a\n")));                                 // no spaces
        CHECK(!dok(dp(H1 + " 0 a.tsv" + url + "a\n")) && !dok(dp(H1 + " 010 a.tsv" + url + "a\n")));  // canonical size
        CHECK(!dok(dp(H1 + " 10 a.tsv" + url + "a\n", "nucleoos-app-v1\nx\n1\nanima/kb\n")));      // domain
        CHECK(!dok(head + H1 + " 10 a.tsv" + url + "a\n"));                                         // no signature
        CHECK(!dok(dp("")));                                                                       // nothing to install
        std::string many;
        for (int i = 0; i <= kDataMax; i++) many += H1 + " 10 f" + std::to_string(i) + url + "x\n";
        CHECK(!dok(dp(many)));
        // a real pack.sig written by tools/store_sign.py data_pack_text() + sign_text() (tools/kb/publish.py)
        if (FILE *f = fopen("corpus/pkg/data-wiki-it-top", "rb")) {
            static char buf[kTextMax];
            const size_t n = fread(buf, 1, sizeof buf, f);
            fclose(f);
            CHECK(parse_data(buf, n, d.get()) && !strcmp(d->id, "wiki-it-top") && !strcmp(d->dest, "anima/kb"));
            CHECK(d->n == 1 && d->parts[0].size > 1000000 && d->sig_len >= 68 && d->sig_len <= 72);
        } else {
            CHECK(!"corpus/pkg/data-wiki-it-top missing");
        }
        // an app package is not a data pack, and the other way round
        CHECK(!parse(one.data(), one.size(), p.get()));
    }

    return TEST_DONE("pkg");
}
