// lib.c — what the app keeps: the collections found in ~/zim (with a cache of their metadata and
// covers in the app's data folder, so the library opens without reading every file), the history,
// the bookmarks and the settings. On the device "~" is "/" (permission "home") and the data folder
// is "/appdata" ("fs"); the PC tests put both under $BIB_ROOT.
#include "bib.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "lodepng.h"
#include "nucleo_sdk.h"

coll_t *g_lib;
int g_nlib;
settings_t g_set = {2, 0, "", "", 0};

const char *bib_root(void) {
#ifdef BIB_NATIVE
    const char *r = getenv("BIB_ROOT");
    return r ? r : ".";
#else
    return "";
#endif
}

static void path_of(char *out, size_t cap, const char *rel) { snprintf(out, cap, "%s%s", bib_root(), rel); }

static void clean(char *s) {   // one line of a TSV field
    for (; *s; s++)
        if (*s == '\t' || *s == '\n' || *s == '\r') *s = ' ';
}

int lib_meta(nvzim_t *z, const char *name, char *buf, size_t cap) {
    buf[0] = 0;
    const int n = nvzim_metadata(z, name, buf, cap);
    if (n < 0) buf[0] = 0;
    clean(buf);
    return n < 0 ? n : 0;   // NVZIM_ENOTFOUND: the file has no such field
}

// ------------------------------------------------------------------------------------- covers

// The 48x48 picture of a collection (PNG): RGBA, scaled when it is another size.
static uint8_t *cover_from_zim(nvzim_t *z) {
    nvzim_entry_t e;
    const char *const kNames[] = {"Illustration_48x48@1", "Illustration_48x48", NULL};
    int found = -1;
    for (int i = 0; kNames[i] && found < 0; i++) found = nvzim_find(z, 'M', kNames[i], &e);
    if (found < 0) found = nvzim_find(z, '-', "favicon", &e);   // ZIM 5
    if (found < 0 || nvzim_resolve(z, &e) < 0) return NULL;
    nvzim_blob_t b;
    if (nvzim_read(z, &e, &b) < 0) return NULL;
    uint8_t *rgba = NULL, *out = NULL;
    unsigned w = 0, h = 0;
    if (b.size > 8 && b.size < 512 * 1024 && !memcmp(b.data, "\x89PNG", 4) &&
        !lodepng_decode32(&rgba, &w, &h, b.data, (size_t)b.size) && w && h && w <= 1024 && h <= 1024) {
        out = (uint8_t *)malloc(COVER_PX * COVER_PX * 4);
        if (out)
            for (int y = 0; y < COVER_PX; y++)   // box filter (or nearest when enlarging)
                for (int x = 0; x < COVER_PX; x++) {
                    unsigned x0 = x * w / COVER_PX, x1 = (x + 1) * w / COVER_PX;
                    unsigned y0 = y * h / COVER_PX, y1 = (y + 1) * h / COVER_PX;
                    if (x1 <= x0) x1 = x0 + 1;
                    if (y1 <= y0) y1 = y0 + 1;
                    unsigned acc[4] = {0}, n = 0;
                    for (unsigned yy = y0; yy < y1 && yy < h; yy++)
                        for (unsigned xx = x0; xx < x1 && xx < w; xx++, n++)
                            for (int c = 0; c < 4; c++) acc[c] += rgba[(yy * w + xx) * 4 + c];
                    for (int c = 0; c < 4; c++) out[(y * COVER_PX + x) * 4 + c] = (uint8_t)(n ? acc[c] / n : 0);
                }
    }
    free(rgba);
    nvzim_blob_release(z, &b);
    return out;
}

static void cover_path(char *out, size_t cap, const char *file) {
    char rel[200];
    snprintf(rel, sizeof rel, "/appdata/covers/%s.rgba", file);
    path_of(out, cap, rel);
}

static uint8_t *cover_load(const char *file) {
    char p[300];
    cover_path(p, sizeof p, file);
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    uint8_t *c = (uint8_t *)malloc(COVER_PX * COVER_PX * 4);
    if (c && fread(c, 1, COVER_PX * COVER_PX * 4, f) != COVER_PX * COVER_PX * 4) { free(c); c = NULL; }
    fclose(f);
    return c;
}

