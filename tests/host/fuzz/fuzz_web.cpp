// libFuzzer: nv_web_util with LAN-shaped input. Besides memory errors (ASan/UBSan) it checks the
// security property the guards exist for, with an independent oracle that resolves a path the way
// FatFs does (components compared case-insensitively, trailing dots/spaces dropped): no path the
// file API accepts may reach the NVS mirror, and none it would let you modify may reach /sdcard/web.
#include "nv_web_util.h"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace nv_web_util;

// FatFs (FF_FS_RPATH 0) splits on '/' and '\', drops trailing dots/spaces per component and rejects
// a component that ends up empty ("/./web" is FR_INVALID_NAME, not "/web"): `invalid` reports that.
static std::vector<std::string> fat_components(const char *path, bool *invalid = nullptr) {
    std::vector<std::string> v;
    std::string cur;
    bool bad = false;
    auto flush = [&] {
        const bool had = !cur.empty();
        while (!cur.empty() && (cur.back() == '.' || cur.back() == ' ')) cur.pop_back();
        if (!cur.empty()) {
            for (char &c : cur) c = (char)tolower((unsigned char)c);
            v.push_back(cur);
        } else if (had) {
            bad = true;
        }
        cur.clear();
    };
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') flush();
        else cur += *p;
    }
    flush();
    if (invalid) *invalid = bad;
    return v;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    // two NUL-separated strings: a query value and a JSON-ish body
    std::string in((const char *)data, size);
    const size_t cut = in.find('\0');
    std::string q = in.substr(0, cut), body = cut == std::string::npos ? "" : in.substr(cut + 1);
    body = body.c_str();   // stop at an inner NUL, like the C string the firmware sees

    // query value -> url_decode at the firmware's buffer sizes (and tiny ones)
    for (size_t n : {size_t(1), size_t(2), size_t(7), size_t(256)}) {
        std::vector<char> out(n);
        url_decode(q.c_str(), out.data(), n);
        if (strlen(out.data()) >= n) abort();
    }
    char logical[256];
    url_decode(q.c_str(), logical, sizeof logical);

    char phys[320];
    if (map_fs(logical, phys, sizeof phys)) {
        bool invalid = false;
        const auto comps = fat_components(phys, &invalid);
        if (!invalid) {                                                // FatFs would refuse it anyway
            for (const auto &c : comps)
                if (c == "settings.nvb") abort();                     // NVS mirror reachable
            const bool in_web = comps.size() >= 2 && comps[0] == "sdcard" && comps[1] == "web";
            if (in_web && fs_writable(phys)) abort();                 // served web OS modifiable
        }
    }
    if (open_path_ok(logical)) {
        bool invalid = false;
        const auto comps = fat_components(logical, &invalid);
        if (!invalid)
            for (const auto &c : comps)
                if (c == "settings.nvb") abort();
    }
    (void)mime_for(logical);

    // JSON helpers on the body
    char s[64];
    if (json_str(body.c_str(), "ssid", s, sizeof s) && strlen(s) >= sizeof s) abort();
    (void)json_int(body.c_str(), "n", 0);
    for (size_t n : {size_t(1), size_t(3), size_t(8), size_t(96)}) {
        std::vector<char> e(n);
        json_escape(e.data(), n, body.c_str());
        if (strlen(e.data()) >= n) abort();
    }
    return 0;
}
