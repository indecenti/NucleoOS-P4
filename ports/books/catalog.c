// catalog.c — the catalog of free books of the NucleoOS store (docs/BOOKS_PLAN.md §2): the index
// and the books of the chosen languages from GitHub Pages (kept on the card for offline use and
// refreshed when old), covers fetched as the grid shows them, search by title and author (accents
// folded), categories, a sheet per book with its source and license, and downloads through the
// system downloader (nv_dl: size and SHA-256 checked, resumed after a restart, going on with the app
// closed). A finished download joins the library.
//
// Memory: a book row is a few words plus its title and author in one pool; the rest of a book
// (description, URL, digest) is read again from the cached shard when its sheet opens.
#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "json.h"
#include "nucleo_sdk.h"

#define CAT_URL "https://indecenti.github.io/nucleoos-p4-store/books/"
#define LANG_MAX 64
#define CATS_MAX 48
#define HTTP_CAP (1u << 20)        // a shard is ~300 KB
#define COVER_CAP (256u << 10)     // a cover is ~10 KB
#define INDEX_STALE_S 3600   // small, and the catalog grows: shards follow the index fetched
#define SHARD_STALE_S (3 * 24 * 3600)
#define CCOV_KEPT 40   // covers in memory (50 KB each when ready; the ones to fetch cost nothing)
#define TOOL_H 60
#define MARGIN 28
#define CELL_W 132
#define CELL_H 192
#define ROW_H 270

typedef struct {
    uint32_t id, t, a;       // offsets in the pool
    uint32_t s;              // size, bytes
    int16_t y;               // year (0 unknown)
    uint8_t c[3];            // categories, index + 1 (0 none)
    uint8_t cv, lang, shard;
} crow_t;

typedef struct {             // a book in full (its sheet, a download)
    char id[48], t[200], st[200], a[160], ad[24], src[8], lic[16], h[72], u[300], d[600], lang[8];
    int y, cv, nc;
    uint32_t s;
    char c[3][24];
} cbook_t;

typedef struct { char code[8]; int count, shards; double rev; uint64_t loaded, asked; } clang_t;   // rev: of its shards
typedef struct { char id[24], name[48]; int count; } ccat_t;
// A card's picture in memory: the cover (state 1), or while there is none (0 to fetch, -1 none) the
// title cover drawn once (typo: its background and render mode, to draw it again when they change).
typedef struct { char id[48]; uint16_t *px; uint32_t used; int state; uint8_t typo, mode; uint16_t bg; } ccov_t;

enum { NET_IDLE, NET_INDEX, NET_SHARD, NET_COVER };
typedef struct { int kind, h, lang, shard; char id[48]; char *buf; size_t n; int t0; } net_t;

typedef struct { int id, state; char title[96], dest[160]; int64_t size, done; int bps; } cdl_t;
enum { DL_QUEUED, DL_PAUSED, DL_DONE, DL_FAILED, DL_RUNNING };

static struct {
    int ready;               // the index is known
    int failed;              // the last fetch of the index failed (offline)
    int shard_fails;         // shards that failed in a row
    int retry_at;            // no shard asked before this (ms)
    clang_t lang[LANG_MAX];
    int nlang;
    ccat_t cat[CATS_MAX];
    int ncat;
    uint8_t sel[LANG_MAX];   // languages shown
    crow_t *rows;
    int nrows, rcap;
    char *pool;
    size_t pn, pcap;
    int *vis, nvis, vcap;
    int vis_dirty;
    int filter_cat;          // -1 all
    int32_t scroll;
    net_t net;
    ccov_t cov[CCOV_KEPT];
    uint32_t clock;
    cbook_t book;            // the sheet's book
    int book_ok;
    cdl_t dl[16];
    int ndl, dl_next_poll, dl_active;
    int dl_armed;            // a cancel tapped once (download id)
    char done_title[4][96], done_file[4][120];   // the last downloads that finished
    int ndone;
} C;

// ---------------------------------------------------------------------------------- helpers

static int32_t s_drawn_scroll;
static int s_drawn_ok;   // the screen holds the grid as at s_drawn_scroll (cat_draw draws only what came in)

static const char *base_url(void) {
    static char u[200];
    if (u[0]) return u;
    char p[300];
    snprintf(p, sizeof p, "%s/appdata/catalog.url", bk_root());   // tests point the app elsewhere
    FILE *f = fopen(p, "r");
    if (f) {
        if (fgets(u, sizeof u, f)) u[strcspn(u, "\r\n")] = 0;
        fclose(f);
    }
    if (!u[0]) snprintf(u, sizeof u, "%s", CAT_URL);
    return u;
}

static uint32_t pool_add(const char *s) {
    const size_t n = strlen(s) + 1;
    if (C.pn + n > C.pcap) {
        size_t cap = C.pcap ? C.pcap * 2 : 65536;
        while (cap < C.pn + n) cap *= 2;
        char *p = (char *)realloc(C.pool, cap);
        if (!p) return 0;
        C.pool = p;
        C.pcap = cap;
    }
    if (!C.pn) C.pool[C.pn++] = 0;   // offset 0 = ""
    const uint32_t off = (uint32_t)C.pn;
    memcpy(C.pool + C.pn, s, n);
    C.pn += n;
    return off;
}
static const char *P(uint32_t off) { return C.pool && off < C.pn ? C.pool + off : ""; }

static void cache_path(char *out, size_t cap, const char *name) { snprintf(out, cap, "%s/appdata/cat/%s", bk_root(), name); }

static int64_t cache_age(const char *name) {   // seconds since it was fetched, -1 none
    char p[300];
    cache_path(p, sizeof p, name);
    char tp[310];
    snprintf(tp, sizeof tp, "%s.t", p);
    FILE *f = fopen(tp, "r");
    if (!f) return -1;
    long long t = 0;
    const int ok = fscanf(f, "%lld", &t) == 1;
    fclose(f);
    return ok ? nv_time_unix() - t : -1;
}

static char *cache_read(const char *name, size_t *n) {
    char p[300];
    cache_path(p, sizeof p, name);
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = sz > 0 && sz < (long)HTTP_CAP * 2 ? (char *)malloc((size_t)sz + 1) : NULL;
    if (b) {
        *n = fread(b, 1, (size_t)sz, f);
        b[*n] = 0;
    }
    fclose(f);
    return b;
}

static void cache_write(const char *name, const char *data, size_t n) {
    char d[300], p[300], tmp[310], tp[310];
    snprintf(d, sizeof d, "%s/appdata", bk_root());
    mkdir(d, 0777);
    snprintf(d, sizeof d, "%s/appdata/cat", bk_root());
    mkdir(d, 0777);
    cache_path(p, sizeof p, name);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    const int ok = fwrite(data, 1, n, f) == n;
    fclose(f);
    if (!ok) { remove(tmp); return; }
    remove(p);
    rename(tmp, p);
    snprintf(tp, sizeof tp, "%s.t", p);
    f = fopen(tp, "w");
    if (f) { fprintf(f, "%lld\n", (long long)nv_time_unix()); fclose(f); }
}

// lower case, Latin accents folded ("È" "è" "é" -> "e"): for search
static size_t fold(const char *s, char *out, size_t cap) {
    static const char kMap[64] = "aaaaaaaceeeeiiiidnooooo/ouuuuyty" "aaaaaaaceeeeiiiidnooooo/ouuuuyty";
    size_t k = 0;
    for (const char *p = s; *p && k + 1 < cap;) {
        const unsigned char c = (unsigned char)*p;
        if (c == 0xC3 && p[1]) {
            const unsigned char d = (unsigned char)p[1];
            if (d >= 0x80 && d <= 0xBF) { out[k++] = kMap[(d - 0x80) & 63]; p += 2; continue; }
        }
        out[k++] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
        p++;
    }
    out[k] = 0;
    return k;
}

static int lang_index(const char *code) {
    for (int i = 0; i < C.nlang; i++) if (!strcasecmp(C.lang[i].code, code)) return i;
    return -1;
}

static const char *cat_name(int ci) { return ci >= 0 && ci < C.ncat ? C.cat[ci].name : ""; }

static int have_book(const char *id) {   // the library index of <id>.epub, -1
    char f[64];
    snprintf(f, sizeof f, "%s.epub", id);
    return lib_find(f);
}

static const cdl_t *dl_of(const char *id) {
    char dest[80];
    snprintf(dest, sizeof dest, "/books/%s.epub", id);
    for (int i = 0; i < C.ndl; i++) if (!strcmp(C.dl[i].dest, dest)) return &C.dl[i];
    return NULL;
}

