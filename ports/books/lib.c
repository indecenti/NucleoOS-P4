// lib.c — what the app keeps: the books found in ~/books and one folder below (their metadata
// cached, so the library opens without reading every file), the covers (RGB565 thumbnails in
// /appdata/covers), the reading positions, the bookmarks and the settings. On the device "~" is "/"
// (permission "home") and the data folder is "/appdata" ("fs"); the PC tests put both under $BK_ROOT.
// Every file is written whole to a .tmp and renamed over the old one: the system may close the app
// at any moment.
#include "bk.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "nucleo_sdk.h"

entry_t *g_lib;
int g_nlib;
settings_t g_set;

const char *bk_root(void) {
#ifdef BK_NATIVE
    const char *r = getenv("BK_ROOT");
    return r ? r : ".";
#else
    return "";
#endif
}

const char *books_dir(void) {
    static char d[256];
    if (!d[0]) snprintf(d, sizeof d, "%s/books", bk_root());
    return d;
}

static void path_of(char *out, size_t cap, const char *rel) { snprintf(out, cap, "%s%s", bk_root(), rel); }

static void clean(char *s) {   // one TSV field
    for (; *s; s++)
        if (*s == '\t' || *s == '\n' || *s == '\r') *s = ' ';
}

static uint32_t fnv(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

static FILE *open_tmp(const char *rel, char *final, size_t cap) {
    char d[256];
    path_of(d, sizeof d, "/appdata");
    mkdir(d, 0777);
    path_of(final, cap, rel);
    char tmp[300];
    snprintf(tmp, sizeof tmp, "%s.tmp", final);
    return fopen(tmp, "w");
}

static void commit_tmp(FILE *f, const char *final) {
    char tmp[300];
    snprintf(tmp, sizeof tmp, "%s.tmp", final);
    const int bad = ferror(f);
    fclose(f);
    if (bad) { remove(tmp); return; }
    remove(final);   // FatFs: rename does not replace
    rename(tmp, final);
}

// ------------------------------------------------------------------------------- the library

static int cmp_name(const void *a, const void *b) { return strcasecmp(((const entry_t *)a)->file, ((const entry_t *)b)->file); }

static int s_cap;   // entries g_lib has room for (it grows by 64)

static void add_file(const char *rel, const struct stat *st) {
    if (g_nlib >= LIB_MAX || book_kind_of(rel) < 0) return;
    if (g_nlib == s_cap) {
        entry_t *n = (entry_t *)realloc(g_lib, sizeof(entry_t) * (size_t)(s_cap + 64));
        if (!n) return;
        g_lib = n;
        s_cap += 64;
    }
    entry_t *e = &g_lib[g_nlib++];
    memset(e, 0, sizeof *e);
    snprintf(e->file, sizeof e->file, "%s", rel);
    e->size = (int64_t)st->st_size;
    e->mtime = (int64_t)st->st_mtime;
    e->kind = (uint8_t)book_kind_of(rel);
    e->permille = -1;
    e->nch = -1;   // metadata not read yet
    // a title from the name until the book is read
    const char *base = strrchr(rel, '/');
    snprintf(e->title, sizeof e->title, "%s", base ? base + 1 : rel);
    char *dot = strrchr(e->title, '.');
    if (dot && dot != e->title) *dot = 0;
    for (char *p = e->title; *p; p++) if (*p == '_') *p = ' ';
    e->hue = fnv(rel);
}

static void scan_dir(const char *dir, const char *prefix, int depth) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) && g_nlib < LIB_MAX) {
        if (de->d_name[0] == '.') continue;
        char p[600], rel[400];
        snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        snprintf(rel, sizeof rel, "%s%s", prefix, de->d_name);
        if (strlen(rel) >= sizeof g_lib[0].file) continue;
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 1) {
                char pre[400];
                snprintf(pre, sizeof pre, "%s/", rel);
                scan_dir(p, pre, depth + 1);
            }
            continue;
        }
        add_file(rel, &st);
    }
    closedir(d);
}

