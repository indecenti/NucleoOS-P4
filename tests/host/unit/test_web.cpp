// nv_web_util: the web API's LAN-input helpers.
#include "check.h"
#include "nv_web_util.h"

#include <cstring>
#include <string>

using namespace nv_web_util;

static std::string mapped(const char *logical) {
    char out[320];
    return map_fs(logical, out, sizeof out) ? std::string(out) : std::string("<refused>");
}

int main() {
    // --- url_decode
    char o[64];
    url_decode("a%20b+c%2Fd", o, sizeof o);           CHECK(!strcmp(o, "a b c/d"));
    url_decode("%zz%4", o, sizeof o);                 CHECK(!strcmp(o, "%zz%4"));   // malformed escapes kept
    url_decode("abcdef", o, 4);                       CHECK(!strcmp(o, "abc"));     // truncates, terminated
    url_decode("x", o, 1);                            CHECK(o[0] == '\0');
    memset(o, 'Z', sizeof o); url_decode("abc", o, 0); CHECK(o[0] == 'Z');         // n = 0 writes nothing

    // --- map_fs: normal paths
    CHECK(mapped("/DCIM/a.jpg") == "/sdcard/DCIM/a.jpg");
    CHECK(mapped("/mnt/usb2/x") == "/usb2/x");
    CHECK(mapped("/mnt/usb9/x") == "/sdcard/mnt/usb9/x");   // not a drive slot: stays on the card
    CHECK(mapped("relative") == "<refused>");
    CHECK(mapped("/a/../b") == "<refused>");
    CHECK(mapped("/a\\b") == "<refused>");
    CHECK(mapped("//web/index.html") == "<refused>");
    CHECK(mapped("/./web/index.html") == "<refused>");   // fuzz_web find: blank components
    CHECK(mapped("/a/ /b") == "<refused>");
    CHECK(mapped("/a/.../b") == "<refused>");
    CHECK(mapped("/v.1/x") == "/sdcard/v.1/x");

    // --- the NVS mirror (Wi-Fi credentials) must be unreachable however FatFs would spell it:
    // case-insensitive names, trailing dots/spaces dropped (regression: /SETTINGS.NVB was served)
    CHECK(mapped("/settings.nvb") == "<refused>");
    CHECK(mapped("/SETTINGS.NVB") == "<refused>");
    CHECK(mapped("/Settings.Nvb") == "<refused>");
    CHECK(mapped("/settings.nvb.") == "<refused>");
    CHECK(mapped("/settings.nvb ") == "<refused>");
    CHECK(mapped("/sub/SETTINGS.nvb") == "<refused>");
    CHECK(mapped("/mnt/usb0/settings.NVB") == "<refused>");
    CHECK(mapped("/settings.nvb.txt") == "/sdcard/settings.nvb.txt");   // a different file

    // --- fs_writable: the served web OS is read-only (regression: /WEB/ and /web./ were writable)
    CHECK(!fs_writable("/sdcard/web"));
    CHECK(!fs_writable("/sdcard/web/index.html"));
    CHECK(!fs_writable("/sdcard/WEB/index.html"));
    CHECK(!fs_writable("/sdcard/Web/apps/x.js"));
    CHECK(!fs_writable("/sdcard/web./index.html"));
    CHECK(!fs_writable("/sdcard/web /index.html"));
    CHECK(!fs_writable("/sdcard/web.."));
    CHECK(fs_writable("/sdcard/webcam/a.jpg"));
    CHECK(fs_writable("/sdcard/DCIM/web/a.jpg"));
    CHECK(fs_writable("/usb0/web/index.html"));        // a USB drive is not the served tree
    CHECK(!fs_writable(mapped("/WEB/index.html").c_str()));

    // --- open_path_ok
    CHECK(open_path_ok("/sdcard/Music/a.mp3"));
    CHECK(!open_path_ok("/spiffs/x"));
    CHECK(!open_path_ok("/sdcard/../x"));
    CHECK(!open_path_ok("/sdcard/SETTINGS.NVB"));
    CHECK(!open_path_ok("/sdcard/settings.nvb\\"));     // fuzz_web find: '\' is a FatFs separator
    CHECK(!open_path_ok("/sdcard/dr\\0/settings.nvb\\\"\""));
    CHECK(!open_path_ok("/sdcard/x\\y.txt"));

    // --- json
    char s[16];
    CHECK(json_str("{\"ssid\":\"home\\\"x\"}", "ssid", s, sizeof s) && !strcmp(s, "home\"x"));
    CHECK(!json_str("{\"ssid\":42}", "ssid", s, sizeof s));
    CHECK(json_str("{\"k\":\"0123456789abcdefXYZ\"}", "k", s, sizeof s) && strlen(s) == sizeof s - 1);
    CHECK(json_int("{\"n\": -12}", "n", 7) == -12);
    CHECK(json_int("{\"m\":1}", "n", 7) == 7);
    char e[16];
    json_escape(e, sizeof e, "a\"b\\c\n");            CHECK(!strcmp(e, "a\\\"b\\\\c\\u000a"));
    json_escape(e, 4, "\"\"\"");                      CHECK(strlen(e) <= 3);
    json_escape(e, 8, "\x01\x02");                    CHECK(!strcmp(e, "\\u0001"));   // no room for the 2nd

    // --- mime
    CHECK(!strcmp(mime_for("/a/B.HTML"), "text/html; charset=utf-8"));
    CHECK(!strcmp(mime_for("noext"), "application/octet-stream"));

    return TEST_DONE("web");
}