static void cover_save(const char *file, const uint8_t *c) {
    char p[300];
    path_of(p, sizeof p, "/appdata/covers");
    mkdir(p, 0777);
    cover_path(p, sizeof p, file);
    FILE *f = fopen(p, "wb");
    if (!f) return;
    fwrite(c, 1, COVER_PX * COVER_PX * 4, f);
    fclose(f);
}

// ----------------------------------------------------------------------------------- the scan

static long articles_of(nvzim_t *z) {
    char counter[1024];
    if (lib_meta(z, "Counter", counter, sizeof counter) == 0) {   // "text/html=1234;image/png=..."
        const char *p = strstr(counter, "text/html=");
        if (p) return strtol(p + 10, NULL, 10);
    }
    const int n = nvzim_title_count(z);
    return n > 0 ? n : 0;
}

static void read_collection(coll_t *c) {
    nvzim_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.clusters = 1;
    cfg.cache_pages = 16;
    nvzim_t *z = NULL;
    c->bad = nvzim_open(&z, c->path, &cfg) != NVZIM_OK;
    if (c->bad) return;
    const int r[] = {lib_meta(z, "Title", c->title, sizeof c->title),
                     lib_meta(z, "Description", c->desc, sizeof c->desc),
                     lib_meta(z, "Language", c->lang, sizeof c->lang), lib_meta(z, "Date", c->date, sizeof c->date),
                     lib_meta(z, "Name", c->name, sizeof c->name)};
    for (size_t i = 0; i < sizeof r / sizeof r[0]; i++)
        if (r[i] < 0 && r[i] != NVZIM_ENOTFOUND) {
            c->retry = 1;
            snprintf(c->desc, sizeof c->desc, "%s", nvzim_strerror(r[i]));   // shown on the card
        }
    c->articles = articles_of(z);
    c->cover = cover_from_zim(z);
    if (c->cover) cover_save(c->file, c->cover);
    nvzim_close(z);
}

static int is_collection(const char *name) {
    const size_t n = strlen(name);
    if (name[0] == '.') return 0;
    if (n > 4 && !strcasecmp(name + n - 4, ".zim")) return 1;
    return n > 6 && !strcasecmp(name + n - 6, ".zimaa");   // the first part of a split file
}

static int coll_cmp(const void *a, const void *b) {
    const coll_t *x = (const coll_t *)a, *y = (const coll_t *)b;
    const char *tx = x->title[0] ? x->title : x->file, *ty = y->title[0] ? y->title : y->file;
    return strcasecmp(tx, ty);
}

#define CACHE_FIELDS 11
#define CACHE_HEAD "#biblioteca library 2\n"   // a cache written by another version is read again

