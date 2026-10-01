// Unit tests for nv_wasi_ro.h: the read-only "/package" preopen of engine packages ("wasi" 1.3).
// Only exactly /sdcard/apps/<valid id> is a package root; every open that could change a file
// through it must be refused.
#include "check.h"
#include "nv_wasi_ro.h"

#include <string>

int main() {
    CHECK(nv_wasi_is_pkg_root("/sdcard/apps/rpncalc"));
    CHECK(nv_wasi_is_pkg_root("/sdcard/apps/svm-bass"));
    CHECK(nv_wasi_is_pkg_root("/sdcard/apps/A_b-9"));
    CHECK(nv_wasi_is_pkg_root(("/sdcard/apps/" + std::string(32, 'a')).c_str()));
    CHECK(!nv_wasi_is_pkg_root(("/sdcard/apps/" + std::string(33, 'a')).c_str()));
    CHECK(!nv_wasi_is_pkg_root("/sdcard/apps/"));
    CHECK(!nv_wasi_is_pkg_root("/sdcard/apps"));
    CHECK(!nv_wasi_is_pkg_root("/sdcard/apps/rpncalc/"));
    CHECK(!nv_wasi_is_pkg_root("/sdcard/apps/rpncalc/data"));    // the private folder stays writable
    CHECK(!nv_wasi_is_pkg_root("/sdcard/apps/rpncalc/snd"));
    CHECK(!nv_wasi_is_pkg_root("/sdcard/apps/.."));
    CHECK(!nv_wasi_is_pkg_root("/sdcard/apps/a.b"));
    CHECK(!nv_wasi_is_pkg_root("/sdcard/home"));
    CHECK(!nv_wasi_is_pkg_root("/sdcard/home/apps/x"));
    CHECK(!nv_wasi_is_pkg_root("/SDCARD/apps/x") && !nv_wasi_is_pkg_root("sdcard/apps/x"));
    CHECK(!nv_wasi_is_pkg_root("/sdcard/apps/x y") && !nv_wasi_is_pkg_root(""));
    CHECK(!nv_wasi_is_pkg_root(nullptr));

    CHECK(!nv_wasi_open_writes(O_RDONLY));
    CHECK(!nv_wasi_open_writes(O_RDONLY | O_NONBLOCK));
    CHECK(nv_wasi_open_writes(O_WRONLY));
    CHECK(nv_wasi_open_writes(O_RDWR));
    CHECK(nv_wasi_open_writes(O_RDONLY | O_CREAT));
    CHECK(nv_wasi_open_writes(O_RDONLY | O_TRUNC));
    CHECK(nv_wasi_open_writes(O_RDONLY | O_APPEND));
    CHECK(nv_wasi_open_writes(O_WRONLY | O_CREAT | O_EXCL));
    return TEST_DONE("wasiro");
}
