// get.c — the data behind the "Download collections" screen (docs/ZIM_READER_PLAN.md §4f): the Kiwix
// catalog over HTTP (ABI 12, kept in /appdata for a day), its collections grouped with their flavours,
// the 48 px thumbnails (one request at a time, kept on disk), and the downloads of the system downloader
// (ABI 18), which go on when the app is closed. Nothing here blocks: get_poll() advances the network
// work a step per frame. Drawing is app.c's.
#include "bib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "nucleo_sdk.h"

#define CAT_HOST "https://opds.library.kiwix.org"
#define CAT_PAGE 400            // entries per request: ~520 KB, under the 1 MB an HTTP answer may be
#define CAT_PAGES 5
#define CAT_MAX 2000
#define CAT_FRESH_S (24 * 3600)
#define ICON_MAX 96             // thumbnails kept in memory

cat_t *g_cat;
int g_ncat;
cat_group_t *g_grp;
int g_ngrp;
dl_t g_dl[DL_MAX];
int g_ndl;

static struct {
    int state;                  // GET_*
    char lang3[8];
    int total;                  // the feed's totalResults
    int pages;                  // pages fetched
    int h;                      // HTTP handle of the catalog page in flight, -1 none
    char *body;                 // what was read of it
    size_t blen, bcap;
    char *xml;                  // every page, for the cache
    size_t xlen;
    int err;                    // the last failure (NV_NET_E_* or an HTTP status)
    // thumbnails
    int ih, ient;               // request in flight and its entry
    char *ibody;
    size_t ilen;
    struct { int ent; uint8_t *rgba; } icons[ICON_MAX];
    int nicons, inext;
    uint8_t *tried;             // per entry: a thumbnail was asked for (CAT_MAX)
} G = {.h = -1, .ih = -1};

static int s_gen;   // bumps when the groups change

int get_state(void) { return G.state; }
int get_gen(void) { return s_gen; }
const char *get_lang3(void) { return G.lang3; }
int get_error(void) { return G.err; }


// ---------------------------------------------------------------------------------- grouping

static int by_name_rank(const void *a, const void *b) {
    const cat_t *x = a, *y = b;
    const int c = strcmp(x->name, y->name);
    if (c) return c;
    const int rx = cat_flavour_rank(x), ry = cat_flavour_rank(y);
    if (rx != ry) return rx - ry;
    return strcmp(y->date, x->date);    // the newest first
}

static const char *g_l2 = "";

static int group_order(const void *a, const void *b) {
    const cat_group_t *x = a, *y = b;
    if ((x->rec >= 0) != (y->rec >= 0)) return x->rec >= 0 ? -1 : 1;
    if (x->rec != y->rec) return x->rec - y->rec;
    return strcasecmp(g_cat[x->pick].title, g_cat[y->pick].title);
}

static void lang2_of(const char *l3, char *l2) {
    static const char *const k[][2] = {{"ita", "it"}, {"eng", "en"}, {"spa", "es"}, {"fra", "fr"}, {"deu", "de"}};
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
        if (!strcmp(l3, k[i][0])) { strcpy(l2, k[i][1]); return; }
    l2[0] = 0;
}

// g_cat sorted by name, one group per name; groups in Recommended order, then by title.
static void regroup(void) {
    qsort(g_cat, (size_t)g_ncat, sizeof *g_cat, by_name_rank);
    free(g_grp);
    g_grp = (cat_group_t *)calloc((size_t)(g_ncat ? g_ncat : 1), sizeof *g_grp);
    g_ngrp = 0;
    if (!g_grp) return;
    static char l2[4];
    lang2_of(G.lang3, l2);
    g_l2 = l2;
    for (int i = 0; i < g_ncat;) {
        int j = i + 1;
        while (j < g_ncat && !strcmp(g_cat[j].name, g_cat[i].name)) j++;
        cat_group_t *gr = &g_grp[g_ngrp++];
        gr->first = i;
        gr->n = j - i;
        gr->pick = i;                    // sorted: the flavour a list offers first
        gr->rec = cat_recommended(g_cat[i].name, l2);
        i = j;
    }
    qsort(g_grp, (size_t)g_ngrp, sizeof *g_grp, group_order);
    s_gen++;
    free(G.tried);
    G.tried = (uint8_t *)calloc((size_t)(g_ncat ? g_ncat : 1), 1);
    for (int i = 0; i < G.nicons; i++) free(G.icons[i].rgba);
    G.nicons = G.inext = 0;
}