int lib_scan(int (*progress)(void *ctx, int done, int total), void *ctx) {
    for (int i = 0; i < g_nlib; i++) free(g_lib[i].cover);
    free(g_lib);
    g_lib = NULL;
    g_nlib = 0;
    char dir[200];
    path_of(dir, sizeof dir, "/zim");
    DIR *dp = opendir(dir);
    if (!dp) return 0;
    coll_t *list = (coll_t *)calloc(LIB_MAX, sizeof(coll_t));
    if (!list) { closedir(dp); return -1; }
    int n = 0;
    struct dirent *de;
    while ((de = readdir(dp)) && n < LIB_MAX) {
        if (!is_collection(de->d_name) || strlen(de->d_name) >= sizeof list[0].file) continue;
        coll_t *c = &list[n];
        snprintf(c->file, sizeof c->file, "%s", de->d_name);
        snprintf(c->path, sizeof c->path, "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(c->path, &st) != 0) continue;
        c->size = (uint64_t)st.st_size;
        c->mtime = (int64_t)st.st_mtime;
        n++;
    }
    closedir(dp);
    // what the cache already knows
    char cpath[220];
    path_of(cpath, sizeof cpath, "/appdata/library.tsv");
    FILE *f = fopen(cpath, "r");
    char *known = (char *)calloc(n ? n : 1, 1);
    static char line[1024];
    if (f && (!fgets(line, sizeof line, f) || strcmp(line, CACHE_HEAD))) {
        fclose(f);
        f = NULL;
    }
    if (f && known) {
        while (fgets(line, sizeof line, f)) {
            char *fl[CACHE_FIELDS] = {0};
            int k = 0;
            for (char *p = line; k < CACHE_FIELDS && p; k++) {
                fl[k] = p;
                p = strchr(p, '\t');
                if (p) *p++ = 0;
            }
            if (k < CACHE_FIELDS) continue;
            fl[CACHE_FIELDS - 1][strcspn(fl[CACHE_FIELDS - 1], "\r\n")] = 0;
            for (int i = 0; i < n; i++) {
                coll_t *c = &list[i];
                if (known[i] || strcmp(c->file, fl[0]) || (uint64_t)strtoull(fl[1], NULL, 10) != c->size ||
                    (int64_t)strtoll(fl[2], NULL, 10) != c->mtime)
                    continue;
                snprintf(c->title, sizeof c->title, "%s", fl[3]);
                snprintf(c->desc, sizeof c->desc, "%s", fl[4]);
                snprintf(c->lang, sizeof c->lang, "%s", fl[5]);
                snprintf(c->date, sizeof c->date, "%s", fl[6]);
                snprintf(c->name, sizeof c->name, "%s", fl[7]);
                c->articles = strtol(fl[8], NULL, 10);
                c->bad = atoi(fl[10]);
                if (atoi(fl[9])) c->cover = cover_load(c->file);
                known[i] = 1;
            }
        }
    }
    if (f) fclose(f);
    int fresh = 0;
    for (int i = 0; i < n; i++) fresh += known ? !known[i] : 1;
    int done = 0;
    for (int i = 0; i < n; i++) {
        if (known && known[i]) continue;
        if (progress && progress(ctx, done, fresh)) break;
        read_collection(&list[i]);
        done++;
    }
    free(known);
    // the cache, rewritten
    char dpath[220];
    path_of(dpath, sizeof dpath, "/appdata");
    mkdir(dpath, 0777);
    f = fopen(cpath, "w");
    if (f) {
        fputs(CACHE_HEAD, f);
        for (int i = 0; i < n; i++) {
            const coll_t *c = &list[i];
            if (c->retry) continue;
            fprintf(f, "%s\t%llu\t%lld\t%s\t%s\t%s\t%s\t%s\t%ld\t%d\t%d\n", c->file, (unsigned long long)c->size,
                    (long long)c->mtime, c->title, c->desc, c->lang, c->date, c->name, c->articles, c->cover != NULL,
                    c->bad);
        }
        fclose(f);
    }
    qsort(list, (size_t)n, sizeof(coll_t), coll_cmp);
    g_lib = list;
    g_nlib = n;
    return n;
}

int lib_find(const char *file) {
    for (int i = 0; i < g_nlib; i++)
        if (!strcmp(g_lib[i].file, file)) return i;
    return -1;
}

// -------------------------------------------------------------------------- history, bookmarks

static const char *marks_file(int which) {
    return which == MARK_HISTORY ? "/appdata/history.tsv" : "/appdata/bookmarks.tsv";
}

#define MARKS_MAX 300

int marks_load(int which, mark_t **out) {
    *out = NULL;
    char p[220];
    path_of(p, sizeof p, marks_file(which));
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    mark_t *m = (mark_t *)calloc(MARKS_MAX, sizeof(mark_t));
    if (!m) { fclose(f); return 0; }
    int n = 0;
    static char line[1200];
    while (n < MARKS_MAX && fgets(line, sizeof line, f)) {
        char *fl[4] = {0};
        int k = 0;
        for (char *q = line; k < 4 && q; k++) {
            fl[k] = q;
            q = strchr(q, '\t');
            if (q) *q++ = 0;
        }
        if (k < 4) continue;
        fl[3][strcspn(fl[3], "\r\n")] = 0;
        m[n].when = (int64_t)strtoll(fl[0], NULL, 10);
        snprintf(m[n].file, sizeof m[n].file, "%s", fl[1]);
        snprintf(m[n].path, sizeof m[n].path, "%s", fl[2]);
        snprintf(m[n].title, sizeof m[n].title, "%s", fl[3]);
        n++;
    }
    fclose(f);
    *out = m;
    return n;
}

static int marks_save(int which, const mark_t *m, int n) {
    char p[220];
    path_of(p, sizeof p, "/appdata");
    mkdir(p, 0777);
    path_of(p, sizeof p, marks_file(which));
    FILE *f = fopen(p, "w");
    if (!f) return -1;
    for (int i = 0; i < n; i++)
        fprintf(f, "%lld\t%s\t%s\t%s\n", (long long)m[i].when, m[i].file, m[i].path, m[i].title);
    fclose(f);
    return 0;
}

int marks_add(int which, const char *file, const char *path, const char *title) {
    mark_t *m;
    int n = marks_load(which, &m);
    if (!m) {
        m = (mark_t *)calloc(MARKS_MAX, sizeof(mark_t));
        if (!m) return -1;
        n = 0;
    }
    int k = 0;   // the same page once, at the top
    for (int i = 0; i < n; i++)
        if (strcmp(m[i].file, file) || strcmp(m[i].path, path)) m[k++] = m[i];
    n = k < MARKS_MAX ? k : MARKS_MAX - 1;
    memmove(m + 1, m, sizeof(mark_t) * n);
    memset(&m[0], 0, sizeof m[0]);
    m[0].when = (int64_t)nv_time_unix();
    snprintf(m[0].file, sizeof m[0].file, "%s", file);
    snprintf(m[0].path, sizeof m[0].path, "%s", path);
    snprintf(m[0].title, sizeof m[0].title, "%s", title);
    clean(m[0].path);
    clean(m[0].title);
    const int r = marks_save(which, m, n + 1);
    free(m);
    return r;
}

int marks_remove(int which, const char *file, const char *path) {
    mark_t *m;
    const int n = marks_load(which, &m);
    if (!m) return 0;
    int k = 0;
    for (int i = 0; i < n; i++)
        if (strcmp(m[i].file, file) || strcmp(m[i].path, path)) m[k++] = m[i];
    const int r = k < n ? marks_save(which, m, k) : 0;
    free(m);
    return r;
}

int marks_has(int which, const char *file, const char *path) {
    mark_t *m;
    const int n = marks_load(which, &m);
    int has = 0;
    for (int i = 0; i < n && !has; i++) has = !strcmp(m[i].file, file) && !strcmp(m[i].path, path);
    free(m);
    return has;
}

// ----------------------------------------------------------------------------------- settings

void settings_load(void) {
    char p[220];
    path_of(p, sizeof p, "/appdata/settings.txt");
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[700];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *v = strchr(line, '=');
        if (!v) continue;
        *v++ = 0;
        if (!strcmp(line, "size")) g_set.text_size = atoi(v);
        else if (!strcmp(line, "dark")) g_set.dark = atoi(v);
        else if (!strcmp(line, "last_file")) snprintf(g_set.last_file, sizeof g_set.last_file, "%s", v);
        else if (!strcmp(line, "last_path")) snprintf(g_set.last_path, sizeof g_set.last_path, "%s", v);
        else if (!strcmp(line, "last_scroll")) g_set.last_scroll = atoi(v);
    }
    fclose(f);
    if (g_set.text_size < 0 || g_set.text_size > 4) g_set.text_size = 2;
}

void settings_save(void) {
    char p[220];
    path_of(p, sizeof p, "/appdata");
    mkdir(p, 0777);
    path_of(p, sizeof p, "/appdata/settings.txt");
    FILE *f = fopen(p, "w");
    if (!f) return;
    clean(g_set.last_path);
    fprintf(f, "size=%d\ndark=%d\nlast_file=%s\nlast_path=%s\nlast_scroll=%d\n", g_set.text_size, g_set.dark,
            g_set.last_file, g_set.last_path, (int)g_set.last_scroll);
    fclose(f);
}