// -------------------------------------------------------------------------------- parsing

// A new index against the one known: a language whose count, shards or rev changed was republished (the
// catalog grows, its shards are rewritten), so its rows and cached shards go and come again; the
// others keep theirs, renumbered.
static void index_changed(const clang_t *old, int nold) {
    int map[LANG_MAX];
    for (int o = 0; o < nold; o++) {
        map[o] = -1;
        for (int i = 0; i < C.nlang; i++)
            if (!strcmp(old[o].code, C.lang[i].code)) {
                if (old[o].count == C.lang[i].count && old[o].shards == C.lang[i].shards && old[o].rev == C.lang[i].rev) {
                    map[o] = i;
                    C.lang[i].loaded = old[o].loaded;
                    C.lang[i].asked = old[o].asked;
                }
                break;
            }
        if (map[o] < 0)
            for (int k = 1; k <= old[o].shards; k++) {
                char name[40], p[400];
                snprintf(name, sizeof name, "%s-%d.json", old[o].code, k);
                cache_path(p, sizeof p, name);
                remove(p);
            }
    }
    int w = 0;
    for (int r = 0; r < C.nrows; r++) {
        const int o = C.rows[r].lang;
        if (o >= nold || map[o] < 0) continue;
        C.rows[w] = C.rows[r];
        C.rows[w++].lang = (uint8_t)map[o];
    }
    C.nrows = w;
    C.vis_dirty = 1;
}

static void parse_index(const char *s, size_t n) {
    js_t j;
    js_init(&j, s, n);
    if (!js_enter(&j, '{')) return;
    clang_t old[LANG_MAX];
    const int nold = C.nlang;
    memcpy(old, C.lang, sizeof(clang_t) * (size_t)nold);
    C.nlang = C.ncat = 0;
    char key[32];
    while (js_next(&j, '}')) {
        if (!js_key(&j, key, sizeof key)) break;
        if (!strcmp(key, "langs") && js_enter(&j, '[')) {
            while (js_next(&j, ']')) {
                if (!js_enter(&j, '{')) { js_skip(&j); continue; }
                clang_t l;
                memset(&l, 0, sizeof l);
                while (js_next(&j, '}')) {
                    char k2[24];
                    if (!js_key(&j, k2, sizeof k2)) break;
                    if (!strcmp(k2, "code")) js_string(&j, l.code, sizeof l.code);
                    else if (!strcmp(k2, "count")) l.count = (int)js_number(&j);
                    else if (!strcmp(k2, "shards")) l.shards = (int)js_number(&j);
                    else if (!strcmp(k2, "rev")) l.rev = js_number(&j);
                    else js_skip(&j);
                }
                if (l.code[0] && l.shards > 0 && l.shards <= 64 && C.nlang < LANG_MAX) C.lang[C.nlang++] = l;
            }
        } else if (!strcmp(key, "cats") && js_enter(&j, '[')) {
            while (js_next(&j, ']')) {
                if (!js_enter(&j, '{')) { js_skip(&j); continue; }
                ccat_t c;
                memset(&c, 0, sizeof c);
                char en[48] = "";
                while (js_next(&j, '}')) {
                    char k2[24];
                    if (!js_key(&j, k2, sizeof k2)) break;
                    if (!strcmp(k2, "id")) js_string(&j, c.id, sizeof c.id);
                    else if (!strcmp(k2, "names") && js_enter(&j, '{')) {
                        while (js_next(&j, '}')) {
                            char lk[8];
                            if (!js_key(&j, lk, sizeof lk)) break;
                            if (!strcmp(lk, ui_lang())) js_string(&j, c.name, sizeof c.name);
                            else if (!strcmp(lk, "en")) js_string(&j, en, sizeof en);
                            else js_skip(&j);
                        }
                    } else js_skip(&j);
                }
                if (!c.name[0]) snprintf(c.name, sizeof c.name, "%s", en[0] ? en : c.id);
                if (c.id[0] && C.ncat < CATS_MAX) C.cat[C.ncat++] = c;
            }
        } else js_skip(&j);
    }
    index_changed(old, nold);
    C.ready = C.nlang > 0;
}

static int cat_by_id(const char *id) {
    for (int i = 0; i < C.ncat; i++) if (!strcmp(C.cat[i].id, id)) return i;
    return -1;
}

// One book object: the fields a row keeps, or all of them into b (b may be NULL).
static void parse_book(js_t *j, crow_t *r, cbook_t *b) {
    char key[16], v[640];
    memset(r, 0, sizeof *r);
    if (b) memset(b, 0, sizeof *b);
    while (js_next(j, '}')) {
        if (!js_key(j, key, sizeof key)) break;
        if (!strcmp(key, "c") && js_enter(j, '[')) {
            int k = 0;
            while (js_next(j, ']')) {
                js_string(j, v, sizeof v);
                const int ci = cat_by_id(v);
                if (k < 3) {
                    if (ci >= 0) r->c[k] = (uint8_t)(ci + 1);
                    if (b) snprintf(b->c[k], sizeof b->c[k], "%s", v);
                    k++;
                }
            }
            if (b) b->nc = k;
            continue;
        }
        if (!strcmp(key, "y")) { r->y = (int16_t)js_number(j); if (b) b->y = r->y; continue; }
        if (!strcmp(key, "s")) { r->s = (uint32_t)js_number(j); if (b) b->s = r->s; continue; }
        if (!strcmp(key, "cv")) { r->cv = (uint8_t)(js_number(j) != 0); if (b) b->cv = r->cv; continue; }
        const char *h = js_here(j);
        if (*h != '"') { js_skip(j); continue; }
        js_string(j, v, sizeof v);
        if (!strcmp(key, "id")) { r->id = pool_add(v); if (b) snprintf(b->id, sizeof b->id, "%s", v); }
        else if (!strcmp(key, "t")) { r->t = pool_add(v); if (b) snprintf(b->t, sizeof b->t, "%s", v); }
        else if (!strcmp(key, "a")) { r->a = pool_add(v); if (b) snprintf(b->a, sizeof b->a, "%s", v); }
        else if (b) {
            if (!strcmp(key, "ad")) snprintf(b->ad, sizeof b->ad, "%s", v);
            else if (!strcmp(key, "st")) snprintf(b->st, sizeof b->st, "%s", v);
            else if (!strcmp(key, "src")) snprintf(b->src, sizeof b->src, "%s", v);
            else if (!strcmp(key, "lic")) snprintf(b->lic, sizeof b->lic, "%s", v);
            else if (!strcmp(key, "h")) snprintf(b->h, sizeof b->h, "%s", v);
            else if (!strcmp(key, "u")) snprintf(b->u, sizeof b->u, "%s", v);
            else if (!strcmp(key, "d")) snprintf(b->d, sizeof b->d, "%s", v);
        }
    }
}

static void parse_shard(const char *s, size_t n, int li, int shard) {
    const int t0 = nv_millis(), before = C.nrows;
    js_t j;
    js_init(&j, s, n);
    if (!js_enter(&j, '{')) return;
    char key[16];
    while (js_next(&j, '}')) {
        if (!js_key(&j, key, sizeof key)) break;
        if (strcmp(key, "books") || !js_enter(&j, '[')) { js_skip(&j); continue; }
        while (js_next(&j, ']')) {
            if (!js_enter(&j, '{')) { js_skip(&j); continue; }
            if (C.nrows == C.rcap) {
                const int cap = C.rcap ? C.rcap * 2 : 512;
                crow_t *nr = (crow_t *)realloc(C.rows, sizeof(crow_t) * (size_t)cap);
                if (!nr) return;
                C.rows = nr;
                C.rcap = cap;
            }
            crow_t *r = &C.rows[C.nrows];
            parse_book(&j, r, NULL);
            if (!r->id || !r->t) continue;
            r->lang = (uint8_t)li;
            r->shard = (uint8_t)shard;
            C.nrows++;
        }
    }
    C.lang[li].loaded |= 1ull << (shard - 1);
    C.vis_dirty = 1;
    perf("catalog: %s/%d: %d books read in %d ms (%u KB)", C.lang[li].code, shard, C.nrows - before, nv_millis() - t0,
         (unsigned)(n / 1024));
}

