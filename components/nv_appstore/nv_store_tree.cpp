// nv_store_tree — see header. Every loop is bounded by its input; nothing allocates.
#include "nv_store_tree.h"

#include <cstring>

namespace nv_store_tree {
namespace {

constexpr char kIndexDomain[] = "nucleoos-tree-v1";

int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool seg_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '.' || c == '_' || c == '-' || c == '+';
}

// One index line "<64 hex> <size> <path>" (no LF). path_out/len may be nullptr.
bool parse_line(const char *ln, size_t ll, Entry *e, const char **path, size_t *plen) {
    if (ll < 64 + 1 + 1 + 1 + 1 || ln[64] != ' ') return false;
    for (int i = 0; i < 32; i++) {
        const int a = hexv(ln[2 * i]), b = hexv(ln[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        if (e) e->sha256[i] = (uint8_t)(a << 4 | b);
    }
    size_t i = 65;
    const size_t ds = i;
    uint64_t sz = 0;
    while (i < ll && ln[i] >= '0' && ln[i] <= '9') {
        sz = sz * 10 + (uint64_t)(ln[i] - '0');
        if (sz > kFileMax) return false;
        i++;
    }
    if (i == ds || (ln[ds] == '0' && i - ds > 1)) return false;                  // canonical (0 allowed)
    if (i >= ll || ln[i] != ' ') return false;
    i++;
    if (!path_ok(ln + i, ll - i)) return false;
    if (e) {
        memcpy(e->path, ln + i, ll - i);
        e->path[ll - i] = '\0';
        e->size = sz;
    }
    if (path) *path = ln + i;
    if (plen) *plen = ll - i;
    return true;
}

// The line starting at off (without its LF).
size_t line_len(const Index &ix, uint32_t off) {
    const void *nl = memchr(ix.text + off, '\n', ix.len - off);
    return nl ? (size_t)((const char *)nl - (ix.text + off)) : 0;
}

int cmp_span(const char *a, size_t al, const char *b, size_t bl) {
    const int c = memcmp(a, b, al < bl ? al : bl);
    return c ? c : (al < bl ? -1 : (al > bl ? 1 : 0));
}

// Octal header field (NUL/space terminated, leading spaces allowed). False on base-256 or junk.
bool octal(const uint8_t *f, size_t n, uint64_t *out) {
    size_t i = 0;
    while (i < n && f[i] == ' ') i++;
    uint64_t v = 0;
    size_t digits = 0;
    for (; i < n && f[i] >= '0' && f[i] <= '7'; i++, digits++) {
        if (v >> 60) return false;
        v = v * 8 + (uint64_t)(f[i] - '0');
    }
    for (; i < n; i++) if (f[i] != ' ' && f[i] != '\0') return false;
    if (!digits) return false;
    *out = v;
    return true;
}

size_t field_len(const uint8_t *f, size_t n) {
    size_t l = 0;
    while (l < n && f[l]) l++;
    return l;
}

}  // namespace

bool path_ok(const char *p, size_t n) {
    if (!p || n == 0 || n >= (size_t)kPathMax) return false;
    int segs = 0;
    size_t seg = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i == n || p[i] == '/') {
            const size_t sl = i - seg;
            if (sl == 0) return false;                                   // leading/trailing/double '/'
            if ((sl == 1 && p[seg] == '.') || (sl == 2 && p[seg] == '.' && p[seg + 1] == '.')) return false;
            if (++segs > kDepthMax) return false;
            seg = i + 1;
        } else if (!seg_char(p[i])) {
            return false;
        }
    }
    return true;
}

bool parse_index(const char *t, size_t len, Index *out) {
    if (!t || !out || len > 512u * 1024) return false;
    out->text = t;
    out->len = len;
    out->n = 0;
    const void *nl = memchr(t, '\n', len);
    if (!nl || (size_t)((const char *)nl - t) != sizeof kIndexDomain - 1 || memcmp(t, kIndexDomain, sizeof kIndexDomain - 1))
        return false;
    size_t pos = sizeof kIndexDomain;
    const char *prev = nullptr;
    size_t prev_len = 0;
    while (pos < len) {
        const void *e = memchr(t + pos, '\n', len - pos);
        if (!e) return false;                                            // every line ends with LF
        const size_t ll = (size_t)((const char *)e - (t + pos));
        const char *path;
        size_t pl;
        if (out->n >= kIndexMax || !parse_line(t + pos, ll, nullptr, &path, &pl)) return false;
        if (prev && cmp_span(prev, prev_len, path, pl) >= 0) return false;   // strictly ascending
        out->off[out->n++] = (uint32_t)pos;
        prev = path;
        prev_len = pl;
        pos += ll + 1;
    }
    if (out->n == 0) return false;
    // A file can't also be a folder of another file ("a" and "a/b"): every folder prefix is looked up.
    char pfx[kPathMax];
    Entry e;
    for (int i = 0; i < out->n; i++) {
        if (!index_entry(*out, i, &e)) return false;
        for (size_t k = 0; e.path[k]; k++) {
            if (e.path[k] != '/') continue;
            memcpy(pfx, e.path, k);
            pfx[k] = '\0';
            if (index_find(*out, pfx) >= 0) return false;
        }
    }
    return true;
}