// library.tsv: file, size, mtime, title, author, lang, kind, fixed, chapters
static void cache_load(void) {
    char p[300];
    path_of(p, sizeof p, "/appdata/library.tsv");
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *fld[9];
        int n = 0;
        for (char *s = line; n < 9;) {
            fld[n++] = s;
            char *t = strchr(s, '\t');
            if (!t) break;
            *t = 0;
            s = t + 1;
        }
        if (n < 9) continue;
        const int i = lib_find(fld[0]);
        if (i < 0) continue;
        entry_t *e = &g_lib[i];
        if (e->size != strtoll(fld[1], NULL, 10) || e->mtime != strtoll(fld[2], NULL, 10)) continue;   // changed
        snprintf(e->title, sizeof e->title, "%s", fld[3]);
        snprintf(e->author, sizeof e->author, "%s", fld[4]);
        snprintf(e->lang, sizeof e->lang, "%s", fld[5]);
        e->kind = (uint8_t)atoi(fld[6]);
        e->fixed = (uint8_t)atoi(fld[7]);
        e->nch = atoi(fld[8]);
        e->bad = e->nch == 0;
    }
    fclose(f);
}

void lib_save(void) {
    char final[300];
    FILE *f = open_tmp("/appdata/library.tsv", final, sizeof final);
    if (!f) return;
    for (int i = 0; i < g_nlib; i++) {
        const entry_t *e = &g_lib[i];
        if (e->nch < 0) continue;   // not read yet
        fprintf(f, "%s\t%lld\t%lld\t%s\t%s\t%s\t%d\t%d\t%d\n", e->file, (long long)e->size, (long long)e->mtime, e->title,
                e->author, e->lang, e->kind, e->fixed, e->bad ? 0 : (int)e->nch);
    }
    commit_tmp(f, final);
}

static void positions_into_lib(void);

int lib_scan(int (*progress)(void *ctx, int done, int total), void *ctx) {
    g_nlib = 0;
    mkdir(books_dir(), 0777);
    if (progress) progress(ctx, 0, 1);
    scan_dir(books_dir(), "", 0);
    if (g_nlib > 1) qsort(g_lib, (size_t)g_nlib, sizeof(entry_t), cmp_name);
    cache_load();
    positions_into_lib();
    return g_nlib;
}

int lib_find(const char *file) {
    int lo = 0, hi = g_nlib - 1;   // sorted by name, any case
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const int c = strcasecmp(g_lib[mid].file, file);
        if (!c) return strcmp(g_lib[mid].file, file) ? -1 : mid;
        if (c < 0) lo = mid + 1;
        else hi = mid - 1;
    }
    for (int i = 0; i < g_nlib; i++) if (!strcmp(g_lib[i].file, file)) return i;
    return -1;
}

void lib_update(int i, const book_t *b) {
    if (i < 0 || i >= g_nlib) return;
    entry_t *e = &g_lib[i];
    if (!b) { e->bad = 1; e->nch = 0; return; }
    snprintf(e->title, sizeof e->title, "%s", b->title);
    snprintf(e->author, sizeof e->author, "%s", b->author);
    snprintf(e->lang, sizeof e->lang, "%s", b->lang);
    clean(e->title);
    clean(e->author);
    clean(e->lang);
    e->kind = (uint8_t)b->kind;
    e->fixed = (uint8_t)b->fixed;
    e->nch = b->nch;
    e->bad = 0;
}

// ------------------------------------------------------------------------------------- covers

static void cover_path(const entry_t *e, char *out, size_t cap) {
    char rel[200];
    snprintf(rel, sizeof rel, "/appdata/covers/%08x_%lld.565", (unsigned)fnv(e->file), (long long)e->size);
    path_of(out, cap, rel);
}

