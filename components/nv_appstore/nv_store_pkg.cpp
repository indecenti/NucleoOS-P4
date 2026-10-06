// nv_store_pkg — see header. Every loop is bounded by `len`; nothing allocates.
#include "nv_store_pkg.h"

#include <cstring>

namespace nv_store_pkg {
namespace {

constexpr char kDomain[] = "nucleoos-app-v1";

int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;                                    // uppercase refused: one canonical encoding
}

// Next LF-terminated line [*pos, eol) of text; false at end or on a missing LF.
bool next_line(const char *t, size_t len, size_t *pos, const char **ln, size_t *ll) {
    if (*pos >= len) return false;
    const char *s = t + *pos;
    const void *nl = memchr(s, '\n', len - *pos);
    if (!nl) return false;
    *ln = s;
    *ll = (size_t)((const char *)nl - s);
    *pos += *ll + 1;
    return true;
}

bool id_ok(const char *s, size_t n) {
    if (n == 0 || n > 31) return false;
    for (size_t i = 0; i < n; i++) {
        const char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_'))
            return false;
    }
    return true;
}

bool version_ok(const char *s, size_t n) {
    if (n == 0 || n > 15) return false;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || s[i] == '.')) return false;
    return true;
}

bool seg_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '.' || c == '_' || c == '-';
}

}  // namespace

bool path_ok(const char *p) {
    if (!p) return false;
    const size_t n = strnlen(p, kPathMax);
    if (n == 0 || n >= (size_t)kPathMax) return false;
    int slashes = 0;
    size_t seg = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i == n || p[i] == '/') {
            const size_t sl = i - seg;
            if (sl == 0) return false;                               // leading '/', "//", trailing '/'
            if ((sl == 1 && p[seg] == '.') || (sl == 2 && p[seg] == '.' && p[seg + 1] == '.'))
                return false;
            if (i < n && ++slashes > 2) return false;
            seg = i + 1;
        } else if (!seg_char(p[i])) {
            return false;
        }
    }
    return true;
}

bool parse(const char *t, size_t len, Package *out) {
    if (!t || !out || len > kTextMax) return false;
    memset(out, 0, sizeof *out);
    size_t pos = 0;
    const char *ln;
    size_t ll;

    if (!next_line(t, len, &pos, &ln, &ll) || ll != sizeof kDomain - 1 || memcmp(ln, kDomain, ll))
        return false;
    if (!next_line(t, len, &pos, &ln, &ll) || !id_ok(ln, ll)) return false;
    memcpy(out->id, ln, ll);
    if (!next_line(t, len, &pos, &ln, &ll) || !version_ok(ln, ll)) return false;
    memcpy(out->version, ln, ll);

    for (;;) {
        const size_t line_start = pos;
        if (!next_line(t, len, &pos, &ln, &ll)) return false;       // no "sig" line
        if (ll >= 4 && memcmp(ln, "sig ", 4) == 0) {
            out->signed_len = line_start;
            const size_t hl = ll - 4;
            if (hl < 16 || hl > (size_t)kSigMax * 2 || (hl & 1)) return false;
            for (size_t i = 0; i < hl; i += 2) {
                const int a = hexv(ln[4 + i]), b = hexv(ln[4 + i + 1]);
                if (a < 0 || b < 0) return false;
                out->sig[i / 2] = (uint8_t)(a << 4 | b);
            }
            out->sig_len = (int)(hl / 2);
            return pos == len && out->n_files > 0;                   // nothing after the sig line
        }
        // "<64 hex> <size> <path>"
        if (out->n_files >= kMaxFiles || ll < 64 + 1 + 1 + 1 + 1 || ln[64] != ' ') return false;
        File &f = out->files[out->n_files];
        for (int i = 0; i < 32; i++) {
            const int a = hexv(ln[2 * i]), b = hexv(ln[2 * i + 1]);
            if (a < 0 || b < 0) return false;
            f.sha256[i] = (uint8_t)(a << 4 | b);
        }
        size_t i = 65;
        uint64_t sz = 0;
        const size_t ds = i;
        while (i < ll && ln[i] >= '0' && ln[i] <= '9') {
            sz = sz * 10 + (uint64_t)(ln[i] - '0');
            if (sz > kFileMax) return false;
            i++;
        }
        if (i == ds || i - ds > 1 + 8 || (ln[ds] == '0' && i - ds > 1)) return false;  // canonical
        if (i >= ll || ln[i] != ' ') return false;
        i++;
        const size_t pl = ll - i;
        if (pl == 0 || pl >= (size_t)kPathMax) return false;
        memcpy(f.path, ln + i, pl);
        f.path[pl] = '\0';
        if (strlen(f.path) != pl || !path_ok(f.path)) return false;
        if (out->n_files > 0 && strcmp(out->files[out->n_files - 1].path, f.path) >= 0) return false;
        f.size = (uint32_t)sz;
        out->n_files++;
    }
}

