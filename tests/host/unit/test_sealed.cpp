// Unit tests for nv_sealed's idea of "a secret file": every spelling FatFs opens as the same file
// ('\' separators, "//" and "/./", any case, trailing dots and spaces, the 8.3 alias of a long name,
// numbered or hashed) and the "<secret>.tmp" copy a write leaves are the secret; near misses are not.
// The web API, the shell, WASI and the Files app refuse on these answers.
#include "check.h"
#include "nv_sealed.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

int main()
{
    static const char *const kIs[] = {
        "/sdcard/data/anima/teacher.json",
        "/sdcard/DATA/Anima/Teacher.JSON",
        "/sdcard/data/anima/teacher.json.",
        "/sdcard/data/anima/teacher.json. .",
        "/sdcard/data//anima/teacher.json",
        "/sdcard/data/./anima/teacher.json",
        "/sdcard/data\\anima\\teacher.json",
        "/sdcard\\data/anima/teacher.json",
        "/sdcard/data/anima/TEACHE~1.JSO",
        "/sdcard/data/anima/teache~1.jso",
        "/sdcard/data/anima/TEACHE~9.JSO",
        "/sdcard/data/anima/TE~1A2B3.JSO",       // hashed tail after many collisions
        "/sdcard/data/anima/TEAC~F3.JSO",
        "/sdcard/data/anima/TEACHE~1.JSO.",
        "/sdcard/data/anima/teacher.json.tmp",
        "/sdcard/data/anima/TEACHER.JSON.TMP",
        "/sdcard/data/anima/TEACHE~1.TMP",       // the .tmp copy by its own alias
        "/sdcard/data/anima/telegram.json",
        "/sdcard/data/anima/TELEGR~1.JSO",
        "/sdcard/system/config/authenticator.json",
        "/sdcard/system/config/AUTHEN~2.JSO",
        "/sdcard/system/config/authenticator.json.tmp",
    };
    for (const char *p : kIs) {
        const bool ok = nv_sealed_path(p) && nv_sealed_within(p);
        if (!ok) std::fprintf(stderr, "  not secret: %s\n", p);
        CHECK(ok);
    }

    static const char *const kNot[] = {
        "/sdcard/data/anima/teacher.json.bak",
        "/sdcard/data/anima/teacher.jso",
        "/sdcard/data/anima/teacher.jsonx",
        "/sdcard/data/anima/xteacher.json",
        "/sdcard/data/anima/ teacher.json",       // a leading space is part of the long name
        "/sdcard/data/anima/TEACHE~1.JSN",        // another extension
        "/sdcard/data/anima/TEACHX~1.JSO",        // other letters before '~'
        "/sdcard/data/anima/TEACHE~G.JSO",        // not a hex tail
        "/sdcard/data/anima/TEACHE~.JSO",
        "/sdcard/data/anima/TEACHER~1.JSO",       // 9 characters: no 8.3 name
        "/sdcard/data/anima/TEACHE~1.JSON",       // a 4-letter extension: no 8.3 name
        "/sdcard/data/anima/permissions.json",
        "/sdcard/data/anima/PERMIS~1.JSO",
        "/sdcard/data/anima/teacher.json/x",
        "/sdcard/home/teacher.json",
        "/sdcard/data/teacher.json",
        "/sdcard/data/anima/telegram.json.tmp.tmp",
        "",
    };
    for (const char *p : kNot) {
        const bool ok = !nv_sealed_path(p);
        if (!ok) std::fprintf(stderr, "  secret?: %s\n", p);
        CHECK(ok);
    }
    CHECK(!nv_sealed_path(nullptr) && !nv_sealed_within(nullptr));

    // Folders on a secret's way: moving one moves the secret.
    CHECK(nv_sealed_within("/sdcard/data/anima"));
    CHECK(nv_sealed_within("/sdcard/data/anima/"));
    CHECK(nv_sealed_within("/sdcard/DATA"));
    CHECK(nv_sealed_within("/sdcard/data\\anima"));
    CHECK(nv_sealed_within("/sdcard/system/config"));
    CHECK(!nv_sealed_within("/sdcard/home"));
    CHECK(!nv_sealed_within("/sdcard/data/zim"));
    CHECK(!nv_sealed_within("/sdcard/data/anima/skills"));
    CHECK(!nv_sealed_path("/sdcard/data/anima"));

    // One named secret, by any of its spellings, and only that one.
    CHECK(nv_sealed_is("/sdcard/data/anima/TEACHE~1.JSO", "/sdcard/data/anima/teacher.json"));
    CHECK(nv_sealed_is("/sdcard/data\\anima/teacher.json.", "/sdcard/data/anima/teacher.json"));
    CHECK(!nv_sealed_is("/sdcard/data/anima/telegram.json", "/sdcard/data/anima/teacher.json"));
    CHECK(!nv_sealed_is("/sdcard/data/anima/teacher.json.tmp", "/sdcard/data/anima/teacher.json"));
    CHECK(!nv_sealed_is("/sdcard/system/config/authenticator.json.tmp", "/sdcard/system/config/authenticator.json"));

    // The write path: a secret's .tmp copy is never written as a file; ordinary files are, atomically.
    CHECK(!nv_sealed_write("/sdcard/data/anima/teacher.json.tmp", "x", 1));
    CHECK(!nv_sealed_write("/sdcard/data/anima/TEACHE~1.TMP", "x", 1));
    CHECK(!nv_sealed_write(nullptr, "x", 1));
    const char *f = "sealed_unit.txt";
    std::remove(f);
    CHECK(nv_sealed_write(f, "hello", 5));
    size_t n = 0;
    char *back = nv_sealed_read(f, 64, &n);
    CHECK(back && n == 5 && !std::memcmp(back, "hello", 5));
    std::free(back);
    CHECK(nv_sealed_read(f, 4, &n) == nullptr);   // over `max`
    std::remove(f);

    return TEST_DONE("sealed");
}
