// nv_wasm_app_on_home: which installed packages get a launcher icon. The tablet home once showed
// every terminal tool the OS installs by itself (lua, python, zstd...) as an icon that only opened
// the Terminal; console games (interactive fiction) must keep theirs.
#include "check.h"
#include <cstdio>
#include <cstring>
extern "C" {
#include "nv_wasm.h"
}

static nv_wasm_app_t app(bool console, const char *category, bool library = false)
{
    nv_wasm_app_t a;
    std::memset(&a, 0, sizeof a);
    a.console = console;
    a.library = library;
    std::snprintf(a.category, sizeof a.category, "%s", category);
    return a;
}

int main()
{
    // windowed apps and games: always on home, whatever the category
    nv_wasm_app_t a = app(false, "");
    CHECK(nv_wasm_app_on_home(&a, false));
    a = app(false, "games");
    CHECK(nv_wasm_app_on_home(&a, false));
    a = app(false, "terminal");               // a mis-filed windowed app still opens a window
    CHECK(nv_wasm_app_on_home(&a, false));
    a = app(false, "utilities");
    CHECK(nv_wasm_app_on_home(&a, true));     // a windowed system app keeps its icon

    // libraries: never
    a = app(false, "games", true);
    CHECK(!nv_wasm_app_on_home(&a, false));
    a = app(true, "games", true);
    CHECK(!nv_wasm_app_on_home(&a, false));

    // terminal tools: the OS's own (any category, even none yet) and the store's "terminal" ones
    a = app(true, "");
    CHECK(!nv_wasm_app_on_home(&a, true));
    a = app(true, "terminal");
    CHECK(!nv_wasm_app_on_home(&a, true));
    CHECK(!nv_wasm_app_on_home(&a, false));

    // console games and console apps of unknown category keep their icon
    a = app(true, "games");
    CHECK(nv_wasm_app_on_home(&a, false));
    a = app(true, "");
    CHECK(nv_wasm_app_on_home(&a, false));

    CHECK(!nv_wasm_app_on_home(nullptr, false));
    return TEST_DONE("launcher");
}