uint16_t *lib_cover(int i, book_t *b) {
    if (i < 0 || i >= g_nlib) return NULL;
    char p[300];
    cover_path(&g_lib[i], p, sizeof p);
    const size_t bytes = (size_t)COVER_W * COVER_H * 2;
    FILE *f = fopen(p, "rb");
    if (f) {
        uint16_t *px = (uint16_t *)malloc(bytes);
        const int ok = px && fread(px, 1, bytes, f) == bytes;
        fclose(f);
        if (ok) return px;
        free(px);
    }
    if (!b) return NULL;
    uint8_t *data;
    size_t n;
    uint16_t *px = NULL;
    if (!book_cover(b, &data, &n)) {
        const int keep = g_rmode;
        g_rmode = RM_COLOR;   // the cache holds colour: the library converts when it draws
        px = img_render565(data, n, COVER_W, COVER_H, rgb565(0x30, 0x34, 0x3A), 1);
        g_rmode = keep;
        free(data);
    }
    if (!px) {   // not decodable here (a progressive JPEG too big to decode whole): the catalog's thumbnail
        char stem[200], cp[300];
        const char *base = strrchr(g_lib[i].file, '/');
        snprintf(stem, sizeof stem, "%s", base ? base + 1 : g_lib[i].file);
        char *dot = strrchr(stem, '.');
        if (dot) *dot = 0;
        snprintf(cp, sizeof cp, "%s/appdata/ccov/%s.565", bk_root(), stem);
        FILE *cf = fopen(cp, "rb");
        if (cf) {
            px = (uint16_t *)malloc(bytes);
            if (px && fread(px, 1, bytes, cf) != bytes) { free(px); px = NULL; }
            fclose(cf);
        }
    }
    if (!px) return NULL;
    char d[300];
    path_of(d, sizeof d, "/appdata/covers");
    mkdir(d, 0777);
    char tmp[310];
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    f = fopen(tmp, "wb");
    if (f) {
        const int ok = fwrite(px, 1, bytes, f) == bytes;
        fclose(f);
        if (ok) { remove(p); rename(tmp, p); }
        else remove(tmp);
    }
    return px;
}

// ----------------------------------------------------------------------------------- positions

// pos.tsv: file, chapter, offset, permille, last opened (unix time)
typedef struct { char file[200]; pos_t p; int64_t when; } prow_t;

static int pos_rows(prow_t **out) {
    *out = NULL;
    char p[300];
    path_of(p, sizeof p, "/appdata/pos.tsv");
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    int n = 0, cap = 0;
    prow_t *r = NULL;
    char line[400];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *t1 = strchr(line, '\t');
        if (!t1) continue;
        *t1 = 0;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            prow_t *nr = (prow_t *)realloc(r, sizeof(prow_t) * (size_t)cap);
            if (!nr) break;
            r = nr;
        }
        prow_t *e = &r[n];
        memset(e, 0, sizeof *e);
        snprintf(e->file, sizeof e->file, "%s", line);
        long long ch = 0, off = 0, pm = 0, when = 0;
        if (sscanf(t1 + 1, "%lld\t%lld\t%lld\t%lld", &ch, &off, &pm, &when) < 3) continue;
        e->p.chap = (int32_t)ch;
        e->p.off = (int32_t)off;
        e->p.permille = (int16_t)pm;
        e->when = when;
        n++;
    }
    fclose(f);
    *out = r;
    return n;
}

static void positions_into_lib(void) {
    prow_t *r;
    const int n = pos_rows(&r);
    for (int i = 0; i < n; i++) {
        const int k = lib_find(r[i].file);
        if (k < 0) continue;
        g_lib[k].permille = r[i].p.permille;
        g_lib[k].opened = r[i].when;
    }
    free(r);
}

int pos_load(const char *file, pos_t *p) {
    prow_t *r;
    const int n = pos_rows(&r);
    int found = -1;
    for (int i = 0; i < n; i++)
        if (!strcmp(r[i].file, file)) { *p = r[i].p; found = 0; }
    free(r);
    return found;
}

void pos_save(const char *file, const pos_t *p) {
    prow_t *r;
    const int n = pos_rows(&r);
    char final[300];
    FILE *f = open_tmp("/appdata/pos.tsv", final, sizeof final);
    if (!f) { free(r); return; }
    const int64_t now = nv_time_unix();
    fprintf(f, "%s\t%d\t%d\t%d\t%lld\n", file, (int)p->chap, (int)p->off, (int)p->permille, (long long)now);
    for (int i = 0; i < n; i++)
        if (strcmp(r[i].file, file) && lib_find(r[i].file) >= 0)   // books still there
            fprintf(f, "%s\t%d\t%d\t%d\t%lld\n", r[i].file, (int)r[i].p.chap, (int)r[i].p.off, (int)r[i].p.permille,
                    (long long)r[i].when);
    commit_tmp(f, final);
    free(r);
    const int k = lib_find(file);
    if (k >= 0) { g_lib[k].permille = p->permille; g_lib[k].opened = now; }
}

