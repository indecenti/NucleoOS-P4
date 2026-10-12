// nv_dl_core — see nv_dl_core.h. Pure C99, no allocation: everything it reads comes from the network or
// the card, so every scan is bounded by the end pointer it is given (fuzzed in tests/host).
#include "nv_dl_core.h"

#include <stdio.h>
#include <string.h>

// ---- small helpers --------------------------------------------------------------------------------

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool hex_span(const char *s, size_t len, uint8_t *out, size_t n) {
    if (len != 2 * n) return false;
    for (size_t i = 0; i < n; i++) {
        const int a = hexval(s[2 * i]), b = hexval(s[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        out[i] = (uint8_t)(a << 4 | b);
    }
    return true;
}

bool nvdl_hex_decode(const char *hex, uint8_t *out, size_t n) {
    return hex && out && hex_span(hex, strlen(hex), out, n);
}

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static void trim(const char **s, const char **e) {
    while (*s < *e && is_space(**s)) (*s)++;
    while (*e > *s && is_space((*e)[-1])) (*e)--;
}

// Decimal [s, e) -> *v. False on anything else, an empty span or an overflow past `max`.
static bool parse_u64(const char *s, const char *e, uint64_t max, uint64_t *v) {
    trim(&s, &e);
    if (s == e) return false;
    uint64_t x = 0;
    for (; s < e; s++) {
        if (*s < '0' || *s > '9') return false;
        const unsigned d = (unsigned)(*s - '0');
        if (x > (max - d) / 10) return false;
        x = x * 10 + d;
    }
    *v = x;
    return true;
}

static const char *find(const char *p, const char *e, const char *needle) {
    const size_t k = strlen(needle);
    for (; p && (size_t)(e - p) >= k; p++)
        if (!memcmp(p, needle, k)) return p;
    return NULL;
}

static bool starts(const char *p, const char *e, const char *s) {
    const size_t k = strlen(s);
    return (size_t)(e - p) >= k && !memcmp(p, s, k);
}

static bool name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ':' || c == '_' ||
           c == '-' || c == '.';
}

// The '>' that ends the tag opened at lt, skipping quoted attribute values. NULL if none.
static const char *tag_end(const char *lt, const char *e) {
    char q = 0;
    for (const char *p = lt + 1; p < e; p++) {
        if (q) { if (*p == q) q = 0; }
        else if (*p == '"' || *p == '\'') q = *p;
        else if (*p == '>') return p;
    }
    return NULL;
}

// Local name of the element (namespace prefix dropped) equals `want`.
static bool name_is(const char *n, size_t len, const char *want) {
    const char *c = memchr(n, ':', len);
    if (c) { len -= (size_t)(c + 1 - n); n = c + 1; }
    return strlen(want) == len && !memcmp(n, want, len);
}

// Value span of attribute `want` in the tag [s, e). False when absent.
static bool attr(const char *s, const char *e, const char *want, const char **vs, const char **ve) {
    const size_t wl = strlen(want);
    for (const char *p = s; p < e; p++) {
        if (!is_space(*p)) continue;
        const char *a = p + 1;
        while (a < e && is_space(*a)) a++;
        const char *ae = a;
        while (ae < e && name_char(*ae)) ae++;
        const char *q = ae;
        while (q < e && is_space(*q)) q++;
        if (q >= e || *q != '=') continue;
        q++;
        while (q < e && is_space(*q)) q++;
        if (q >= e || (*q != '"' && *q != '\'')) continue;
        const char quote = *q++;
        const char *end = memchr(q, quote, (size_t)(e - q));
        if (!end) return false;
        if ((size_t)(ae - a) == wl && !memcmp(a, want, wl)) { *vs = q; *ve = end; return true; }
        p = end;
    }
    return false;
}

// Character data [s, e) with the XML entities expanded into out (NUL-terminated). False when an entity
// is unknown, the result holds a control character or a space, or it doesn't fit.
static bool text_to(const char *s, const char *e, char *out, size_t cap) {
    trim(&s, &e);
    size_t o = 0;
    while (s < e) {
        unsigned c = (unsigned char)*s;
        if (c == '&') {
            const char *semi = memchr(s, ';', (size_t)(e - s));
            if (!semi || semi - s > 10) return false;
            const size_t k = (size_t)(semi - s - 1);
            const char *ent = s + 1;
            if (k == 3 && !memcmp(ent, "amp", 3)) c = '&';
            else if (k == 2 && !memcmp(ent, "lt", 2)) c = '<';
            else if (k == 2 && !memcmp(ent, "gt", 2)) c = '>';
            else if (k == 4 && !memcmp(ent, "quot", 4)) c = '"';
            else if (k == 4 && !memcmp(ent, "apos", 4)) c = '\'';
            else if (k >= 2 && ent[0] == '#') {
                unsigned v = 0;
                const bool hex = ent[1] == 'x' || ent[1] == 'X';
                for (const char *d = ent + (hex ? 2 : 1); d < semi; d++) {
                    const int h = hex ? hexval(*d) : (*d >= '0' && *d <= '9' ? *d - '0' : -1);
                    if (h < 0 || v > 0x7f) return false;
                    v = v * (hex ? 16u : 10u) + (unsigned)h;
                }
                if (v == 0 || v > 0x7e) return false;     // ASCII only: enough for URLs
                c = v;
            } else {
                return false;
            }
            s = semi + 1;
        } else {
            s++;
        }
        if (c <= ' ' || c == 0x7f) return false;
        if (o + 1 >= cap) return false;
        out[o++] = (char)c;
    }
    out[o] = 0;
    return o > 0;
}

static bool https_url(const char *u) {
    static const char pre[] = "https://";
    for (size_t i = 0; i < sizeof pre - 1; i++)
        if (u[i] == 0 || (u[i] | 0x20) != pre[i]) return false;
    return u[sizeof pre - 1] != 0 && u[sizeof pre - 1] != '/';
}

// Keep the NVDL_URLS_MAX best (lowest) priorities, earlier ones first among equals.
static void add_url(nvdl_meta4_t *m, uint32_t *prio, uint32_t p, const char *url) {
    for (int i = 0; i < m->n_urls; i++) if (!strcmp(m->urls[i], url)) return;
    int at = m->n_urls;
    while (at > 0 && prio[at - 1] > p) at--;
    if (at >= NVDL_URLS_MAX) return;
    const int last = m->n_urls < NVDL_URLS_MAX ? m->n_urls : NVDL_URLS_MAX - 1;
    for (int i = last; i > at; i--) {
        memcpy(m->urls[i], m->urls[i - 1], NVDL_URL_MAX);
        prio[i] = prio[i - 1];
    }
    snprintf(m->urls[at], NVDL_URL_MAX, "%s", url);
    prio[at] = p;
    if (m->n_urls < NVDL_URLS_MAX) m->n_urls++;
}

// ---- Metalink 4 -----------------------------------------------------------------------------------

int nvdl_meta4_parse(const char *xml, size_t n, nvdl_meta4_t *out, uint8_t *pieces, size_t pieces_cap) {
    if (!out) return NVDL_EPARSE;
    memset(out, 0, sizeof *out);
    if (!xml) return NVDL_EPARSE;
    const char *p = xml, *e = xml + n;
    int in_file = 0;                 // 0 before the first <file>, 1 inside, 2 after it
    bool in_pieces = false, pieces_ok = false, size_seen = false, bad_pieces = false;
    uint32_t got = 0;
    uint32_t prio[NVDL_URLS_MAX] = {0};
    char url[NVDL_URL_MAX];
    while (p < e && in_file < 2) {
        const char *lt = memchr(p, '<', (size_t)(e - p));
        if (!lt) break;
        if (starts(lt, e, "<!--")) { const char *q = find(lt + 4, e, "-->"); if (!q) break; p = q + 3; continue; }
        if (starts(lt, e, "<?"))   { const char *q = find(lt + 2, e, "?>");  if (!q) break; p = q + 2; continue; }
        if (starts(lt, e, "<!"))   { const char *q = memchr(lt, '>', (size_t)(e - lt)); if (!q) break; p = q + 1; continue; }
        const char *gt = tag_end(lt, e);
        if (!gt) break;
        const bool closing = lt + 1 < gt && lt[1] == '/';
        const char *nm = lt + 1 + (closing ? 1 : 0);
        const char *ne = nm;
        while (ne < gt && name_char(*ne)) ne++;
        const size_t nl = (size_t)(ne - nm);
        const bool selfclose = gt[-1] == '/';
        const char *ts = gt + 1;
        const char *te = memchr(ts, '<', (size_t)(e - ts));
        if (!te) te = e;
        const char *vs, *ve;
        if (!closing) {
            if (name_is(nm, nl, "file")) {
                if (in_file == 0) {
                    in_file = 1;
                    if (attr(ne, gt, "name", &vs, &ve)) {
                        size_t o = 0;
                        for (const char *c = vs; c < ve && o + 1 < sizeof out->name; c++)
                            out->name[o++] = ((unsigned char)*c < ' ' || *c == '/' || *c == '\\') ? '_' : *c;
                        out->name[o] = 0;
                    }
                }
            } else if (in_file == 1 && name_is(nm, nl, "size") && !size_seen) {
                if (!parse_u64(ts, te, NVDL_SIZE_MAX, &out->size)) return NVDL_EPARSE;
                size_seen = true;
            } else if (in_file == 1 && name_is(nm, nl, "pieces") && !selfclose) {
                in_pieces = true;
                pieces_ok = false;
                uint64_t len = 0;
                if (out->piece_len == 0 && attr(ne, gt, "type", &vs, &ve) && ve - vs == 5 && !memcmp(vs, "sha-1", 5) &&
                    attr(ne, gt, "length", &vs, &ve) && parse_u64(vs, ve, NVDL_PIECE_MAX, &len) && len >= NVDL_PIECE_MIN) {
                    out->piece_len = (uint32_t)len;
                    pieces_ok = true;
                }
            } else if (in_file == 1 && name_is(nm, nl, "hash")) {
                const char *hs = ts, *he = te;
                trim(&hs, &he);
                if (in_pieces) {
                    if (pieces_ok) {
                        uint8_t h[20];
                        if (!hex_span(hs, (size_t)(he - hs), h, 20) || got >= 0x7fffffffu) bad_pieces = true;
                        else {
                            if (pieces) {
                                if ((size_t)(got + 1) * 20 > pieces_cap) return NVDL_ETOOBIG;
                                memcpy(pieces + (size_t)got * 20, h, 20);
                            }
                            got++;
                        }
                    }
                } else if (attr(ne, gt, "type", &vs, &ve) && ve - vs == 7 && !memcmp(vs, "sha-256", 7) && !out->has_sha256) {
                    if (!hex_span(hs, (size_t)(he - hs), out->sha256, 32)) return NVDL_EPARSE;
                    out->has_sha256 = true;
                }
            } else if (in_file == 1 && name_is(nm, nl, "url") && !selfclose) {
                uint64_t pr = 999999;
                if (attr(ne, gt, "priority", &vs, &ve) && !parse_u64(vs, ve, 999999, &pr)) pr = 999999;
                if (text_to(ts, te, url, sizeof url) && https_url(url)) add_url(out, prio, (uint32_t)pr, url);
            }
        } else {
            if (name_is(nm, nl, "pieces")) in_pieces = false;
            else if (name_is(nm, nl, "file") && in_file == 1) in_file = 2;
        }
        p = gt + 1;
    }
    if (in_file != 2 || !size_seen || out->size == 0) return NVDL_EPARSE;
    if (out->piece_len) {
        const uint64_t want = (out->size + out->piece_len - 1) / out->piece_len;
        if (bad_pieces || got != want) return NVDL_EPIECES;
        out->n_pieces = got;
    }
    if (!out->n_urls) return NVDL_ENOURL;
    return NVDL_OK;
}

// ---- job record -----------------------------------------------------------------------------------

static const char *const kStates[] = {"queued", "paused", "done", "failed"};

static bool clean(const char *s) {
    for (; *s; s++) if ((unsigned char)*s < ' ' || *s == 0x7f) return false;
    return true;
}

typedef struct { char *b; size_t cap, len; bool over; } outbuf_t;

static void put(outbuf_t *o, const char *fmt, const char *key, const char *s) {
    if (o->over) return;
    const int k = snprintf(o->b + o->len, o->cap - o->len, fmt, key, s);
    if (k < 0 || (size_t)k >= o->cap - o->len) o->over = true;
    else o->len += (size_t)k;
}
static void put_u(outbuf_t *o, const char *key, uint64_t v) {
    char n[24];
    snprintf(n, sizeof n, "%llu", (unsigned long long)v);
    put(o, "%s %s\n", key, n);
}
// Free text: line breaks and other controls become spaces.
static void put_text(outbuf_t *o, const char *key, const char *s) {
    char t[NVDL_URL_MAX];
    size_t i = 0;
    for (; s[i] && i + 1 < sizeof t; i++) t[i] = ((unsigned char)s[i] < ' ' || s[i] == 0x7f) ? ' ' : s[i];
    t[i] = 0;
    put(o, "%s %s\n", key, t);
}

int nvdl_job_format(const nvdl_job_t *j, char *buf, size_t cap) {
    if (!j || !buf || !cap || j->state > NVDL_FAILED) return -1;
    if (!clean(j->owner) || !clean(j->dest) || !clean(j->src)) return -1;
    for (int i = 0; i < j->n_urls; i++) if (!clean(j->urls[i])) return -1;
    outbuf_t o = {buf, cap, 0, false};
    put(&o, "%s%s\n", "nvdl 1", "");
    put_u(&o, "id", j->id);
    put(&o, "%s %s\n", "owner", j->owner);
    put_text(&o, "title", j->title);
    put(&o, "%s %s\n", "dest", j->dest);
    put(&o, "%s %s\n", "src", j->src);
    put_u(&o, "meta4", j->src_meta4);
    put_u(&o, "prepared", j->prepared);
    put_u(&o, "size", j->size);
    if (j->has_sha256) {
        char h[65];
        for (int i = 0; i < 32; i++) snprintf(h + 2 * i, 3, "%02x", j->sha256[i]);
        put(&o, "%s %s\n", "sha256", h);
    }
    put_u(&o, "piece_len", j->piece_len);
    put_u(&o, "pieces", j->n_pieces);
    put_u(&o, "verified", j->verified);
    put(&o, "%s %s\n", "state", kStates[j->state]);
    put_u(&o, "created", j->created);
    put_text(&o, "error", j->error);
    for (int i = 0; i < j->n_urls && i < NVDL_URLS_MAX; i++) put(&o, "%s %s\n", "url", j->urls[i]);
    return o.over ? -1 : (int)o.len;
}

static bool copy_val(char *dst, size_t cap, const char *s, const char *e) {
    const size_t k = (size_t)(e - s);
    if (k >= cap) return false;
    memcpy(dst, s, k);
    dst[k] = 0;
    return true;
}

bool nvdl_job_parse(const char *text, size_t n, nvdl_job_t *j) {
    if (!text || !j) return false;
    memset(j, 0, sizeof *j);
    const char *p = text, *e = text + n;
    bool first = true, have_id = false, have_state = false;
    while (p < e) {
        const char *le = memchr(p, '\n', (size_t)(e - p));
        if (!le) le = e;
        const char *ls = p, *lend = le;
        p = le + (le < e ? 1 : 0);
        if (lend > ls && lend[-1] == '\r') lend--;
        if (lend - ls > NVDL_URL_MAX + 16) return false;
        for (const char *c = ls; c < lend; c++) if ((unsigned char)*c < ' ' || *c == 0x7f) return false;   // as written
        if (first) {
            if (lend - ls != 6 || memcmp(ls, "nvdl 1", 6)) return false;
            first = false;
            continue;
        }
        if (ls == lend) continue;
        const char *sp = memchr(ls, ' ', (size_t)(lend - ls));
        const char *ke = sp ? sp : lend, *vs = sp ? sp + 1 : lend;
        const size_t kl = (size_t)(ke - ls);
        uint64_t v = 0;
#define KEY(s) (kl == sizeof(s) - 1 && !memcmp(ls, s, kl))
        if (KEY("id")) { if (!parse_u64(vs, lend, 0xffffffffu, &v) || !v) return false; j->id = (uint32_t)v; have_id = true; }
        else if (KEY("owner")) { if (!copy_val(j->owner, sizeof j->owner, vs, lend)) return false; }
        else if (KEY("title")) { if (!copy_val(j->title, sizeof j->title, vs, lend)) return false; }
        else if (KEY("dest")) { if (!copy_val(j->dest, sizeof j->dest, vs, lend)) return false; }
        else if (KEY("src")) { if (!copy_val(j->src, sizeof j->src, vs, lend)) return false; }
        else if (KEY("error")) { if (!copy_val(j->error, sizeof j->error, vs, lend)) return false; }
        else if (KEY("meta4")) { if (!parse_u64(vs, lend, 1, &v)) return false; j->src_meta4 = v; }
        else if (KEY("prepared")) { if (!parse_u64(vs, lend, 1, &v)) return false; j->prepared = v; }
        else if (KEY("size")) { if (!parse_u64(vs, lend, NVDL_SIZE_MAX, &j->size)) return false; }
        else if (KEY("sha256")) { if (!hex_span(vs, (size_t)(lend - vs), j->sha256, 32)) return false; j->has_sha256 = true; }
        else if (KEY("piece_len")) { if (!parse_u64(vs, lend, NVDL_PIECE_MAX, &v)) return false; j->piece_len = (uint32_t)v; }
        else if (KEY("pieces")) { if (!parse_u64(vs, lend, 0x7fffffffu, &v)) return false; j->n_pieces = (uint32_t)v; }
        else if (KEY("verified")) { if (!parse_u64(vs, lend, NVDL_SIZE_MAX, &j->verified)) return false; }
        else if (KEY("created")) { if (!parse_u64(vs, lend, 0xffffffffu, &v)) return false; j->created = (uint32_t)v; }
        else if (KEY("state")) {
            int s = -1;
            for (int i = 0; i < 4; i++)
                if ((size_t)(lend - vs) == strlen(kStates[i]) && !memcmp(vs, kStates[i], (size_t)(lend - vs))) s = i;
            if (s < 0) return false;
            j->state = (nvdl_state_t)s;
            have_state = true;
        } else if (KEY("url")) {
            if (j->n_urls >= NVDL_URLS_MAX || !copy_val(j->urls[j->n_urls], NVDL_URL_MAX, vs, lend)) return false;
            if (!https_url(j->urls[j->n_urls])) return false;
            j->n_urls++;
        }
#undef KEY
    }
    if (first || !have_id || !have_state || !j->owner[0] || j->dest[0] != '/' || !j->src[0]) return false;
    if (j->prepared) {
        if (!j->size || !j->n_urls) return false;
        if (j->piece_len) {
            if (j->piece_len < NVDL_PIECE_MIN) return false;
            if (j->n_pieces != (j->size + j->piece_len - 1) / j->piece_len) return false;
        } else if (j->n_pieces) {
            return false;
        }
        if (j->verified > j->size) return false;
        if (j->state == NVDL_DONE && j->verified != j->size) return false;
    } else if (j->verified || j->state == NVDL_DONE) {
        return false;
    }
    return true;
}

uint64_t nvdl_resume_offset(const nvdl_job_t *j) {
    if (!j || !j->prepared) return 0;
    if (j->verified >= j->size) return j->size;
    const uint64_t unit = j->piece_len ? j->piece_len : NVDL_CHECKPOINT;
    return j->verified / unit * unit;
}

// ---- files ----------------------------------------------------------------------------------------

uint32_t nvdl_parts(uint64_t size) {
    if (size <= NVDL_PART_SIZE) return 1;
    const uint64_t k = (size + NVDL_PART_SIZE - 1) / NVDL_PART_SIZE;
    return k > NVDL_PARTS_MAX ? 0 : (uint32_t)k;
}

bool nvdl_part_name(const char *dest, uint32_t i, uint32_t nparts, char *out, size_t cap) {
    if (!dest || !out || !nparts || nparts > NVDL_PARTS_MAX || i >= nparts) return false;
    const size_t dl = strlen(dest);
    if (nparts == 1) {
        if (dl + 1 > cap) return false;
        memcpy(out, dest, dl + 1);
        return true;
    }
    if (dl + 3 > cap) return false;
    memcpy(out, dest, dl);
    out[dl] = (char)('a' + i / 26);
    out[dl + 1] = (char)('a' + i % 26);
    out[dl + 2] = 0;
    return true;
}

bool nvdl_guest_path(const char *root, const char *guest, char *out, size_t cap) {
    if (!root || !guest || !out || guest[0] != '/') return false;
    const size_t rl = strlen(root);
    if (!rl || root[rl - 1] == '/') return false;
    const char *p = guest + 1;
    if (!*p) return false;
    while (*p) {
        const char *c = p;
        while (*c && *c != '/') {
            const unsigned char ch = (unsigned char)*c;
            if (ch < ' ' || ch == 0x7f || strchr("\\:*?\"<>|", ch)) return false;
            c++;
        }
        const size_t len = (size_t)(c - p);
        if (len == 0 || len > 120) return false;                       // "//" or a trailing "/"
        if ((len == 1 && p[0] == '.') || (len == 2 && p[0] == '.' && p[1] == '.')) return false;
        if (p[len - 1] == '.' || p[len - 1] == ' ') return false;      // FatFs drops them: another name
        p = *c ? c + 1 : c;
        if (*c == '/' && !*p) return false;
    }
    const size_t gl = strlen(guest);
    if (rl + gl + 1 > cap) return false;
    memcpy(out, root, rl);
    memcpy(out + rl, guest, gl + 1);
    return true;
}