const char *const kDataDests[] = { "anima/kb", "anima", nullptr };
const char *const kContentDests[] = { "web", "data/anima", "data/anima/kb", "data/tts", "nucleos/drivers", nullptr };

namespace {
constexpr char kDataDomain[]   = "nucleoos-data-v1";
constexpr char kDataDomainV2[] = "nucleoos-data-v2";
constexpr size_t kV1NameMax    = 48;                 // v1's limit (incl. NUL), as shipped since 1.2.20

bool name_ok(const char *s, size_t n) {
    if (n == 0 || n >= kV1NameMax) return false;
    if ((n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.')) return false;
    for (size_t i = 0; i < n; i++) if (!seg_char(s[i])) return false;
    return true;
}

bool ends_with(const char *s, const char *suf) {
    const size_t a = strlen(s), b = strlen(suf);
    return a >= b && !memcmp(s + a - b, suf, b);
}

// A v2 entry name: a relative path (path_ok) or "." for a tree; never an installer staging name.
bool name_ok_v2(const char *s, char kind) {
    if (!strcmp(s, ".")) return kind != 'f';
    if (!path_ok(s)) return false;
    return !ends_with(s, ".part") && !ends_with(s, ".new") && !ends_with(s, ".old") && !ends_with(s, ".tmp");
}

// Two entry names overlap when equal or when one is a folder holding the other ("." holds everything).
bool overlaps(const char *a, const char *b) {
    if (!strcmp(a, ".") || !strcmp(b, ".")) return true;
    const size_t la = strlen(a), lb = strlen(b);
    if (la == lb) return !strcmp(a, b);
    const char *s = la < lb ? a : b, *l = la < lb ? b : a;
    const size_t ls = la < lb ? la : lb;
    return !memcmp(s, l, ls) && l[ls] == '/';
}

bool url_ok(const char *s, size_t n) {
    if (n <= 8 || n >= (size_t)kDataUrlMax || memcmp(s, "https://", 8) != 0) return false;
    for (size_t i = 0; i < n; i++) if (s[i] <= ' ' || s[i] > '~') return false;   // printable, no space
    return true;
}

bool sig_line(const char *ln, size_t ll, DataPack *out) {
    const size_t hl = ll - 4;
    if (hl < 16 || hl > (size_t)kSigMax * 2 || (hl & 1)) return false;
    for (size_t i = 0; i < hl; i += 2) {
        const int a = hexv(ln[4 + i]), b = hexv(ln[4 + i + 1]);
        if (a < 0 || b < 0) return false;
        out->sig[i / 2] = (uint8_t)(a << 4 | b);
    }
    out->sig_len = (int)(hl / 2);
    return true;
}

// The v2 rules that span lines, checked once every line is parsed. A group is a file's parts, or an
// index and its tree; distinct groups never overlap.
bool v2_structure_ok(const DataPack &p) {
    for (int i = 0; i < p.n; i++) {
        const DataPart &q = p.parts[i];
        if (q.kind == 'i' && (i + 1 >= p.n || (p.parts[i + 1].kind != 't' && p.parts[i + 1].kind != 'u') ||
                              strcmp(p.parts[i + 1].name, q.name) || q.size > kTreeIndexMax))
            return false;
        if ((q.kind == 't' || q.kind == 'u') && (i == 0 || p.parts[i - 1].kind != 'i')) return false;
        const bool same = i > 0 && !strcmp(p.parts[i - 1].name, q.name);
        const bool cont = same && ((q.kind == 'f' && p.parts[i - 1].kind == 'f') ||
                                   (q.kind != 'f' && q.kind != 'i' && p.parts[i - 1].kind == 'i'));
        if (same && !cont) return false;
        if (cont) continue;
        for (int k = 0; k < i; k++) if (overlaps(p.parts[k].name, q.name)) return false;
    }
    return true;
}
}  // namespace

bool parse_data(const char *t, size_t len, DataPack *out) {
    if (!t || !out || len > kTextMax) return false;
    memset(out, 0, sizeof *out);
    size_t pos = 0;
    const char *ln;
    size_t ll;
    if (!next_line(t, len, &pos, &ln, &ll)) return false;
    if (ll == sizeof kDataDomain - 1 && !memcmp(ln, kDataDomain, ll)) out->format = 1;
    else if (ll == sizeof kDataDomainV2 - 1 && !memcmp(ln, kDataDomainV2, ll)) out->format = 2;
    else return false;
    const bool v2 = out->format == 2;
    if (!next_line(t, len, &pos, &ln, &ll) || !id_ok(ln, ll)) return false;
    memcpy(out->id, ln, ll);
    if (!next_line(t, len, &pos, &ln, &ll) || !version_ok(ln, ll)) return false;
    memcpy(out->version, ln, ll);
    if (!next_line(t, len, &pos, &ln, &ll) || ll == 0 || ll >= sizeof out->dest) return false;
    bool dest_ok = false;
    const char *const *dests = v2 ? kContentDests : kDataDests;
    for (int i = 0; dests[i]; i++)
        if (strlen(dests[i]) == ll && !memcmp(dests[i], ln, ll)) dest_ok = true;
    if (!dest_ok) return false;
    memcpy(out->dest, ln, ll);

    uint64_t file_sum = 0;                                    // bytes of the file being listed
    for (;;) {
        const size_t line_start = pos;
        if (!next_line(t, len, &pos, &ln, &ll)) return false;
        if (ll >= 4 && memcmp(ln, "sig ", 4) == 0) {
            out->signed_len = line_start;
            if (!sig_line(ln, ll, out)) return false;
            return pos == len && out->n > 0 && (!v2 || v2_structure_ok(*out));
        }
        // v1 "<64 hex> <size> <name> <url>", v2 "<64 hex> <size> <kind> <name> <url> [<url2>]"
        if (out->n >= kDataMax || ll < 64 + 1 + 1 + 1 + 1 + 1 + 9 || ln[64] != ' ') return false;
        DataPart &d = out->parts[out->n];
        for (int i = 0; i < 32; i++) {
            const int a = hexv(ln[2 * i]), b = hexv(ln[2 * i + 1]);
            if (a < 0 || b < 0) return false;
            d.sha256[i] = (uint8_t)(a << 4 | b);
        }
        size_t i = 65;
        const size_t ds = i;
        uint64_t sz = 0;
        while (i < ll && ln[i] >= '0' && ln[i] <= '9') {
            sz = sz * 10 + (uint64_t)(ln[i] - '0');
            if (sz > kDataFileMax) return false;
            i++;
        }
        if (i == ds || (ln[ds] == '0' && i - ds > 1) || sz == 0) return false;     // canonical, non-empty
        if (i >= ll || ln[i] != ' ') return false;
        i++;
        d.kind = 'f';
        if (v2) {
            if (i + 1 >= ll || ln[i + 1] != ' ') return false;
            d.kind = ln[i];
            if (d.kind != 'f' && d.kind != 'i' && d.kind != 't' && d.kind != 'u') return false;
            i += 2;
        }
        const size_t ns = i;
        while (i < ll && ln[i] != ' ') i++;
        if (i >= ll || i - ns >= (size_t)kDataNameMax) return false;
        memcpy(d.name, ln + ns, i - ns);
        if (v2 ? !name_ok_v2(d.name, d.kind) : !name_ok(ln + ns, i - ns)) return false;
        const size_t us = ++i;
        while (i < ll && ln[i] != ' ') i++;
        if (!url_ok(ln + us, i - us)) return false;
        memcpy(d.url, ln + us, i - us);
        if (i < ll) {                                           // a mirror: v2 only, exactly one
            const size_t ms = ++i;
            if (!v2 || !url_ok(ln + ms, ll - ms)) return false;
            memcpy(d.url2, ln + ms, ll - ms);
        }
        d.size = sz;
        const DataPart *prev = out->n > 0 ? &out->parts[out->n - 1] : nullptr;
        const bool part_of_prev = prev && !strcmp(prev->name, d.name) && d.kind == 'f' && prev->kind == 'f';
        if (!part_of_prev) {                                    // a new file (v2: names checked at the end)
            if (!v2)
                for (int k = 0; k < out->n; k++) if (!strcmp(out->parts[k].name, d.name)) return false;
            file_sum = 0;
        }
        file_sum += sz;
        if (file_sum > kDataFileMax) return false;
        out->n++;
    }
}

uint64_t data_total(const DataPack &p) {
    uint64_t t = 0;
    for (int i = 0; i < p.n; i++) t += p.parts[i].size;
    return t;
}

const File *find(const Package &p, const char *path) {
    if (!path) return nullptr;
    for (int i = 0; i < p.n_files; i++)
        if (strcmp(p.files[i].path, path) == 0) return &p.files[i];
    return nullptr;
}

}  // namespace nv_store_pkg
