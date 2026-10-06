// nv_content_plan — see header.
#include "nv_content_plan.h"

#include <cstdio>
#include <cstring>

namespace nv_content_plan {

namespace {
void fields(const char *v, long out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0;
    for (int i = 0; v && *v && i < 4; i++) {
        long n = 0;
        while (*v >= '0' && *v <= '9') { if (n < 100000000) n = n * 10 + (*v - '0'); v++; }
        out[i] = n;
        if (*v != '.') break;
        v++;
    }
}
bool empty(const char *s) { return !s || !s[0]; }
}  // namespace

bool newer(const char *a, const char *b) {
    long x[4], y[4];
    fields(a, x);
    fields(b, y);
    for (int i = 0; i < 4; i++) if (x[i] != y[i]) return x[i] > y[i];
    return false;
}

State state(const Pack &p) {
    if (empty(p.installed)) return empty(p.avail) ? UNAVAILABLE : MISSING;
    return !empty(p.avail) && newer(p.avail, p.installed) ? UPDATE : OK;
}

bool recommended(const Pack &p, const char *lang) {
    if (empty(p.langs)) return false;
    const size_t ll = lang ? strlen(lang) : 0;
    for (const char *s = p.langs; *s; ) {
        const char *e = strchr(s, ',');
        const size_t n = e ? (size_t)(e - s) : strlen(s);
        if ((n == 1 && s[0] == '*') || (ll && n == ll && !strncmp(s, lang, n))) return true;
        if (!e) break;
        s = e + 1;
    }
    return false;
}

bool auto_update(const Pack &p, bool web_local) {
    if (empty(p.required_min) || empty(p.installed) || empty(p.avail)) return false;
    if (web_local && p.id && !strcmp(p.id, "sys-web")) return false;
    return newer(p.required_min, p.installed) && !newer(p.required_min, p.avail);
}

bool requirement_unmet(const Pack &p) {
    if (empty(p.required_min)) return false;
    const bool have = !empty(p.installed) && !newer(p.required_min, p.installed);
    return !have && (empty(p.avail) || newer(p.required_min, p.avail));
}

uint32_t backoff_s(int failures) {
    if (failures <= 0) return 0;
    if (failures == 1) return 60;
    if (failures == 2) return 5 * 60;
    if (failures == 3) return 15 * 60;
    return 60 * 60;
}

}  // namespace nv_content_plan