bool index_entry(const Index &ix, int i, Entry *out) {
    if (i < 0 || i >= ix.n || !out) return false;
    return parse_line(ix.text + ix.off[i], line_len(ix, ix.off[i]), out, nullptr, nullptr);
}

int index_find(const Index &ix, const char *path) {
    if (!path) return -1;
    const size_t want = strlen(path);
    int lo = 0, hi = ix.n - 1;
    while (lo <= hi) {
        const int mid = lo + (hi - lo) / 2;
        const char *p;
        size_t pl;
        if (!parse_line(ix.text + ix.off[mid], line_len(ix, ix.off[mid]), nullptr, &p, &pl)) return -1;
        const int c = cmp_span(p, pl, path, want);
        if (c == 0) return mid;
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

uint64_t index_total(const Index &ix) {
    uint64_t t = 0;
    Entry e;
    for (int i = 0; i < ix.n; i++) if (index_entry(ix, i, &e)) t += e.size;
    return t;
}

// ---- ustar ----

void TarReader::reset() {
    hdr_n_ = 0;
    left_ = 0;
    pad_ = 0;
    zeros_ = 0;
    in_file_ = false;
    bad_ = false;
}

bool TarReader::header(Sink &sink) {
    bool zero = true;
    for (int i = 0; i < 512 && zero; i++) zero = hdr_[i] == 0;
    if (zero) { zeros_++; return true; }
    if (zeros_) return false;                                            // data after the end marker

    uint64_t sum = 0, want = 0;
    for (int i = 0; i < 512; i++) sum += (i >= 148 && i < 156) ? ' ' : hdr_[i];
    if (!octal(hdr_ + 148, 8, &want) || want != sum) return false;

    const bool posix = !memcmp(hdr_ + 257, "ustar\0" "00", 8);
    const bool gnu   = !memcmp(hdr_ + 257, "ustar  \0", 8);
    if (!posix && !gnu) return false;

    const char type = (char)hdr_[156];
    const bool is_dir = type == '5', is_file = type == '0' || type == '\0';
    if (!is_dir && !is_file) return false;

    uint64_t size = 0;
    if (!octal(hdr_ + 124, 12, &size) || size > kFileMax) return false;
    if (is_dir && size) return false;

    char path[kPathMax + 8];
    size_t pl = 0;
    const size_t prl = posix ? field_len(hdr_ + 345, 155) : 0;
    const size_t nl = field_len(hdr_, 100);
    if (prl + 1 + nl >= sizeof path) return false;
    if (prl) { memcpy(path, hdr_ + 345, prl); path[prl] = '/'; pl = prl + 1; }
    memcpy(path + pl, hdr_, nl);
    pl += nl;
    path[pl] = '\0';
    char *p = path;
    while (p[0] == '.' && p[1] == '/') { p += 2; pl -= 2; }
    if (is_dir) {
        while (pl && p[pl - 1] == '/') p[--pl] = '\0';
        if (pl == 0 || (pl == 1 && p[0] == '.')) return true;            // the root itself
    }
    if (!path_ok(p, pl)) return false;

    if (is_dir) return sink.dir(p);
    if (!sink.begin(p, size)) return false;
    pad_ = (uint32_t)((512 - size % 512) % 512);
    if (!size) return sink.end();
    left_ = size;
    in_file_ = true;
    return true;
}

bool TarReader::feed(const uint8_t *p, size_t n, Sink &sink) {
    while (n && !bad_) {
        if (in_file_) {
            const size_t k = (uint64_t)n < left_ ? n : (size_t)left_;
            if (!sink.data(p, k)) { bad_ = true; break; }
            p += k; n -= k; left_ -= k;
            if (!left_) { in_file_ = false; if (!sink.end()) { bad_ = true; break; } }
            continue;
        }
        if (pad_) {
            const size_t k = n < pad_ ? n : pad_;
            for (size_t i = 0; i < k; i++) if (p[i]) { bad_ = true; break; }   // padding is zeros
            p += k; n -= k; pad_ -= (uint32_t)k;
            continue;
        }
        const size_t k = n < 512 - hdr_n_ ? n : 512 - hdr_n_;
        memcpy(hdr_ + hdr_n_, p, k);
        hdr_n_ += k; p += k; n -= k;
        if (hdr_n_ == 512) {
            hdr_n_ = 0;
            if (!header(sink)) bad_ = true;
        }
    }
    return !bad_;
}

bool TarReader::finished() const {
    return !bad_ && !in_file_ && !pad_ && hdr_n_ == 0 && zeros_ >= 2;
}

// ---- swap recovery ----

unsigned swap_recover(bool journal, bool dir, bool dir_new, bool dir_old) {
    if (journal) {
        if (dir_new) {
            if (dir) return (dir_old ? SWAP_RM_STALE : 0u) | SWAP_DIR_TO_OLD | SWAP_NEW_TO_DIR | SWAP_RM_OLD;
            return SWAP_NEW_TO_DIR | (dir_old ? SWAP_RM_OLD : 0u);
        }
        if (dir) return dir_old ? SWAP_RM_OLD : 0u;
        return dir_old ? SWAP_OLD_TO_DIR : 0u;
    }
    return (dir_new ? SWAP_RM_NEW : 0u) | (dir_old ? (dir ? SWAP_RM_OLD : SWAP_OLD_TO_DIR) : 0u);
}

}  // namespace nv_store_tree