// ----------------------------------------------------------------------------------- bookmarks

// marks.tsv: file, chapter, offset, permille, when, the text there
static int mark_rows(const char *only, mark_t **out, char (**files)[200]) {
    *out = NULL;
    if (files) *files = NULL;
    char p[300];
    path_of(p, sizeof p, "/appdata/marks.tsv");
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    int n = 0, cap = 0;
    mark_t *r = NULL;
    char (*fl)[200] = NULL;
    char line[700];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *fld[6];
        int k = 0;
        for (char *s = line; k < 6;) {
            fld[k++] = s;
            char *t = strchr(s, '\t');
            if (!t) break;
            *t = 0;
            s = t + 1;
        }
        if (k < 6 || (only && strcmp(fld[0], only))) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            mark_t *nr = (mark_t *)realloc(r, sizeof(mark_t) * (size_t)cap);
            if (!nr) break;
            r = nr;
            if (files) {
                char (*nf)[200] = (char(*)[200])realloc(fl, 200 * (size_t)cap);
                if (!nf) break;
                fl = nf;
            }
        }
        mark_t *m = &r[n];
        memset(m, 0, sizeof *m);
        m->at.chap = atoi(fld[1]);
        m->at.off = atoi(fld[2]);
        m->at.permille = (int16_t)atoi(fld[3]);
        m->when = strtoll(fld[4], NULL, 10);
        snprintf(m->note, sizeof m->note, "%s", fld[5]);
        if (files) snprintf(fl[n], 200, "%s", fld[0]);
        n++;
    }
    fclose(f);
    *out = r;
    if (files) *files = fl;
    return n;
}

static int cmp_mark(const void *a, const void *b) {
    const mark_t *x = (const mark_t *)a, *y = (const mark_t *)b;
    return x->at.chap != y->at.chap ? x->at.chap - y->at.chap : x->at.off - y->at.off;
}

int marks_load(const char *file, mark_t **out) {
    const int n = mark_rows(file, out, NULL);
    if (n > 1) qsort(*out, (size_t)n, sizeof(mark_t), cmp_mark);
    return n;
}

static int marks_write(mark_t *r, char (*fl)[200], int n, const char *skip_file, const pos_t *skip, const char *add_file,
                       const mark_t *add) {
    char final[300];
    FILE *f = open_tmp("/appdata/marks.tsv", final, sizeof final);
    if (!f) return -1;
    for (int i = 0; i < n; i++) {
        if (skip && !strcmp(fl[i], skip_file) && r[i].at.chap == skip->chap && r[i].at.off == skip->off) continue;
        fprintf(f, "%s\t%d\t%d\t%d\t%lld\t%s\n", fl[i], (int)r[i].at.chap, (int)r[i].at.off, (int)r[i].at.permille,
                (long long)r[i].when, r[i].note);
    }
    if (add) {
        char note[200];
        snprintf(note, sizeof note, "%s", add->note);
        clean(note);
        fprintf(f, "%s\t%d\t%d\t%d\t%lld\t%s\n", add_file, (int)add->at.chap, (int)add->at.off, (int)add->at.permille,
                (long long)add->when, note);
    }
    commit_tmp(f, final);
    return 0;
}

int marks_add(const char *file, const mark_t *m) {
    mark_t *r;
    char (*fl)[200];
    const int n = mark_rows(NULL, &r, &fl);
    const int rc = marks_write(r, fl, n, NULL, NULL, file, m);
    free(r);
    free(fl);
    return rc;
}

int marks_remove(const char *file, const pos_t *at) {
    mark_t *r;
    char (*fl)[200];
    const int n = mark_rows(NULL, &r, &fl);
    const int rc = marks_write(r, fl, n, file, at, NULL, NULL);
    free(r);
    free(fl);
    return rc;
}