static int parse_xml(const char *xml, size_t n) {
    free(g_cat);
    g_cat = (cat_t *)malloc(sizeof(cat_t) * CAT_MAX);
    if (!g_cat) { g_ncat = 0; return -1; }
    g_ncat = cat_parse(xml, n, g_cat, CAT_MAX, &G.total);
    // TED talks and other video collections are many and heavy: kept out of the lists
    int k = 0;
    for (int i = 0; i < g_ncat; i++)
        if (!g_cat[i].videos && strcmp(g_cat[i].category, "ted")) g_cat[k++] = g_cat[i];
    g_ncat = k;
    regroup();
    return g_ncat;
}

// ----------------------------------------------------------------------------- the catalog

static int start_page(void) {
    char spec[300];
    snprintf(spec, sizeof spec,
             "{\"url\":\"" CAT_HOST "/catalog/v2/entries?lang=%s&count=%d&start=%d\",\"timeout\":40000,\"max\":1048576}",
             G.lang3, CAT_PAGE, G.pages * CAT_PAGE);
    G.h = nv_http_req(spec, NULL, 0);
    G.blen = 0;
    if (G.h < 0) { G.err = G.h; return -1; }
    return 0;
}

// The cache: "<!--nvtime T-->" then the pages as they came.
static int load_cache(int max_age) {
    char p[200];
    snprintf(p, sizeof p, "%s/appdata/catalog-%s.xml", bib_root(), G.lang3);
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *x = n > 16 && n < 8 << 20 ? (char *)malloc((size_t)n + 1) : NULL;
    const int ok = x && fread(x, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if (!ok) { free(x); return 0; }
    x[n] = 0;
    long long t = 0;
    if (sscanf(x, "<!--nvtime %lld-->", &t) != 1 || (max_age >= 0 && nv_time_unix() - t > max_age)) { free(x); return 0; }
    const int k = parse_xml(x, (size_t)n);
    free(x);
    if (k <= 0) return 0;
    G.state = GET_READY;
    return 1;
}

static void save_cache(void) {
    char p[200], t[40];
    snprintf(p, sizeof p, "%s/appdata/catalog-%s.xml", bib_root(), G.lang3);
    FILE *f = fopen(p, "wb");
    if (!f) return;
    const int k = snprintf(t, sizeof t, "<!--nvtime %lld-->\n", (long long)nv_time_unix());
    fwrite(t, 1, (size_t)k, f);
    fwrite(G.xml, 1, G.xlen, f);
    fclose(f);
}

void get_open(const char *lang3, int force) {
    if (G.h >= 0) { nv_http_close(G.h); G.h = -1; }
    snprintf(G.lang3, sizeof G.lang3, "%s", lang3);
    G.pages = 0;
    G.err = 0;
    free(G.xml);
    G.xml = NULL;
    G.xlen = 0;
    if (!force && load_cache(CAT_FRESH_S)) return;
    // an old catalog is better than none while the new one comes (or if it never does); another
    // language's never is
    const int had = load_cache(-1);
    if (!had) {
        free(g_cat); g_cat = NULL; g_ncat = 0;
        free(g_grp); g_grp = NULL; g_ngrp = 0;
        s_gen++;
    }
    if (start_page() == 0) G.state = had ? GET_READY : GET_LOADING;
    else G.state = had ? GET_READY : GET_ERROR;
}

int get_busy(void) { return G.h >= 0; }

static int read_all(int h, char **buf, size_t *len, size_t *cap) {
    for (;;) {
        if (*len + 16384 + 1 > *cap) {
            const size_t nc = *cap ? *cap * 2 : 65536;
            char *nb = (char *)realloc(*buf, nc);
            if (!nb) return -1;
            *buf = nb;
            *cap = nc;
        }
        const int r = nv_http_read(h, *buf + *len, 16384);
        if (r < 0) return -1;
        if (r == 0) break;
        *len += (size_t)r;
    }
    (*buf)[*len] = 0;
    return 0;
}

static int poll_catalog(void) {
    if (G.h < 0) return 0;
    const int st = nv_http_state(G.h);
    if (st == 0) return 0;
    const int status = st > 0 ? nv_http_status(G.h) : 0;
    int ok = st > 0 && status == 200 && read_all(G.h, &G.body, &G.blen, &G.bcap) == 0;
    nv_http_close(G.h);
    G.h = -1;
    if (!ok) {
        G.err = st < 0 ? st : status;
        G.state = g_ncat ? GET_READY : GET_ERROR;   // what the cache had stays
        return 1;
    }
    char *nx = (char *)realloc(G.xml, G.xlen + G.blen + 1);
    if (!nx) { G.state = g_ncat ? GET_READY : GET_ERROR; return 1; }
    G.xml = nx;
    memcpy(G.xml + G.xlen, G.body, G.blen);
    G.xlen += G.blen;
    G.xml[G.xlen] = 0;
    G.pages++;
    int total = 0;
    cat_t probe;
    cat_parse(G.body, G.blen, &probe, 0, &total);
    if (G.pages == 1) G.total = total;
    if (G.total > G.pages * CAT_PAGE && G.pages < CAT_PAGES && start_page() == 0) return 1;
    parse_xml(G.xml, G.xlen);
    save_cache();
    free(G.xml);
    G.xml = NULL;
    G.xlen = 0;
    G.state = GET_READY;
    return 1;
}

// ------------------------------------------------------------------------------- thumbnails

static int icon_file(int ent, char *out, size_t cap) {
    const char *n = g_cat[ent].name;
    for (const char *c = n; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_' || *c == '-' || *c == '.'))
            return 0;
    snprintf(out, cap, "%s/appdata/icons/%s.png", bib_root(), n);
    return 1;
}

static void icon_keep(int ent, const uint8_t *data, size_t n) {
    uint8_t *rgba = data && n ? img_decode_rgba(data, n, 48, 48) : NULL;
    if (G.nicons == ICON_MAX) { free(G.icons[0].rgba); memmove(G.icons, G.icons + 1, sizeof G.icons[0] * (ICON_MAX - 1)); G.nicons--; }
    G.icons[G.nicons].ent = ent;
    G.icons[G.nicons].rgba = rgba;
    G.nicons++;
}

const uint8_t *get_icon(int ent) {
    if (ent < 0 || ent >= g_ncat || !G.tried) return NULL;
    for (int i = 0; i < G.nicons; i++)
        if (!strcmp(g_cat[G.icons[i].ent].name, g_cat[ent].name)) return G.icons[i].rgba;
    if (G.tried[ent]) return NULL;
    // not asked yet: from the card at once, else queued for the network
    char p[220];
    if (icon_file(ent, p, sizeof p)) {
        FILE *f = fopen(p, "rb");
        if (f) {
            uint8_t buf[16384];
            const size_t n = fread(buf, 1, sizeof buf, f);
            fclose(f);
            G.tried[ent] = 1;
            icon_keep(ent, buf, n);
            return G.icons[G.nicons - 1].rgba;
        }
    }
    G.tried[ent] = 2;   // wanted
    return NULL;
}

static int poll_icons(void) {
    if (G.ih >= 0) {
        const int st = nv_http_state(G.ih);
        if (st == 0) return 0;
        size_t cap = 0;
        int ok = st > 0 && nv_http_status(G.ih) == 200 && read_all(G.ih, &G.ibody, &G.ilen, &cap) == 0;
        nv_http_close(G.ih);
        G.ih = -1;
        if (ok && G.ient < g_ncat) {
            char p[220];
            if (icon_file(G.ient, p, sizeof p)) {
                char d[200];
                snprintf(d, sizeof d, "%s/appdata/icons", bib_root());
                mkdir(d, 0777);
                FILE *f = fopen(p, "wb");
                if (f) { fwrite(G.ibody, 1, G.ilen, f); fclose(f); }
            }
            icon_keep(G.ient, (const uint8_t *)G.ibody, G.ilen);
        }
        free(G.ibody);
        G.ibody = NULL;
        G.ilen = 0;
        return ok;
    }
    if (G.h >= 0 || !G.tried) return 0;           // the catalog first
    for (int i = 0; i < g_ncat; i++) {
        if (G.tried[i] != 2) continue;
        G.tried[i] = 1;
        if (!g_cat[i].icon[0]) continue;
        char spec[300];
        snprintf(spec, sizeof spec, "{\"url\":\"" CAT_HOST "%s\",\"timeout\":20000,\"max\":65536}", g_cat[i].icon);
        G.ih = nv_http_req(spec, NULL, 0);
        G.ient = i;
        if (G.ih >= 0) return 0;
    }
    return 0;
}

int get_poll(void) { return poll_catalog() | poll_icons(); }

// ------------------------------------------------------------------------------- downloads

static const char *json_find(const char *o, const char *e, const char *key) {
    char k[40];
    snprintf(k, sizeof k, "\"%s\":", key);
    const size_t kl = strlen(k);
    for (const char *p = o; p + kl <= e; p++)
        if (*p == '"' && !memcmp(p, k, kl)) return p + kl;
    return NULL;
}

static long long json_num(const char *o, const char *e, const char *key) {
    const char *v = json_find(o, e, key);
    return v ? strtoll(v, NULL, 10) : 0;
}

static void json_str(const char *o, const char *e, const char *key, char *out, size_t cap) {
    const char *v = json_find(o, e, key);
    size_t k = 0;
    if (v && *v == '"') {
        for (v++; v < e && *v != '"' && k + 1 < cap; v++) {
            char c = *v;
            if (c == '\\' && v + 1 < e) {
                c = *++v;
                if (c == 'n' || c == 't' || c == 'r') c = ' ';
                else if (c == 'u') {   // \u00XX only (our strings are ASCII or UTF-8 as is)
                    c = (v + 4 < e) ? (char)strtol((char[3]){v[3], v[4], 0}, NULL, 16) : '?';
                    v += 4;
                }
            }
            out[k++] = c;
        }
    }
    out[k] = 0;
}

static int state_of(const char *s) {
    static const char *const k[] = {"queued", "paused", "done", "failed", "running"};
    for (int i = 0; i < 5; i++) if (!strcmp(s, k[i])) return i;
    return DL_QUEUED;
}

int get_dl_refresh(int *finished, int *failed) {
    static char buf[DL_MAX * 700 + 64];
    *finished = *failed = 0;
    const int n = nv_dl_list(buf, sizeof buf);
    if (n < 0) return 0;
    buf[n] = 0;
    dl_t prev[DL_MAX];
    const int np = g_ndl;
    memcpy(prev, g_dl, sizeof prev);
    g_ndl = 0;
    int changed = 0;
    for (const char *o = strchr(buf, '{'); o && g_ndl < DL_MAX; o = strchr(o + 1, '{')) {
        const char *e = strchr(o, '}');
        if (!e) break;
        dl_t *d = &g_dl[g_ndl++];
        memset(d, 0, sizeof *d);
        char st[16];
        d->id = (int)json_num(o, e, "id");
        json_str(o, e, "state", st, sizeof st);
        d->state = state_of(st);
        json_str(o, e, "title", d->title, sizeof d->title);
        json_str(o, e, "dest", d->dest, sizeof d->dest);
        json_str(o, e, "error", d->error, sizeof d->error);
        json_str(o, e, "phase", d->phase, sizeof d->phase);
        d->size = json_num(o, e, "size");
        d->done = json_num(o, e, "done");
        d->bps = (int32_t)json_num(o, e, "bps");
        int seen = 0;
        for (int i = 0; i < np; i++) {
            if (prev[i].id != d->id) continue;
            seen = 1;
            if (prev[i].state != d->state || prev[i].done != d->done || prev[i].bps != d->bps) changed = 1;
            if (prev[i].state != DL_DONE && d->state == DL_DONE) (*finished)++;
            if (prev[i].state != DL_FAILED && d->state == DL_FAILED) (*failed)++;
        }
        if (!seen) changed = 1;
        o = e;
    }
    return changed || np != g_ndl;
}

int get_dl_start(const cat_t *c) {
    char file[128], title[160], spec[700];
    if (!cat_file_name(c, file, sizeof file)) return NV_DL_E_ARG;
    const char *fl = !strcmp(c->flavour, "nopic") ? tr(S_GET_NOPIC) : !strcmp(c->flavour, "mini") ? tr(S_GET_MINI)
                   : !strcmp(c->flavour, "maxi") ? tr(S_GET_MAXI) : "";
    if (fl[0]) {   // "Il meglio di Wikipedia (senza immagini)"
        char low[48];
        snprintf(low, sizeof low, "%s", fl);
        if (low[0] >= 'A' && low[0] <= 'Z' && strcmp(tr(S_GET_MAXI), "Mit Bildern")) low[0] = (char)(low[0] + 32);
        snprintf(title, sizeof title, "%s (%s)", c->title, low);
    } else {
        snprintf(title, sizeof title, "%s", c->title);
    }
    char esc[200];
    size_t k = 0;
    for (const char *s = title; *s && k + 3 < sizeof esc; s++) {
        if (*s == '"' || *s == '\\') esc[k++] = '\\';
        esc[k++] = (unsigned char)*s < ' ' ? ' ' : *s;
    }
    esc[k] = 0;
    char dir[200];
    snprintf(dir, sizeof dir, "%s/zim", bib_root());
    mkdir(dir, 0777);
    snprintf(spec, sizeof spec, "{\"meta4\":\"%s\",\"dest\":\"/zim/%s\",\"title\":\"%s\"}", c->meta4, file, esc);
    return nv_dl_start(spec);
}

int get_dl_of(const cat_t *c) {
    char file[128], dest[160];
    if (!cat_file_name(c, file, sizeof file)) return -1;
    snprintf(dest, sizeof dest, "/zim/%s", file);
    for (int i = g_ndl - 1; i >= 0; i--)
        if (!strcmp(g_dl[i].dest, dest)) return i;
    return -1;
}

int64_t get_room(void) {
    const int32_t mb = nv_dl_free();
    return mb < 0 ? -1 : (int64_t)mb << 20;
}

// 1: the library has this very file; 2: it has an older edition of the same collection and flavour.
int get_have(const cat_t *c) {
    char file[128];
    if (!cat_file_name(c, file, sizeof file)) return 0;
    if (lib_find(file) >= 0) return 1;
    char stem[96];
    snprintf(stem, sizeof stem, "%s_%s%s", c->name, c->flavour, c->flavour[0] ? "_" : "");
    const size_t sl = strlen(stem);
    for (int i = 0; i < g_nlib; i++) {
        if (strncmp(g_lib[i].file, stem, sl)) continue;
        const char *d = g_lib[i].file + sl;   // "2026-07.zim"
        if (strlen(d) >= 7 && strncmp(d, c->date, 7) < 0) return 2;
        if (strlen(d) >= 7) return 1;
    }
    return 0;
}