// The whole record of a book, from its cached shard.
static int book_detail(const crow_t *r, cbook_t *b) {
    char name[40];
    snprintf(name, sizeof name, "%s-%d.json", C.lang[r->lang].code, r->shard);
    size_t n = 0;
    char *s = cache_read(name, &n);
    if (!s) return -1;
    const char *want = P(r->id);
    js_t j;
    js_init(&j, s, n);
    int found = -1;
    char key[16];
    if (js_enter(&j, '{'))
        while (found < 0 && js_next(&j, '}')) {
            if (!js_key(&j, key, sizeof key)) break;
            if (strcmp(key, "books") || !js_enter(&j, '[')) { js_skip(&j); continue; }
            while (found < 0 && js_next(&j, ']')) {
                if (!js_enter(&j, '{')) { js_skip(&j); continue; }
                crow_t tmp;
                const size_t keep = C.pn;   // the pool takes nothing for this
                parse_book(&j, &tmp, b);
                C.pn = keep;
                if (!strcmp(b->id, want)) found = 0;
            }
        }
    free(s);
    if (!found) snprintf(b->lang, sizeof b->lang, "%s", C.lang[r->lang].code);
    return found;
}

// --------------------------------------------------------------------------------- network

static size_t net_cap(int kind) { return kind == NET_COVER ? COVER_CAP : HTTP_CAP; }

static void net_start(int kind, const char *url, int li, int shard, const char *id) {
    char spec[600];   // the firmware answers up to "max" bytes (64 KB when it is not said)
    snprintf(spec, sizeof spec, "{\"url\":\"%s\",\"method\":\"GET\",\"timeout\":20000,\"max\":%u}", url,
             (unsigned)net_cap(kind));
    const int h = nv_http_req(spec, NULL, 0);
    if (h < 0) { if (kind == NET_INDEX) C.failed = 1; return; }
    memset(&C.net, 0, sizeof C.net);
    C.net.kind = kind;
    C.net.h = h;
    C.net.lang = li;
    C.net.shard = shard;
    C.net.t0 = nv_millis();
    if (id) snprintf(C.net.id, sizeof C.net.id, "%s", id);
}

static void cover_store(const char *id, uint16_t *px, int state);
static void have_add(const char *id);

static void net_done(int ok) {
    net_t *n = &C.net;
    char name[64];
    if (n->kind == NET_INDEX) {
        if (ok) { cache_write("index-v1.json", n->buf, n->n); parse_index(n->buf, n->n); C.failed = 0; }
        else if (!C.ready) C.failed = 1;
    } else if (n->kind == NET_SHARD) {
        snprintf(name, sizeof name, "%s-%d.json", C.lang[n->lang].code, n->shard);
        if (ok) {
            cache_write(name, n->buf, n->n);
            if (!(C.lang[n->lang].loaded & (1ull << (n->shard - 1)))) parse_shard(n->buf, n->n, n->lang, n->shard);
            C.shard_fails = 0;
        } else if (!(C.lang[n->lang].loaded & (1ull << (n->shard - 1)))) {   // asked again, 2 s, 4 s ... 30 s later
            C.lang[n->lang].asked &= ~(1ull << (n->shard - 1));
            const int wait = C.shard_fails < 4 ? 2000 << C.shard_fails : 30000;
            C.shard_fails++;
            C.retry_at = nv_millis() + wait;
        }
    } else if (n->kind == NET_COVER) {
        uint16_t *px = NULL;
        if (ok) {
            const int keep = g_rmode;
            g_rmode = RM_COLOR;   // the cache holds colour; drawing tones it
            px = img_render565((const uint8_t *)n->buf, n->n, COVER_W, COVER_H, rgb565(0x30, 0x34, 0x3A), 1);
            g_rmode = keep;
            if (px) {   // kept on the card: a cover is fetched once
                char p[300];
                ccov_path(p, sizeof p, n->id, 1);
                FILE *f = fopen(p, "wb");
                if (f) {
                    const int ok2 = fwrite(px, 2, (size_t)COVER_W * COVER_H, f) == (size_t)COVER_W * COVER_H;
                    fclose(f);
                    if (ok2) have_add(n->id);
                }
            }
        }
        cover_store(n->id, px, px ? 1 : -1);
    }
    perf("catalog: %s %s in %d ms (%u KB)", n->kind == NET_INDEX ? "index" : n->kind == NET_SHARD ? "shard" : "cover",
         ok ? "ok" : "failed", nv_millis() - n->t0, (unsigned)(n->n / 1024));
    free(n->buf);
    memset(n, 0, sizeof *n);
    A.redraw |= R_ALL;
}

static void net_poll(void) {
    net_t *n = &C.net;
    if (n->kind == NET_IDLE) return;
    const int st = nv_http_state(n->h);
    if (st == 0) return;
    if (st < 0 || nv_http_status(n->h) != 200) { nv_http_close(n->h); net_done(0); return; }
    const size_t cap = net_cap(n->kind);
    if (!n->buf) n->buf = (char *)malloc(cap + 1);
    if (!n->buf) { nv_http_close(n->h); net_done(0); return; }
    for (;;) {
        const int r = nv_http_read(n->h, n->buf + n->n, (uint32_t)(cap + 1 - n->n));
        if (r <= 0) break;
        n->n += (size_t)r;
        if (n->n > cap) break;
    }
    n->buf[n->n < cap ? n->n : cap] = 0;
    nv_http_close(n->h);
    net_done(n->n > 0 && n->n <= cap);
}

// ---------------------------------------------------------------------------------- covers

