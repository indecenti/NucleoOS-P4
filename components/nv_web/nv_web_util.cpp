// nv_web_util — pure helpers of the web API (see nv_web_util.h). No ESP-IDF includes: this file is
// also compiled on the PC by tests/host.
#include "nv_web_util.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

namespace nv_web_util {

namespace {

// The SD card's FAT/exFAT volume matches names case-insensitively and FatFs drops trailing dots
// and spaces from every path component ("/Web./" opens "/web/", "SETTINGS.NVB " the NVS mirror).
// So a guard must compare components the way the filesystem resolves them, not byte for byte:
// the old strstr/strncmp checks let "/SETTINGS.NVB" be read and "/WEB/index.html" be deleted.
bool component_is(const char *s, size_t len, const char *name) {
    while (len && (s[len - 1] == '.' || s[len - 1] == ' ')) len--;
    return len == strlen(name) && strncasecmp(s, name, len) == 0;
}

// True when any component of `path` resolves to `name`. FatFs splits on '\' as well as '/'.
bool has_component(const char *path, const char *name) {
    for (const char *p = path; *p;) {
        while (*p == '/' || *p == '\\') p++;
        const char *e = p;
        while (*e && *e != '/' && *e != '\\') e++;
        if (e > p && component_is(p, (size_t)(e - p), name)) return true;
        p = e;
    }
    return false;
}

// The NVS mirror carries the Wi-Fi credentials: never serve, overwrite or open it over the LAN.
bool is_nvs_mirror(const char *path) { return has_component(path, "settings.nvb"); }

// A component made only of dots/spaces (".", "...", " ") names nothing: FatFs (FF_FS_RPATH 0) rejects
// it today, and a relative-path build would read "/./web" as "/web" past the fs_writable prefix check.
bool has_blank_component(const char *path) {
    for (const char *p = path; *p;) {
        while (*p == '/' || *p == '\\') p++;
        const char *e = p;
        while (*e && *e != '/' && *e != '\\') e++;
        if (e > p && component_is(p, (size_t)(e - p), "")) return true;
        p = e;
    }
    return false;
}

}  // namespace

void url_decode(const char *in, char *out, size_t n) {
    if (!n) return;
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < n; p++) {
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char hex[3] = {p[1], p[2], 0};
            out[o++] = (char)strtol(hex, nullptr, 16);
            p += 2;
        } else {
            out[o++] = (*p == '+') ? ' ' : *p;
        }
    }
    out[o] = '\0';
}

const char *mime_for(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    struct { const char *ext, *mime; } M[] = {
        {".html", "text/html; charset=utf-8"}, {".htm", "text/html; charset=utf-8"},
        {".js", "text/javascript; charset=utf-8"}, {".mjs", "text/javascript; charset=utf-8"},
        {".css", "text/css; charset=utf-8"}, {".json", "application/json; charset=utf-8"},
        {".webmanifest", "application/manifest+json"}, {".map", "application/json"},
        {".svg", "image/svg+xml"}, {".png", "image/png"}, {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"}, {".gif", "image/gif"}, {".webp", "image/webp"},
        {".ico", "image/x-icon"}, {".bmp", "image/bmp"}, {".wasm", "application/wasm"},
        {".woff2", "font/woff2"}, {".woff", "font/woff"}, {".ttf", "font/ttf"},
        {".txt", "text/plain; charset=utf-8"}, {".mp3", "audio/mpeg"}, {".wav", "audio/wav"},
        {".mp4", "video/mp4"}, {".webm", "video/webm"}, {".avi", "video/x-msvideo"},
        {".mpg", "video/mpeg"}, {".mpeg", "video/mpeg"}, {".m1v", "video/mpeg"},
        {".mkv", "video/x-matroska"}, {".mov", "video/quicktime"},
    };
    for (auto &m : M) if (!strcasecmp(dot, m.ext)) return m.mime;
    return "application/octet-stream";
}

bool map_fs(const char *logical, char *out, size_t n) {
    if (!logical || logical[0] != '/') return false;
    // FATFS accepts '\' as a separator and collapses "//": both let a path slip past the
    // WEB_ROOT prefix guard in fs_writable ("//web/index.html" deleted the served shell).
    if (strstr(logical, "..") || strchr(logical, '\\') || strstr(logical, "//")) return false;
    if (has_blank_component(logical) || is_nvs_mirror(logical)) return false;
    // USB drives: "/mnt/usb0/DCIM" -> "/usb0/DCIM" (everything else stays under the SD card).
    if (!strncmp(logical, "/mnt/", 5) && nv_usb_storage_slot_of(logical + 4) >= 0) {
        snprintf(out, n, "%s", logical + 4);
        return true;
    }
    snprintf(out, n, "%s%s", FS_ROOT, logical);
    return true;
}

bool fs_writable(const char *phys) {
    // Guard mutations: never let the file manager modify the served web-OS tree (/sdcard/web) — a
    // stray delete there would take the OS offline until the next SD re-sync. The first component
    // under FS_ROOT is compared the way FatFs resolves it (see component_is).
    const size_t rl = strlen(FS_ROOT);
    if (strncmp(phys, FS_ROOT, rl) != 0 || phys[rl] != '/') return true;   // not on the SD card
    const char *c = phys + rl + 1;
    const char *e = strchr(c, '/');
    return !component_is(c, e ? (size_t)(e - c) : strlen(c), &WEB_ROOT[rl + 1]);   // "web"
}

bool open_path_ok(const char *p) {
    // Same separator rules as map_fs: FatFs treats '\' as a separator, so "/sdcard/settings.nvb\"
    // opened the NVS mirror (found by tests/host fuzz_web).
    return !strncmp(p, FS_ROOT "/", strlen(FS_ROOT) + 1) && !strstr(p, "..") && !strchr(p, '\\') &&
           !is_nvs_mirror(p);
}

long json_int(const char *body, const char *key, long dflt) {
    char pat[32];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(body, pat);
    if (!p) return dflt;
    p += strlen(pat);
    while (*p && (*p == ':' || *p == ' ' || *p == '\t')) p++;
    if (!*p) return dflt;
    return strtol(p, nullptr, 10);
}

bool json_str(const char *body, const char *key, char *out, size_t n) {
    if (!n) return false;
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(body, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p && *p != ':') p++;
    if (*p == ':') p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return false;
    p++;
    size_t o = 0;
    while (*p && *p != '"' && o + 1 < n) { if (*p == '\\' && p[1]) p++; out[o++] = *p++; }
    out[o] = '\0';
    return true;
}

void json_escape(char *out, size_t n, const char *src) {
    if (!n) return;
    size_t o = 0;
    for (const char *p = src; *p && o + 2 < n; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = c; }
        else if (c < 0x20) { if (o + 6 < n) o += snprintf(out + o, n - o, "\\u%04x", c); }
        else out[o++] = c;
    }
    out[o] = '\0';
}

}  // namespace nv_web_util