int marks_find(const char *file, int32_t chap, int32_t off0, int32_t off1) {
    mark_t *r;
    const int n = mark_rows(file, &r, NULL);
    int found = -1;
    for (int i = 0; i < n && found < 0; i++)
        if (r[i].at.chap == chap && r[i].at.off >= off0 && r[i].at.off < off1) found = i;
    free(r);
    return found;
}

int lib_remove(int i) {
    if (i < 0 || i >= g_nlib) return -1;
    char p[600];
    snprintf(p, sizeof p, "%s/%s", books_dir(), g_lib[i].file);
    if (remove(p) != 0) return -1;
    cover_path(&g_lib[i], p, sizeof p);
    remove(p);
    memmove(&g_lib[i], &g_lib[i + 1], sizeof(entry_t) * (size_t)(g_nlib - i - 1));
    g_nlib--;
    lib_save();
    return 0;
}

// ----------------------------------------------------------------------------------- settings

static void defaults(void) {
    memset(&g_set, 0, sizeof g_set);
    g_set.theme = TH_WHITE;
    g_set.size = 22;
    g_set.serif = 1;
    g_set.justify = 1;
    g_set.hyphen = 1;
    g_set.indent = 1;
    g_set.spacing = 1;
    g_set.margin = 1;
    g_set.anim = 0;
    g_set.eink_every = 6;
    g_set.tap_zones = 1;
    g_set.status = 1;
}

void settings_load(void) {
    defaults();
    char p[300];
    path_of(p, sizeof p, "/appdata/settings.txt");
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[400];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        const int iv = atoi(v);
        if (!strcmp(k, "theme") && iv >= 0 && iv < TH_COUNT) g_set.theme = iv;
        else if (!strcmp(k, "size") && iv >= 12 && iv <= 48) g_set.size = iv;
        else if (!strcmp(k, "serif")) g_set.serif = !!iv;
        else if (!strcmp(k, "justify")) g_set.justify = !!iv;
        else if (!strcmp(k, "hyphen")) g_set.hyphen = !!iv;
        else if (!strcmp(k, "indent")) g_set.indent = !!iv;
        else if (!strcmp(k, "spacing") && iv >= 0 && iv <= 3) g_set.spacing = iv;
        else if (!strcmp(k, "margin") && iv >= 0 && iv <= 3) g_set.margin = iv;
        else if (!strcmp(k, "anim") && iv >= 0 && iv <= 2) g_set.anim = iv;
        else if (!strcmp(k, "cols") && iv >= 0 && iv <= 2) g_set.cols = iv;
        else if (!strcmp(k, "portrait")) g_set.portrait = !!iv;
        else if (!strcmp(k, "eink_every") && iv >= 1 && iv <= 10) g_set.eink_every = iv;
        else if (!strcmp(k, "sort") && iv >= 0 && iv <= 3) g_set.sort = iv;
        else if (!strcmp(k, "tap_zones")) g_set.tap_zones = !!iv;
        else if (!strcmp(k, "status")) g_set.status = !!iv;
        else if (!strcmp(k, "last_file")) snprintf(g_set.last_file, sizeof g_set.last_file, "%s", v);
        else if (!strcmp(k, "cat_langs")) snprintf(g_set.cat_langs, sizeof g_set.cat_langs, "%s", v);
    }
    fclose(f);
}

void settings_save(void) {
    char final[300];
    FILE *f = open_tmp("/appdata/settings.txt", final, sizeof final);
    if (!f) return;
    fprintf(f, "theme=%d\nsize=%d\nserif=%d\njustify=%d\nhyphen=%d\nindent=%d\nspacing=%d\nmargin=%d\nanim=%d\n"
               "cols=%d\nportrait=%d\neink_every=%d\nsort=%d\ntap_zones=%d\nstatus=%d\nlast_file=%s\ncat_langs=%s\n",
            g_set.theme, g_set.size, g_set.serif, g_set.justify, g_set.hyphen, g_set.indent, g_set.spacing, g_set.margin,
            g_set.anim, g_set.cols, g_set.portrait, g_set.eink_every, g_set.sort, g_set.tap_zones, g_set.status, g_set.last_file, g_set.cat_langs);
    commit_tmp(f, final);
}
