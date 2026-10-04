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

namespace {
constexpr char kDataDomain[] = "nucleoos-data-v1";

bool name_ok(const char *s, size_t n) {
    if (n == 0 || n >= (size_t)kDataNameMax) return false;
    if ((n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.')) return false;
    for (size_t i = 0; i < n; i++) if (!seg_char(s[i])) return false;
    return true;
}

bool url_ok(const char *s, size_t n) {
    if (n <= 8 || n >= (size_t)kDataUrlMax || memcmp(s, "https://", 8) != 0) return false;
    for (size_t i = 0; i < n; i++) if (s[i] <= ' ' || s[i] > '~') return false;   // printable, no space
    return true;
}
}  // namespace

bool parse_data(const char *t, size_t len, DataPack *out) {
    if (!t || !out || len > kTextMax) return false;
    memset(out, 0, sizeof *out);
    size_t pos = 0;
    const char *ln;
    size_t ll;
    if (!next_line(t, len, &pos, &ln, &ll) || ll != sizeof kDataDomain - 1 || memcmp(ln, kDataDomain, ll))
        return false;
    if (!next_line(t, len, &pos, &ln, &ll) || !id_ok(ln, ll)) return false;
    memcpy(out->id, ln, ll);
    if (!next_line(t, len, &pos, &ln, &ll) || !version_ok(ln, ll)) return false;
    memcpy(out->version, ln, ll);
    if (!next_line(t, len, &pos, &ln, &ll) || ll == 0 || ll >= sizeof out->dest) return false;
    bool dest_ok = false;
    for (int i = 0; kDataDests[i]; i++)
        if (strlen(kDataDests[i]) == ll && !memcmp(kDataDests[i], ln, ll)) dest_ok = true;
    if (!dest_ok) return false;
    memcpy(out->dest, ln, ll);

    uint64_t file_sum = 0;                                    // bytes of the file being listed
    for (;;) {
        const size_t line_start = pos;
        if (!next_line(t, len, &pos, &ln, &ll)) return false;
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
            return pos == len && out->n > 0;
        }
        // "<64 hex> <size> <name> <url>"
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
        const size_t ns = ++i;
        while (i < ll && ln[i] != ' ') i++;
        if (i >= ll || !name_ok(ln + ns, i - ns)) return false;
        memcpy(d.name, ln + ns, i - ns);
        const size_t us = ++i;
        if (!url_ok(ln + us, ll - us)) return false;
        memcpy(d.url, ln + us, ll - us);
        d.size = sz;
        const bool same = out->n > 0 && !strcmp(out->parts[out->n - 1].name, d.name);
        if (!same) {                                            // a new file: its name must be new too
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