static uint32_t id_hash(const char *id) {
    uint32_t h = 2166136261u;
    for (const char *p = id; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    return h;
}

static struct { uint32_t *h; int n, cap, loaded; } s_have;

static int have_at(uint32_t h) {   // where h is, or goes
    int lo = 0, hi = s_have.n;
    while (lo < hi) {
        const int m = (lo + hi) / 2;
        if (s_have.h[m] < h) lo = m + 1;
        else hi = m;
    }
    return lo;
}

static int have_has(uint32_t h) { const int i = have_at(h); return i < s_have.n && s_have.h[i] == h; }

static void have_put(uint32_t h) {
    const int i = have_at(h);
    if (i < s_have.n && s_have.h[i] == h) return;
    if (s_have.n == s_have.cap) {
        const int cap = s_have.cap ? s_have.cap * 2 : 1024;
        uint32_t *nh = (uint32_t *)realloc(s_have.h, sizeof(uint32_t) * (size_t)cap);
        if (!nh) return;
        s_have.h = nh;
        s_have.cap = cap;
    }
    memmove(s_have.h + i + 1, s_have.h + i, sizeof(uint32_t) * (size_t)(s_have.n - i));
    s_have.h[i] = h;
    s_have.n++;
}

static void have_load(void) {
    if (s_have.loaded) return;
    s_have.loaded = 1;
    char p[300];
    snprintf(p, sizeof p, "%s/appdata/ccov/have.u32", bk_root());
    FILE *f = fopen(p, "rb");
    if (!f) return;
    uint32_t b[256];
    size_t k;
    while ((k = fread(b, 4, 256, f)) > 0)
        for (size_t i = 0; i < k; i++) have_put(b[i]);
    fclose(f);
}

static void have_add(const char *id) {   // a cover now on the card
    const uint32_t h = id_hash(id);
    if (have_has(h)) return;
    have_put(h);
    char p[300];
    snprintf(p, sizeof p, "%s/appdata/ccov/have.u32", bk_root());
    FILE *f = fopen(p, "ab");
    if (f) { fwrite(&h, 4, 1, f); fclose(f); }
}

static ccov_t *cover_find(const char *id) {
    for (int i = 0; i < CCOV_KEPT; i++) if (C.cov[i].id[0] && !strcmp(C.cov[i].id, id)) { C.cov[i].used = ++C.clock; return &C.cov[i]; }
    return NULL;
}

static void cover_store(const char *id, uint16_t *px, int state) {
    ccov_t *e = cover_find(id);
    if (!e) {
        int lru = 0;
        for (int i = 0; i < CCOV_KEPT; i++) {
            if (!C.cov[i].id[0]) { lru = i; break; }
            if (C.cov[i].used < C.cov[lru].used) lru = i;
        }
        e = &C.cov[lru];
        free(e->px);
        memset(e, 0, sizeof *e);
        snprintf(e->id, sizeof e->id, "%s", id);
    }
    if (px || state == 1) {
        if (e->px && e->px != px) free(e->px);
        e->px = px;
        e->typo = 0;
    }
    e->state = state;
    e->used = ++C.clock;
}

void ccov_path(char *out, size_t cap, const char *id, int mk) {
    const uint32_t h = id_hash(id);
    char d[300];
    snprintf(d, sizeof d, "%s/appdata/ccov/%02x", bk_root(), (unsigned)(h & 31));
    if (mk) {
        char top[300];
        snprintf(top, sizeof top, "%s/appdata/ccov", bk_root());
        mkdir(top, 0777);
        mkdir(d, 0777);
    }
    snprintf(out, cap, "%s/%s.565", d, id);
}

static uint16_t *cover_load(const char *id) {   // the card's copy, or NULL
    if (!have_has(id_hash(id))) return NULL;
    char p[300];
    ccov_path(p, sizeof p, id, 0);
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    uint16_t *px = (uint16_t *)malloc((size_t)COVER_W * COVER_H * 2);
    const int ok = px && fread(px, 2, (size_t)COVER_W * COVER_H, f) == (size_t)COVER_W * COVER_H;
    fclose(f);
    if (ok) return px;
    free(px);
    return NULL;
}

// The cover of a book with "cv": the card's copy (looked for once), else fetched, one at a time. NULL
// until then.
static const uint16_t *cover_of(const crow_t *r) {
    const char *id = P(r->id);
    ccov_t *e = cover_find(id);
    if (!e) {
        if (!r->cv) return NULL;
        uint16_t *px = cover_load(id);
        cover_store(id, px, px ? 1 : 0);
        if (px) return px;
        e = cover_find(id);
    }
    if (e->state == 1) return e->px;
    if (e->state == 0 && C.net.kind == NET_IDLE && A.now - A.idle_since > 150) {
        char url[300];
        snprintf(url, sizeof url, "%scovers/%s.jpg", base_url(), id);
        net_start(NET_COVER, url, 0, 0, id);
    }
    return NULL;
}

static int grid_top(void);

static void draw_tile565(const uint16_t *px, int x, int y) {
    int cx0, cy0, cx1, cy1;
    gfx_get_clip(&cx0, &cy0, &cx1, &cy1);
    const int x0 = x < cx0 ? cx0 : x, x1 = x + COVER_W > cx1 ? cx1 : x + COVER_W;
    if (x1 <= x0) return;
    for (int j = 0; j < COVER_H; j++) {
        const int sy = y + j;
        if (sy < cy0 || sy >= cy1) continue;
        memcpy(g_rows[sy] + x0, px + (size_t)j * COVER_W + (x0 - x), (size_t)(x1 - x0) * 2);
    }
    gfx_dirty(y, y + COVER_H);
}

static uint16_t *typo_tile(const crow_t *r) {   // the title cover, drawn off the screen
    uint16_t *px = (uint16_t *)malloc((size_t)COVER_W * COVER_H * 2);
    uint16_t **rows = (uint16_t **)malloc(sizeof(uint16_t *) * COVER_H);
    if (!px || !rows) { free(px); free(rows); return NULL; }
    for (int j = 0; j < COVER_H; j++) rows[j] = px + (size_t)j * COVER_W;
    for (int i = 0; i < COVER_W * COVER_H; i++) px[i] = g_col[C_BG];
    surf_t s = {px, rows};
    gfx_target(&s);
    gfx_clip(0, 0, COVER_W, COVER_H);
    draw_typo_cover(P(r->t), P(r->a), (uint32_t)(r->id * 2654435761u), 0, 0, COVER_W, COVER_H);
    gfx_target(NULL);
    free(rows);
    return px;
}

// What a card shows: its cover, else the title cover (kept). NULL: draw the title cover directly.
static const uint16_t *card_art(const crow_t *r, int *raw) {
    *raw = 0;
    const uint16_t *c = cover_of(r);
    if (c) return c;
    const char *id = P(r->id);
    ccov_t *e = cover_find(id);
    if (!e) {
        cover_store(id, NULL, -1);
        if (!(e = cover_find(id))) return NULL;
    }
    if (e->px && e->typo && (e->bg != g_col[C_BG] || e->mode != g_rmode)) { free(e->px); e->px = NULL; }
    if (!e->px) {
        e->px = typo_tile(r);
        e->typo = 1;
        e->bg = g_col[C_BG];
        e->mode = (uint8_t)g_rmode;
    }
    *raw = 1;   // drawn already in the render mode
    return e->px;
}

static void draw_cover565(const uint16_t *px, int x, int y, int w, int h) {
    if (w == COVER_W && h == COVER_H && g_rmode == RM_COLOR) {   // the usual case: rows copied, inside the clip
        int cx0, cy0, cx1, cy1;
        gfx_get_clip(&cx0, &cy0, &cx1, &cy1);
        const int x0 = x < cx0 ? cx0 : x, x1 = x + w > cx1 ? cx1 : x + w;
        if (x1 > x0)
            for (int j = 0; j < h; j++) {
                const int sy = y + j;
                if (sy < cy0 || sy >= cy1) continue;
                memcpy(g_rows[sy] + x0, px + (size_t)j * COVER_W + (x0 - x), (size_t)(x1 - x0) * 2);
            }
        gfx_dirty(y, y + h);
        gfx_ring(x, y, w, h, 2, 1, g_col[C_RULE]);
        return;
    }
    for (int j = 0; j < h; j++) {
        const int sy = y + j;
        if (sy < 0 || sy >= g_h) continue;
        const uint16_t *src = px + (size_t)(j * COVER_H / h) * COVER_W;
        uint16_t *d = g_rows[sy];
        for (int k = 0; k < w; k++) {
            const int sx = x + k;
            if (sx < 0 || sx >= g_w) continue;
            uint16_t c = src[k * COVER_W / w];
            if (g_rmode != RM_COLOR) {
                int l = lum565(c);
                if (g_rmode == RM_MONO) {
                    static const uint8_t kB[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
                    l = l * 16 / 256 > kB[(sy & 3) * 4 + (sx & 3)] ? 255 : 0;
                } else l = ((l * 15 + 127) / 255) * 17;
                c = rgb565(l, l, l);
            }
            d[sx] = c;
        }
    }
    gfx_dirty(y, y + h);
    gfx_ring(x, y, w, h, 2, 1, g_col[C_RULE]);
}

// ------------------------------------------------------------------------------ the shown books

static void rebuild_vis(void) {
    char q[160];
    fold(A.q, q, sizeof q);
    // the query's words: every one must be in the title or the author
    char *words[8];
    int nw = 0;
    for (char *p = strtok(q, " "); p && nw < 8; p = strtok(NULL, " ")) words[nw++] = p;
    if (C.vcap < C.nrows) {
        int *nv = (int *)realloc(C.vis, sizeof(int) * (size_t)(C.nrows + 1));
        if (!nv) return;
        C.vis = nv;
        C.vcap = C.nrows;
    }
    C.nvis = 0;
    for (int i = 0; i < C.ncat; i++) C.cat[i].count = 0;
    for (int i = 0; i < C.nrows; i++) {
        const crow_t *r = &C.rows[i];
        if (!C.sel[r->lang]) continue;
        for (int k = 0; k < 3; k++) if (r->c[k]) C.cat[r->c[k] - 1].count++;
        if (C.filter_cat >= 0 && r->c[0] != C.filter_cat + 1 && r->c[1] != C.filter_cat + 1 && r->c[2] != C.filter_cat + 1) continue;
        if (nw) {
            char hay[400];
            char ta[400];
            snprintf(ta, sizeof ta, "%s %s", P(r->t), P(r->a));
            fold(ta, hay, sizeof hay);
            int ok = 1;
            for (int w = 0; w < nw && ok; w++) ok = strstr(hay, words[w]) != NULL;
            if (!ok) continue;
        }
        C.vis[C.nvis++] = i;
    }
    C.vis_dirty = 0;
}

static int all_loaded(void) {
    for (int i = 0; i < C.nlang; i++)
        if (C.sel[i] && C.lang[i].loaded != (C.lang[i].shards >= 64 ? ~0ull : (1ull << C.lang[i].shards) - 1)) return 0;
    return 1;
}

// The next shard to read: a selected language's first missing one (searching: any of them).
static int next_shard(int *li, int *k) {
    for (int pass = 0; pass < 64; pass++)
        for (int i = 0; i < C.nlang; i++) {
            if (!C.sel[i] || pass >= C.lang[i].shards) continue;
            if (!(C.lang[i].loaded & (1ull << pass)) && !(C.lang[i].asked & (1ull << pass))) { *li = i; *k = pass + 1; return 1; }
        }
    return 0;
}

static int cols(void) { const int c = (g_w - 2 * MARGIN + 24) / (CELL_W + 24); return c < 1 ? 1 : c; }
static int pitch(void) { const int c = cols(); return c > 1 ? (g_w - 2 * MARGIN - CELL_W) / (c - 1) : 0; }
static int grid_top(void) { return BAR_H + TOOL_H + 12; }
static int grid_height(void) { return (C.nvis + cols() - 1) / cols() * ROW_H + 60; }

// Shards are read when the end of the list comes near, or all of them for a search or a category.
static void want_more(void) {
    if (C.net.kind != NET_IDLE || !C.ready || nv_millis() - C.retry_at < 0) return;
    const int need = A.q[0] || C.filter_cat >= 0 || C.scroll + (g_h - grid_top()) * 2 > grid_height() || C.nvis < 24;
    if (!need) return;
    int li, k;
    if (!next_shard(&li, &k)) return;
    char name[40];
    snprintf(name, sizeof name, "%s-%d.json", C.lang[li].code, k);
    C.lang[li].asked |= 1ull << (k - 1);
    const int64_t age = cache_age(name);
    size_t n = 0;
    char *s = age >= 0 ? cache_read(name, &n) : NULL;
    if (s) {   // the card's copy now; a new one when it is old (the next visit shows it)
        parse_shard(s, n, li, k);
        free(s);
        A.redraw |= R_ALL;
        if (age < SHARD_STALE_S) return;
        C.lang[li].loaded &= ~(1ull << (k - 1));   // parsed, but fetched again: the fresh copy replaces it on disk
        C.lang[li].loaded |= 1ull << (k - 1);
    }
    char url[300];
    snprintf(url, sizeof url, "%s%s/%d.json", base_url(), C.lang[li].code, k);
    net_start(NET_SHARD, url, li, k, NULL);
}

static void sel_from_settings(void) {
    memset(C.sel, 0, sizeof C.sel);
    int any = 0;
    char b[64];
    snprintf(b, sizeof b, "%s", g_set.cat_langs);
    for (char *p = strtok(b, ","); p; p = strtok(NULL, ",")) {
        const int i = lang_index(p);
        if (i >= 0) { C.sel[i] = 1; any = 1; }
    }
    if (!any) {   // the interface's language, and English
        int i = lang_index(ui_lang());
        if (i >= 0) C.sel[i] = 1;
        if ((i = lang_index("en")) >= 0) C.sel[i] = 1;
        if (i < 0 && C.nlang) C.sel[0] = 1;
    }
}

static void sel_to_settings(void) {
    g_set.cat_langs[0] = 0;
    for (int i = 0; i < C.nlang; i++) {
        if (!C.sel[i]) continue;
        const size_t l = strlen(g_set.cat_langs);
        if (l + strlen(C.lang[i].code) + 2 >= sizeof g_set.cat_langs) break;
        snprintf(g_set.cat_langs + l, sizeof g_set.cat_langs - l, "%s%s", l ? "," : "", C.lang[i].code);
    }
    settings_save();
}

// ------------------------------------------------------------------------------ downloads

static int dl_state(const char *s) {
    return !strcmp(s, "running") ? DL_RUNNING : !strcmp(s, "paused") ? DL_PAUSED : !strcmp(s, "done") ? DL_DONE
           : !strcmp(s, "failed") ? DL_FAILED : DL_QUEUED;
}

static void dl_refresh(void) {
    static char buf[16384];
    const int n = nv_dl_list(buf, sizeof buf - 1);
    if (n < 0) return;
    buf[n] = 0;
    js_t j;
    js_init(&j, buf, (size_t)n);
    int k = 0, active = 0, finished = 0;
    if (js_enter(&j, '['))
        while (js_next(&j, ']') && k < 16) {
            if (!js_enter(&j, '{')) { js_skip(&j); continue; }
            cdl_t d;
            memset(&d, 0, sizeof d);
            char key[16], st[16] = "";
            while (js_next(&j, '}')) {
                if (!js_key(&j, key, sizeof key)) break;
                if (!strcmp(key, "id")) d.id = (int)js_number(&j);
                else if (!strcmp(key, "state")) js_string(&j, st, sizeof st);
                else if (!strcmp(key, "title")) js_string(&j, d.title, sizeof d.title);
                else if (!strcmp(key, "dest")) js_string(&j, d.dest, sizeof d.dest);
                else if (!strcmp(key, "size")) d.size = (int64_t)js_number(&j);
                else if (!strcmp(key, "done")) d.done = (int64_t)js_number(&j);
                else if (!strcmp(key, "bps")) d.bps = (int)js_number(&j);
                else js_skip(&j);
            }
            d.state = dl_state(st);
            if (strncmp(d.dest, "/books/", 7)) continue;   // the app's other downloads (none today)
            if (d.state == DL_DONE) {   // in the library now: the downloader may forget it
                finished++;
                nv_dl_cancel(d.id);
                toast(tr(S_CAT_DONE), d.title);
                snprintf(C.done_title[C.ndone % 4], sizeof C.done_title[0], "%s", d.title);
                snprintf(C.done_file[C.ndone % 4], sizeof C.done_file[0], "%s", d.dest + 7);
                C.ndone++;
                continue;
            }
            active += d.state == DL_RUNNING || d.state == DL_QUEUED;
            C.dl[k++] = d;
        }
    C.ndl = k;
    C.dl_active = active;
    if (finished) {
        library_rescan();
        // until the book itself is read (idle): the catalog's title on its tile
        for (int i = 0; i < 4 && i < C.ndone; i++) {
            const int li = lib_find(C.done_file[i]);
            if (li >= 0 && g_lib[li].nch < 0) snprintf(g_lib[li].title, sizeof g_lib[li].title, "%s", C.done_title[i]);
        }
        A.redraw |= R_ALL;
    }
    if (k && A.screen == SCR_CATALOG) A.redraw |= R_ALL;   // the progress on the cards and the sheet
}

static void json_escape(const char *s, char *out, size_t cap) {
    size_t k = 0;
    for (; *s && k + 7 < cap; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[k++] = '\\'; out[k++] = (char)c; }
        else if (c < 0x20) out[k++] = ' ';
        else out[k++] = (char)c;
    }
    out[k] = 0;
}

static void download(const cbook_t *b) {
    if (!b->u[0] || !b->h[0] || !b->s) { toast(tr(S_CAT_FAILED), NULL); return; }
    if ((int64_t)b->s / 1048576 + 1 > nv_dl_free() && nv_dl_free() >= 0) { toast(tr(S_CAT_NO_ROOM), NULL); return; }
    char t[200], spec[1200];
    json_escape(b->t, t, sizeof t);
    snprintf(spec, sizeof spec, "{\"url\":\"%s\",\"size\":%u,\"sha256\":\"%s\",\"dest\":\"/books/%s.epub\",\"title\":\"%s\"}", b->u,
             (unsigned)b->s, b->h, b->id, t);
    const int id = nv_dl_start(spec);
    perf("catalog: download %s -> %d", b->id, id);
    if (id < 0) toast(tr(S_CAT_FAILED), NULL);
    dl_refresh();
    C.dl_next_poll = A.now + 1000;   // followed every second from now on
    A.redraw |= R_ALL;
}

void cat_bg_poll(void) {   // every screen: downloads finish into the library
    if (A.now - C.dl_next_poll < 0) return;
    C.dl_next_poll = A.now + (A.screen == SCR_CATALOG || C.dl_active ? 1000 : 6000);
    dl_refresh();
}

// ---------------------------------------------------------------------------------- screen

void cat_open(void) {
    A.screen = SCR_CATALOG;
    A.sheet = SH_NONE;
    A.q[0] = 0;
    A.osk_on = 0;
    C.scroll = 0;
    C.filter_cat = -1;
    have_load();
    s_drawn_ok = 0;
    if (!C.ready) {   // the card's index first, the network's when it is old or missing
        size_t n = 0;
        const int64_t age = cache_age("index-v1.json");
        char *s = age >= 0 ? cache_read("index-v1.json", &n) : NULL;
        if (s) { parse_index(s, n); free(s); }
        if (C.ready) sel_from_settings();
        if (!C.ready || age > INDEX_STALE_S) {
            char url[300];
            snprintf(url, sizeof url, "%sindex-v1.json", base_url());
            net_start(NET_INDEX, url, 0, 0, NULL);
        }
    }
    C.vis_dirty = 1;
    dl_refresh();
    C.dl_next_poll = A.now + 1000;
    A.redraw |= R_ALL;
}

void cat_frame(void) {
    const int was_ready = C.ready;
    net_poll();
    if (!was_ready && C.ready) { sel_from_settings(); C.vis_dirty = 1; }
    want_more();
    if (C.vis_dirty) { rebuild_vis(); A.redraw |= R_ALL; }
    const int32_t m = grid_height() - (g_h - grid_top());
    if (C.scroll > m) C.scroll = m > 0 ? m : 0;
    if (C.scroll < 0) C.scroll = 0;
}

static void draw_badge(int x, int y, const char *s, uint16_t bg, uint16_t fg) {
    const int w = text_width(F_BOLD, 12, s, (int)strlen(s)) + 14;
    gfx_rrect(x, y, w, 22, 11, bg);
    text_draw(F_BOLD, 12, x + 7, y + 16, s, (int)strlen(s), fg);
}

static uint64_t us_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static int s_t_art, s_t_text;   // the last frame's time on pictures and on words (us)

static int card_reg(int i, int x, int y) { return reg_add(x - 8, y - 8, CELL_W + 16, ROW_H - 8, A_CAT_ROW, i); }

static void draw_card(int i, int x, int y) {
    const crow_t *r = &C.rows[i];
    int cx0, cy0, cx1, cy1;
    gfx_get_clip(&cx0, &cy0, &cx1, &cy1);
    const int reg = card_reg(i, x, y);
    const int st = reg_state(reg);
    if (st == 2) gfx_rrect(x - 8, y - 8, CELL_W + 16, ROW_H - 12, 14, g_col[C_PRESS]);
    if (st == 1) gfx_ring(x - 8, y - 8, CELL_W + 16, ROW_H - 12, 14, 2, g_col[C_FOCUS]);
    int raw;
    const uint64_t ta = us_now();
    const uint16_t *px = card_art(r, &raw);
    if (px && raw) draw_tile565(px, x, y);
    else if (px) draw_cover565(px, x, y, CELL_W, CELL_H);
    else draw_typo_cover(P(r->t), P(r->a), (uint32_t)(r->id * 2654435761u), x, y, CELL_W, CELL_H);
    gfx_clip(cx0, cy0, cx1, cy1);   // drawing off the screen left the clip
    s_t_art += (int)(us_now() - ta);
    const char *id = P(r->id);
    const cdl_t *d = dl_of(id);
    if (have_book(id) >= 0) draw_badge(x + 6, y + CELL_H - 30, tr(S_CAT_HAVE), g_col[C_ACCENT], g_col[C_ON_ACCENT]);
    else if (d) {
        const int pm = d->size > 0 ? (int)(d->done * 1000 / d->size) : 0;
        gfx_rrect(x + 6, y + CELL_H - 14, CELL_W - 12, 6, 3, g_col[C_BG]);
        gfx_rrect(x + 6, y + CELL_H - 14, (CELL_W - 12) * pm / 1000, 6, 3, d->state == DL_FAILED ? g_col[C_MARK] : g_col[C_ACCENT]);
    }
    const uint64_t tt = us_now();
    wrap_text(F_BOLD, 15, x, y + CELL_H + 22, CELL_W, 19, P(r->t), 2, g_col[C_TEXT]);
    text_draw_fit(F_SANS, 13, x, y + CELL_H + 60, P(r->a), (int)strlen(P(r->a)), CELL_W, g_col[C_DIM]);
    s_t_text += (int)(us_now() - tt);
}

static int pill_w(const char *s) { return text_width(F_SANS, 17, s, (int)strlen(s)) + 32; }

static void draw_book_sheet(void);
static void draw_list_sheet(void);

void cat_draw(void) {
    static int s_log_at;
    const int t0 = nv_millis();
    const int top = grid_top(), gy0 = top - 8;
    const int dy = (int)(C.scroll - s_drawn_scroll), ady = dy < 0 ? -dy : dy;
    const int band = A.redraw == R_SCROLL && s_drawn_ok && !C.vis_dirty && A.sheet == SH_NONE && !A.osk_on &&
                     !A.toast[0] && C.nvis && dy && ady < (g_h - gy0) * 2 / 3;
    if (C.vis_dirty) rebuild_vis();
    s_t_art = s_t_text = 0;
    int sy0 = gy0, sy1 = g_h;   // the rows to draw
    if (band) {
        gfx_scroll(gy0, g_h, dy);
        if (dy > 0) sy0 = g_h - dy;
        else sy1 = gy0 + ady;
        gfx_fill(0, sy0, g_w, sy1 - sy0, g_col[C_BG]);
    } else {
        gfx_fill(0, 0, g_w, g_h, g_col[C_BG]);
    }
    const int t_fill = nv_millis() - t0;
    // the grid (every card keeps its place to be tapped; the ones in the band are drawn)
    gfx_clip(0, sy0, g_w, sy1);
    const int c = cols(), p = pitch();
    for (int k = 0; k < C.nvis; k++) {
        const int row = k / c, col = k % c;
        const int y = top + row * ROW_H - C.scroll;
        if (y + ROW_H < top) { k += c - col - 1; continue; }
        if (y > g_h) break;
        if (y - 8 < sy1 && y + ROW_H > sy0) draw_card(C.vis[k], MARGIN + col * p, y);
        else card_reg(C.vis[k], MARGIN + col * p, y);
    }
    gfx_noclip();
    s_drawn_scroll = C.scroll;
    s_drawn_ok = 1;
    const char *msg = NULL;
    const int offline = (!C.ready && C.failed) || (!C.nvis && C.shard_fails >= 2);
    if (offline) msg = tr(S_CAT_OFFLINE);
    else if (!C.ready) msg = tr(S_CAT_LOADING);
    else if (!C.nvis && C.net.kind == NET_IDLE && all_loaded()) msg = tr(S_CAT_NONE);
    else if (!C.nvis) msg = tr(S_CAT_LOADING);
    if (msg) {
        s_drawn_ok = 0;   // not the grid alone
        const int w = text_width(F_SANS, 18, msg, (int)strlen(msg));
        text_draw(F_SANS, 18, (g_w - w) / 2, top + 120, msg, (int)strlen(msg), g_col[C_DIM]);
        if (offline) {
            const char *rl = tr(S_CAT_RETRY);
            pill((g_w - pill_w(rl)) / 2, top + 150, rl, A_CAT_RETRY, 0, 1);
        }
    }
    // the bars on top: as they were when only the grid moved (their places to tap kept)
    static reg_t s_bar_regs[16];
    static int s_nbar;
    if (band) {
        for (int i = 0; i < s_nbar; i++) {
            const reg_t *r = &s_bar_regs[i];
            reg_add(r->x, r->y, r->w, r->h, r->act, r->arg);
        }
    } else {
    const int nb0 = A.nregs;
    bar_bg();
    button(6, 6, IC_BACK, A_BACK, 0, 1, g_col[C_BAR_TEXT]);
    const char *t = tr(S_CATALOG);
    text_draw(F_BOLD, 24, 68, 41, t, (int)strlen(t), g_col[C_BAR_TEXT]);
    if (C.ready) {
        char n[48];
        snprintf(n, sizeof n, tr(S_CAT_BOOKS), C.nvis);
        if (!all_loaded() && !A.q[0] && C.filter_cat < 0) {   // more on the way: the index's total
            int tot = 0;
            for (int i = 0; i < C.nlang; i++) if (C.sel[i]) tot += C.lang[i].count;
            snprintf(n, sizeof n, tr(S_CAT_BOOKS), tot);
        }
        text_draw(F_SANS, 15, 68 + text_width(F_BOLD, 24, t, (int)strlen(t)) + 12, 40, n, (int)strlen(n), g_col[C_DIM]);
    }
    // downloads, with how many are going
    int going = 0;
    for (int i = 0; i < C.ndl; i++) going += C.dl[i].state != DL_DONE;
    button(g_w - BTN - 8, 6, IC_DOWNLOAD, A_CAT_DLS, 0, 1, g_col[C_BAR_TEXT]);
    if (going) {
        char b[8];
        snprintf(b, sizeof b, "%d", going);
        draw_badge(g_w - 34, 6, b, g_col[C_MARK], g_col[C_ON_ACCENT]);
    }
    // the toolbar: search, category, languages
    gfx_fill(0, BAR_H, g_w, TOOL_H, g_col[C_BAR]);
    gfx_fill(0, BAR_H + TOOL_H - 1, g_w, 1, g_col[C_RULE]);
    char langs[64] = "";
    for (int i = 0; i < C.nlang; i++)
        if (C.sel[i] && strlen(langs) + 5 < sizeof langs) {
            char up[8];
            snprintf(up, sizeof up, "%s", C.lang[i].code);
            for (char *q = up; *q; q++) if (*q >= 'a' && *q <= 'z') *q -= 32;
            snprintf(langs + strlen(langs), sizeof langs - strlen(langs), "%s%s", langs[0] ? " " : "", up);
        }
    if (!langs[0]) snprintf(langs, sizeof langs, "%s", tr(S_CAT_LANGS));
    const char *cn = C.filter_cat >= 0 ? cat_name(C.filter_cat) : tr(S_CAT_ALL);
    char cl[64];
    snprintf(cl, sizeof cl, "%s \xE2\x96\xBE", cn);
    const int lw = pill_w(langs), cw = pill_w(cl) < 220 ? pill_w(cl) : 220;
    const int fx = 16, fw = g_w - 16 - lw - cw - 40;
    draw_field(fx, BAR_H + 8, fw, tr(S_CAT_SEARCH));
    pill(g_w - 16 - lw - cw - 10, BAR_H + 10, cl, A_CAT_CHIP, 0, C.filter_cat >= 0);
    pill(g_w - 16 - lw, BAR_H + 10, langs, A_CAT_LANGS, 0, 0);
    s_nbar = A.nregs - nb0 < 16 ? A.nregs - nb0 : 16;
    memcpy(s_bar_regs, A.regs + nb0, sizeof(reg_t) * (size_t)s_nbar);
    }
    if (A.sheet == SH_CAT_BOOK) draw_book_sheet();
    else if (A.sheet == SH_CAT_LANGS || A.sheet == SH_CAT_DLS || A.sheet == SH_CAT_CATS) draw_list_sheet();
    if (A.sheet != SH_NONE || A.osk_on) s_drawn_ok = 0;
    const int ms = nv_millis() - t0;
    if (ms > 30 && nv_millis() - s_log_at > 2000) {   // slow frames, now and then
        s_log_at = nv_millis();
        extern int g_flush_ms, g_flush_rot_ms;
        perf("catalog: %s drawn in %d ms (background %d, pictures %d, words %d; last flush %d, turning %d; %d of %d)",
             band ? "band" : "frame", ms, t_fill, s_t_art / 1000, s_t_text / 1000, g_flush_ms, g_flush_rot_ms, C.nvis,
             C.nrows);
    }
}

// ------------------------------------------------------------------------------- sheets

static const char *lic_name(const char *l) {
    static const struct { const char *k, *n; } kL[] = {{"cc0", "CC0"}, {"cc-by", "CC BY"}, {"cc-by-sa", "CC BY-SA"},
                                                       {"gfdl", "GNU FDL"}, {"mit", "MIT"}, {"apache", "Apache 2.0"},
                                                       {"bsd", "BSD"}, {"ofl", "SIL OFL"}};
    if (!strcmp(l, "pd")) return tr(S_CAT_PD);
    for (size_t i = 0; i < sizeof kL / sizeof kL[0]; i++) if (!strcmp(l, kL[i].k)) return kL[i].n;
    return l;
}

static const char *src_name(const char *s) {
    return !strcmp(s, "pg") ? "Project Gutenberg" : !strcmp(s, "se") ? "Standard Ebooks" : !strcmp(s, "ws") ? "Wikisource"
           : !strcmp(s, "ia") ? "Internet Archive" : s;
}

static void draw_book_sheet(void) {
    if (!C.book_ok) { A.sheet = SH_NONE; return; }
    const cbook_t *b = &C.book;
    scrim();
    const int w = g_w - 32 < 760 ? g_w - 32 : 760, h = g_h - 60 < 520 ? g_h - 60 : 520, x = (g_w - w) / 2, y = (g_h - h) / 2;
    sheet_frame(x, y, w, h);
    button(x + w - BTN - 8, y + 8, IC_CLOSE, A_CLOSE, 0, 1, g_col[C_DIM]);
    // the cover
    ccov_t *e = cover_find(b->id);
    if (e && e->state == 1) draw_cover565(e->px, x + 28, y + 28, CELL_W, CELL_H);
    else draw_typo_cover(b->t, b->a, (uint32_t)strlen(b->t) * 2654435761u, x + 28, y + 28, CELL_W, CELL_H);
    const int tx = x + 28 + CELL_W + 28, tw = w - (tx - x) - 60;
    const int tl = wrap_text(F_SERIF_BOLD, 22, tx, y + 58, tw, 28, b->t, b->st[0] ? 2 : 3, g_col[C_TEXT]);
    int ry = y + 58 + 28 * (b->st[0] ? tl : 3);
    if (b->st[0]) ry += 22 * wrap_text(F_SERIF_ITALIC, 17, tx, ry - 4, tw, 22, b->st, 2, g_col[C_DIM]) + 6;   // the subtitle
    char line[300];
    snprintf(line, sizeof line, "%s%s%s%s", b->a[0] ? b->a : tr(S_UNKNOWN_AUTHOR), b->ad[0] ? " (" : "", b->ad, b->ad[0] ? ")" : "");
    text_draw_fit(F_SANS, 17, tx, ry, line, (int)strlen(line), tw + 40, g_col[C_TEXT]);
    ry += 28;
    char sz[24];
    fmt_size(b->s, sz, sizeof sz);
    if (b->y) snprintf(line, sizeof line, "%d  \xC2\xB7  %s  \xC2\xB7  %s", b->y, lang_name(b->lang), sz);
    else snprintf(line, sizeof line, "%s  \xC2\xB7  %s", lang_name(b->lang), sz);
    text_draw_fit(F_SANS, 15, tx, ry, line, (int)strlen(line), tw + 40, g_col[C_DIM]);
    ry += 26;
    line[0] = 0;
    for (int k = 0; k < b->nc; k++) {
        const int ci = cat_by_id(b->c[k]);
        snprintf(line + strlen(line), sizeof line - strlen(line), "%s%s", k ? ", " : "", ci >= 0 ? C.cat[ci].name : b->c[k]);
    }
    if (line[0]) { text_draw_fit(F_SANS, 15, tx, ry, line, (int)strlen(line), tw + 40, g_col[C_DIM]); ry += 26; }
    snprintf(line, sizeof line, "%s: %s  \xC2\xB7  %s: %s", tr(S_CAT_SOURCE), src_name(b->src), tr(S_CAT_LICENSE), lic_name(b->lic));
    text_draw_fit(F_SANS, 14, tx, ry, line, (int)strlen(line), tw + 40, g_col[C_DIM]);
    // the description
    const int dy = y + 28 + CELL_H + 34 > ry + 20 ? y + 28 + CELL_H + 34 : ry + 20;
    if (b->d[0]) wrap_text(F_SERIF, 16, x + 28, dy, w - 56, 23, b->d, (y + h - 90 - dy) / 23, g_col[C_TEXT]);
    // the action
    const int have = have_book(b->id);
    const cdl_t *d = dl_of(b->id);
    const int by = y + h - 72;
    if (have >= 0) {
        pill(x + w - 28 - pill_w(tr(S_CAT_READ)), by, tr(S_CAT_READ), A_CAT_READ, have, 1);
    } else if (d && d->state != DL_FAILED) {
        const int pm = d->size > 0 ? (int)(d->done * 1000 / d->size) : 0;
        char pr[64];
        snprintf(pr, sizeof pr, d->state == DL_QUEUED ? "%s" : tr(S_CAT_PROGRESS), d->state == DL_QUEUED ? tr(S_CAT_QUEUED) : "");
        if (d->state != DL_QUEUED) snprintf(pr, sizeof pr, tr(S_CAT_PROGRESS), pm / 10);
        gfx_rrect(x + 28, by + 18, w - 56 - 200, 8, 4, g_col[C_RULE]);
        gfx_rrect(x + 28, by + 18, (w - 56 - 200) * pm / 1000, 8, 4, g_col[C_ACCENT]);
        text_draw(F_BOLD, 15, x + 28, by + 4, pr, (int)strlen(pr), g_col[C_TEXT]);
        const char *cl = C.dl_armed == d->id ? tr(S_DELETE_SURE) : tr(S_CAT_CANCEL);
        pill(x + w - 28 - pill_w(cl), by, cl, A_CAT_CANCEL, d->id, 0);
    } else {
        const char *g = d ? tr(S_CAT_RETRY) : tr(S_CAT_DOWNLOAD);
        const int pw = pill_w(g) + 36;
        const int r = reg_add(x + w - 28 - pw, by, pw, 44, A_CAT_GET, 0);
        gfx_rrect(x + w - 28 - pw, by, pw, 44, 22, reg_state(r) == 2 ? g_col[C_PRESS] : g_col[C_ACCENT]);
        if (reg_state(r) == 1) gfx_ring(x + w - 31 - pw, by - 3, pw + 6, 50, 25, 2, g_col[C_FOCUS]);
        icon_draw(IC_DOWNLOAD, x + w - 28 - pw + 16, by + 10, g_col[C_ON_ACCENT]);
        text_draw(F_BOLD, 17, x + w - 28 - pw + 48, by + 28, g, (int)strlen(g), g_col[C_ON_ACCENT]);
        if (d) text_draw(F_SANS, 15, x + 28, by + 28, tr(S_CAT_FAILED), (int)strlen(tr(S_CAT_FAILED)), g_col[C_MARK]);
    }
}

#define LROW 56

static void draw_list_sheet(void) {
    scrim();
    const int w = g_w - 32 < 520 ? g_w - 32 : 520, x = (g_w - w) / 2, y = 24, h = g_h - 48;
    sheet_frame(x, y, w, h);
    button(x + w - BTN - 8, y + 8, IC_CLOSE, A_CLOSE, 0, 1, g_col[C_DIM]);
    const char *t = A.sheet == SH_CAT_LANGS ? tr(S_CAT_LANGS) : A.sheet == SH_CAT_CATS ? tr(S_CAT_CATEGORY) : tr(S_CAT_DOWNLOADS);
    text_draw(F_BOLD, 20, x + 24, y + 40, t, (int)strlen(t), g_col[C_TEXT]);
    const int ly = y + 70;
    gfx_clip(x, ly, x + w, y + h - 8);
    if (A.sheet == SH_CAT_LANGS) {
        for (int i = 0; i < C.nlang; i++) {
            const int ry = ly + i * LROW - A.list_scroll;
            if (ry + LROW < ly || ry > y + h) continue;
            row_box(x + 8, ry + 2, w - 16, LROW - 4, A_CAT_LANG, i, C.sel[i] ? g_col[C_CARD] : g_col[C_BG]);
            char b[96];
            snprintf(b, sizeof b, "%s", lang_name(C.lang[i].code));
            text_draw(C.sel[i] ? F_BOLD : F_SANS, 17, x + 28, ry + 34, b, (int)strlen(b), g_col[C_TEXT]);
            snprintf(b, sizeof b, tr(S_CAT_BOOKS), C.lang[i].count);
            const int bw = text_width(F_SANS, 14, b, (int)strlen(b));
            text_draw(F_SANS, 14, x + w - 70 - bw, ry + 33, b, (int)strlen(b), g_col[C_DIM]);
            if (C.sel[i]) icon_draw(IC_CHECK, x + w - 52, ry + 15, g_col[C_ACCENT]);
        }
    } else if (A.sheet == SH_CAT_CATS) {
        for (int i = -1; i < C.ncat; i++) {
            const int ry = ly + (i + 1) * LROW - A.list_scroll;
            if (ry + LROW < ly || ry > y + h) continue;
            if (i >= 0 && !C.cat[i].count) continue;
            const int on = C.filter_cat == i;
            row_box(x + 8, ry + 2, w - 16, LROW - 4, A_CAT_CHIP, i + 1, on ? g_col[C_CARD] : g_col[C_BG]);
            const char *n = i < 0 ? tr(S_CAT_ALL) : C.cat[i].name;
            text_draw(on ? F_BOLD : F_SANS, 17, x + 28, ry + 34, n, (int)strlen(n), g_col[C_TEXT]);
            if (i >= 0) {
                char b[16];
                snprintf(b, sizeof b, "%d", C.cat[i].count);
                const int bw = text_width(F_SANS, 14, b, (int)strlen(b));
                text_draw(F_SANS, 14, x + w - 70 - bw, ry + 33, b, (int)strlen(b), g_col[C_DIM]);
            }
            if (on) icon_draw(IC_CHECK, x + w - 52, ry + 15, g_col[C_ACCENT]);
        }
    } else {
        if (!C.ndl) text_draw(F_SANS, 16, x + 28, ly + 30, tr(S_CAT_NONE), (int)strlen(tr(S_CAT_NONE)), g_col[C_DIM]);
        for (int i = 0; i < C.ndl; i++) {
            const cdl_t *d = &C.dl[i];
            const int ry = ly + i * 76 - A.list_scroll;
            if (ry + 76 < ly || ry > y + h) continue;
            text_draw_fit(F_BOLD, 16, x + 28, ry + 26, d->title, (int)strlen(d->title), w - 56 - BTN, g_col[C_TEXT]);
            const int pm = d->size > 0 ? (int)(d->done * 1000 / d->size) : 0;
            gfx_rrect(x + 28, ry + 42, w - 56 - BTN - 12, 6, 3, g_col[C_RULE]);
            gfx_rrect(x + 28, ry + 42, (w - 56 - BTN - 12) * pm / 1000, 6, 3, d->state == DL_FAILED ? g_col[C_MARK] : g_col[C_ACCENT]);
            char b[80], s1[24], s2[24];
            fmt_size((uint64_t)d->done, s1, sizeof s1);
            fmt_size((uint64_t)d->size, s2, sizeof s2);
            snprintf(b, sizeof b, "%s / %s%s%s", s1, s2, d->state == DL_FAILED ? "  \xC2\xB7  " : "",
                     d->state == DL_FAILED ? tr(S_CAT_FAILED) : "");
            text_draw(F_SANS, 13, x + 28, ry + 66, b, (int)strlen(b), g_col[C_DIM]);
            button(x + w - BTN - 12, ry + 12, IC_CLOSE, A_CAT_CANCEL, d->id, 1, C.dl_armed == d->id ? g_col[C_MARK] : g_col[C_DIM]);
        }
    }
    gfx_noclip();
}

// ------------------------------------------------------------------------------- input

int cat_action(int act, int arg) {
    A.idle_since = A.now;
    if (act != A_CAT_CANCEL) C.dl_armed = 0;
    switch (act) {
    case A_CAT_SEARCH:   // the query changed (arg 1: Enter)
        C.vis_dirty = 1;
        C.scroll = 0;
        A.redraw |= R_ALL;
        return 1;
    case A_CAT_RETRY: {
        C.failed = 0;
        C.shard_fails = 0;
        C.retry_at = nv_millis();
        if (!C.ready && C.net.kind == NET_IDLE) {
            char url[300];
            snprintf(url, sizeof url, "%sindex-v1.json", base_url());
            net_start(NET_INDEX, url, 0, 0, NULL);
        }
        return 1;
    }
    case A_CAT_ROW:
        if (arg < 0 || arg >= C.nrows) return 1;
        C.book_ok = book_detail(&C.rows[arg], &C.book) == 0;
        if (C.book_ok) { A.sheet = SH_CAT_BOOK; cover_of(&C.rows[arg]); }
        return 1;
    case A_CAT_GET: if (C.book_ok) download(&C.book); return 1;
    case A_CAT_READ: A.sheet = SH_NONE; reader_open(arg); return 1;
    case A_CAT_CANCEL:
        if (C.dl_armed != arg) { C.dl_armed = arg; toast(tr(S_DELETE_SURE), NULL); return 1; }
        C.dl_armed = 0;
        nv_dl_cancel(arg);
        dl_refresh();
        return 1;
    case A_CAT_LANGS: A.sheet = SH_CAT_LANGS; A.list_scroll = 0; return 1;
    case A_CAT_LANG: {
        if (arg < 0 || arg >= C.nlang) return 1;
        int n = 0;
        for (int i = 0; i < C.nlang; i++) n += C.sel[i];
        if (C.sel[arg] && n == 1) return 1;   // one language at least
        C.sel[arg] = !C.sel[arg];
        sel_to_settings();
        C.vis_dirty = 1;
        C.scroll = 0;
        return 1;
    }
    case A_CAT_CHIP:
        if (A.sheet != SH_CAT_CATS) { if (C.vis_dirty) rebuild_vis(); A.sheet = SH_CAT_CATS; A.list_scroll = 0; return 1; }
        C.filter_cat = arg - 1;
        A.sheet = SH_NONE;
        C.vis_dirty = 1;
        C.scroll = 0;
        return 1;
    case A_CAT_DLS: dl_refresh(); A.sheet = SH_CAT_DLS; A.list_scroll = 0; return 1;
    case A_CLOSE: A.sheet = SH_NONE; return 1;
    default: return 0;
    }
}

void cat_key(const kev_t *k) {
    const int step = k->key == K_PGDN || k->key == K_PGUP ? g_h - grid_top() - 40 : ROW_H / 2;
    if (k->key == K_DOWN || k->key == K_PGDN) C.scroll += step;
    else if (k->key == K_UP || k->key == K_PGUP) C.scroll -= step;
    else if (k->key == K_HOME) C.scroll = 0;
    else if (k->key == K_ENTER) { A.osk_on = 0; cat_action(A_CAT_SEARCH, 1); }
    else return;
    A.redraw |= R_ALL;
}

int32_t *cat_scroll(void) { return &C.scroll; }
int cat_height(void) { return grid_height() + grid_top(); }
