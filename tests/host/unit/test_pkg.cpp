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

    return TEST_DONE("pkg");
}
