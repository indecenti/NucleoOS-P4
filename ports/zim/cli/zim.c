// zim.c — the `zim` program of the NucleoOS Terminal, and ANIMA's tool for the offline library:
// encyclopedias, dictionaries and manuals in Kiwix's ZIM files, in ~/zim on the SD card. Articles
// come out as plain text (nvhtml). See docs/ZIM_READER_PLAN.md.
//
//   zim ls                                   the collections in ~/zim
//   zim info NAME                            title, language, date, articles, licence of one
//   zim search [-z NAME] [-n N] TEXT         titles starting with TEXT (any case), everywhere or in NAME
//   zim get [-z NAME] TITLE [--intro | --section S | --sections | --html | --out FILE] [--max N] [--all]
//   zim random [-z NAME]                     the beginning of a random article
//   zim check NAME                           the file's MD5 checksum
//
// NAME is a collection's name (wikipedia_it_medicine), a unique part of it (medicine) or a file.
// Exit status: 0 done, 1 not found, 2 usage, 3 error.
#include "nvhtml.h"
#include "nvzim.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __wasm__
__attribute__((import_module("nv"), import_name("lang"))) int32_t nv_lang(char *buf, uint32_t len);
#endif

#define MAX_COLL 64

typedef struct {
    char file[512];         // path as the program sees it
    char name[96];          // metadata Name, or the file name without date and extension
    char title[128], lang[16], date[16];
    nvzim_t *z;
} coll_t;

static coll_t g_coll[MAX_COLL];
static int g_ncoll;
static const char *g_dir = "/zim";

static int ends_with(const char *s, const char *suf) {
    const size_t a = strlen(s), b = strlen(suf);
    return a >= b && !strcmp(s + a - b, suf);
}

static void lowercase(char *s) {
    for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s += 32;
}

static int open_coll(coll_t *c) {
    if (c->z) return 0;
    nvzim_config_t cfg = {0};
    cfg.clusters = 1;   // one article at a time
    const int r = nvzim_open(&c->z, c->file, &cfg);
    if (r) fprintf(stderr, "zim: %s: %s\n", c->file, nvzim_strerror(r));
    return r;
}

static void close_coll(coll_t *c) {
    nvzim_close(c->z);
    c->z = NULL;
}

static void describe(coll_t *c) {
    if (open_coll(c)) return;
    char n[96];
    if (nvzim_metadata(c->z, "Name", n, sizeof n) > 0) snprintf(c->name, sizeof c->name, "%s", n);
    nvzim_metadata(c->z, "Title", c->title, sizeof c->title);
    nvzim_metadata(c->z, "Language", c->lang, sizeof c->lang);
    nvzim_metadata(c->z, "Date", c->date, sizeof c->date);
}

// The collections in g_dir: *.zim and the first part of split files (*.zimaa). For two files of
// the same collection (an update next to the old one) the newer date wins.
static void scan(void) {
    const char *env = getenv("ZIM_DIR");
    if (env && *env) g_dir = env;
    DIR *d = opendir(g_dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) && g_ncoll < MAX_COLL) {
        const char *f = de->d_name;
        if (f[0] == '.' || !(ends_with(f, ".zim") || ends_with(f, ".zimaa"))) continue;
        coll_t *c = &g_coll[g_ncoll];
        memset(c, 0, sizeof *c);
        snprintf(c->file, sizeof c->file, "%s/%s", g_dir, f);
        // default name: the file name without extension and "_YYYY-MM"
        snprintf(c->name, sizeof c->name, "%s", f);
        char *dot = strstr(c->name, ".zim");
        if (dot) *dot = 0;
        const size_t l = strlen(c->name);
        if (l > 8 && c->name[l - 8] == '_' && c->name[l - 3] == '-') c->name[l - 8] = 0;
        describe(c);
        if (!c->z) continue;
        close_coll(c);   // open again when needed: one collection in memory at a time
        int dup = -1;
        for (int i = 0; i < g_ncoll; i++) if (!strcmp(g_coll[i].name, c->name)) dup = i;
        if (dup >= 0) {
            if (strcmp(c->date, g_coll[dup].date) > 0) g_coll[dup] = *c;
            continue;
        }
        g_ncoll++;
    }
    closedir(d);
}

// Collections in the language of the device first (ita, eng... in ZIM metadata).
static void order_by_language(void) {
    char ui[8] = "";
    const char *env = getenv("ZIM_LANG");
    if (env && *env) snprintf(ui, sizeof ui, "%s", env);
#ifdef __wasm__
    else if (getenv("NUCLEO_APP")) nv_lang(ui, sizeof ui);   // only the device has this import
#endif
    static const char *const kMap[][2] = {{"it", "ita"}, {"en", "eng"}, {"es", "spa"}, {"fr", "fra"}, {"de", "deu"}};
    const char *want = ui;
    for (size_t i = 0; i < sizeof kMap / sizeof kMap[0]; i++) if (!strcmp(ui, kMap[i][0])) want = kMap[i][1];
    if (!want[0]) return;
    int k = 0;
    for (int i = 0; i < g_ncoll; i++)
        if (strstr(g_coll[i].lang, want)) {
            const coll_t t = g_coll[i];
            memmove(&g_coll[k + 1], &g_coll[k], sizeof(coll_t) * (size_t)(i - k));
            g_coll[k++] = t;
        }
}

// NAME: a collection's name, a unique part of it, or a file path.
static coll_t *pick(const char *name) {
    static coll_t direct;
    if (strchr(name, '/') || ends_with(name, ".zim") || ends_with(name, ".zimaa")) {
        memset(&direct, 0, sizeof direct);
        const char *pwd = getenv("PWD");
        if (name[0] != '/' && pwd && strcmp(pwd, "/")) snprintf(direct.file, sizeof direct.file, "%s/%s", pwd, name);
        else snprintf(direct.file, sizeof direct.file, "%s", name);
        snprintf(direct.name, sizeof direct.name, "%s", name);
        describe(&direct);
        return direct.z ? &direct : NULL;
    }
    for (int i = 0; i < g_ncoll; i++)
        if (!strcmp(g_coll[i].name, name)) return open_coll(&g_coll[i]) ? NULL : &g_coll[i];
    char want[96], have[96];
    snprintf(want, sizeof want, "%s", name);
    lowercase(want);
    coll_t *found = NULL;
    int n = 0;
    for (int i = 0; i < g_ncoll; i++) {
        snprintf(have, sizeof have, "%s", g_coll[i].name);
        lowercase(have);
        if (strstr(have, want)) { found = &g_coll[i]; n++; }
    }
    if (n == 1) return open_coll(found) ? NULL : found;
    if (n > 1) {
        fprintf(stderr, "zim: '%s' matches more than one collection:", name);
        for (int i = 0; i < g_ncoll; i++) {
            snprintf(have, sizeof have, "%s", g_coll[i].name);
            lowercase(have);
            if (strstr(have, want)) fprintf(stderr, " %s", g_coll[i].name);
        }
        fprintf(stderr, "\n");
    } else {
        fprintf(stderr, "zim: no collection '%s' in ~%s (zim ls lists them)\n", name, g_dir);
    }
    return NULL;
}

static int no_collections(void) {
    fprintf(stderr, "zim: no ZIM files in ~%s. Copy them there (Kiwix library: library.kiwix.org)\n", g_dir);
    return 1;
}

// ------------------------------------------------------------------------------------ commands

static long html_count(nvzim_t *z) {   // M/Counter: "text/html=72544;image/webp=98;..."
    char buf[1024];
    if (nvzim_metadata(z, "Counter", buf, sizeof buf) <= 0) return -1;
    const char *p = strstr(buf, "text/html=");
    return p ? atol(p + 10) : -1;
}

static int cmd_ls(void) {
    if (!g_ncoll) return no_collections();
    for (int i = 0; i < g_ncoll; i++) {
        coll_t *c = &g_coll[i];
        if (open_coll(c)) continue;
        const nvzim_info_t *in = nvzim_get_info(c->z);
        long n = html_count(c->z);
        if (n < 0) n = nvzim_title_count(c->z);
        printf("%-32s %s (%s, %s) %ld articles, %.0f MB%s\n", c->name, c->title, c->lang, c->date, n,
               in->size / 1048576.0, in->parts > 1 ? ", split" : "");
        close_coll(c);
    }
    return 0;
}

static int cmd_info(const char *name) {
    coll_t *c = pick(name);
    if (!c) return 1;
    static const char *const keys[] = {"Title", "Description", "LongDescription", "Language", "Creator", "Publisher",
                                       "Date", "Name", "Flavour", "Source", "License", "Tags"};
    char buf[1024];
    for (size_t k = 0; k < sizeof keys / sizeof keys[0]; k++)
        if (nvzim_metadata(c->z, keys[k], buf, sizeof buf) > 0) printf("%s: %s\n", keys[k], buf);
    const nvzim_info_t *in = nvzim_get_info(c->z);
    nvzim_entry_t e;
    printf("Articles: %ld\nTitles: %d\nFile: %s (%.1f MB%s, ZIM %u.%u)\n", html_count(c->z), nvzim_title_count(c->z),
           c->file, in->size / 1048576.0, in->parts > 1 ? ", split" : "", in->major, in->minor);
    if (!nvzim_main_page(c->z, &e)) printf("Main page: %s\n", e.title);
    return 0;
}

static int cmd_search(const char *name, int n, const char *q) {
    if (!g_ncoll && !name) return no_collections();
    nvzim_entry_t *out = (nvzim_entry_t *)malloc(sizeof(nvzim_entry_t) * (size_t)n);
    if (!out) return 3;
    int total = 0;
    coll_t *only = name ? pick(name) : NULL;
    if (name && !only) { free(out); return 1; }
    for (int i = 0; i < (only ? 1 : g_ncoll); i++) {
        coll_t *c = only ? only : &g_coll[i];
        if (open_coll(c)) continue;
        const int k = nvzim_suggest(c->z, q, out, n);
        for (int j = 0; j < k; j++) {
            if (only) printf("%s\n", out[j].title);
            else printf("%s: %s\n", c->name, out[j].title);
        }
        total += k > 0 ? k : 0;
        if (!only) close_coll(c);
    }
    free(out);
    if (!total) fprintf(stderr, "zim: no title starts with '%s'\n", q);
    return total ? 0 : 1;
}

// An article of c by path or by title: an exact title (any case) wins, else the nearest title.
static int find_title(coll_t *c, const char *q, nvzim_entry_t *e, int exact_only) {
    if (!nvzim_find_article(c->z, q, e)) return 0;
    char path[NVZIM_PATH_MAX];
    snprintf(path, sizeof path, "%s", q);
    for (char *p = path; *p; p++) if (*p == ' ') *p = '_';
    if (!nvzim_find_article(c->z, path, e)) return 0;
    nvzim_entry_t s[8];
    const int k = nvzim_suggest(c->z, q, s, 8);
    if (k <= 0) return 1;
    // nvzim_suggest puts an exact match (ignoring case) first
    char a[NVZIM_TITLE_MAX], b[NVZIM_TITLE_MAX];
    snprintf(a, sizeof a, "%s", s[0].title);
    snprintf(b, sizeof b, "%s", q);
    lowercase(a);
    lowercase(b);
    if (exact_only && strcmp(a, b)) return 1;
    *e = s[0];
    return 0;
}

static int print_article(coll_t *c, nvzim_entry_t *e, const nvhtml_opts_t *o, int mode, const char *outfile) {
    int r = nvzim_resolve(c->z, e);
    nvzim_blob_t b;
    if (!r) r = nvzim_read(c->z, e, &b);
    if (r) { fprintf(stderr, "zim: %s: %s\n", e->path, nvzim_strerror(r)); return 3; }
    const char *mime = nvzim_mime_name(c->z, e->mime);
    if (outfile) {
        const uint64_t size = b.size;
        FILE *f = fopen(outfile, "wb");
        const int ok = f && fwrite(b.data, 1, (size_t)size, f) == size;
        if (f && fclose(f)) { fprintf(stderr, "zim: cannot write %s\n", outfile); nvzim_blob_release(c->z, &b); return 3; }
        nvzim_blob_release(c->z, &b);
        if (!ok) { fprintf(stderr, "zim: cannot write %s\n", outfile); return 3; }
        printf("%s: %llu bytes (%s)\n", outfile, (unsigned long long)size, mime);
        return 0;
    }
    if (strncmp(mime, "text/", 5)) {
        fprintf(stderr, "zim: %s is %s (%llu bytes): save it with --out FILE\n", e->path, mime, (unsigned long long)b.size);
        nvzim_blob_release(c->z, &b);
        return 3;
    }
    char *conv = NULL;
    const char *html = (const char *)b.data;
    size_t hn = (size_t)b.size;
    if (strstr(mime, "iso-8859-1")) {
        conv = nvhtml_latin1_to_utf8(html, hn, &hn);
        html = conv;
    }
    if (!html) { nvzim_blob_release(c->z, &b); return 3; }
    if (mode == 'h' || strncmp(mime, "text/html", 9)) {   // --html, or plain text as it is
        fwrite(html, 1, hn, stdout);
        if (hn && html[hn - 1] != '\n') putchar('\n');
    } else {
        size_t len;
        char *t = mode == 's' ? nvhtml_sections(html, hn, &len) : nvhtml_text(html, hn, o, &len);
        if (!t) { free(conv); nvzim_blob_release(c->z, &b); return 3; }
        if (!len && o->section) {
            fprintf(stderr, "zim: no section '%s' in %s; its sections:\n", o->section, e->title);
            char *secs = nvhtml_sections(html, hn, NULL);
            if (secs) fputs(secs, stderr);
            free(secs);
            free(t);
            free(conv);
            nvzim_blob_release(c->z, &b);
            return 1;
        }
        fwrite(t, 1, len, stdout);
        if (len && t[len - 1] != '\n') putchar('\n');
        free(t);
        if (mode != 's') printf("\n-- %s (%s, %s), offline\n", c->title[0] ? c->title : c->name, c->name, c->date);
    }
    free(conv);
    nvzim_blob_release(c->z, &b);
    return 0;
}

static int cmd_get(const char *name, const char *q, const nvhtml_opts_t *o, int mode, const char *outfile) {
    nvzim_entry_t e;
    if (name) {
        coll_t *c = pick(name);
        if (!c) return 1;
        if (find_title(c, q, &e, 0)) { fprintf(stderr, "zim: nothing called '%s' in %s\n", q, c->name); return 1; }
        return print_article(c, &e, o, mode, outfile);
    }
    if (!g_ncoll) return no_collections();
    for (int pass = 0; pass < 2; pass++)   // an exact title anywhere first, then the nearest one
        for (int i = 0; i < g_ncoll; i++) {
            if (open_coll(&g_coll[i])) continue;
            if (!find_title(&g_coll[i], q, &e, pass == 0)) return print_article(&g_coll[i], &e, o, mode, outfile);
            close_coll(&g_coll[i]);
        }
    fprintf(stderr, "zim: nothing called '%s' (zim search finds titles)\n", q);
    return 1;
}

static int cmd_random(const char *name) {
    coll_t *c = name ? pick(name) : (g_ncoll ? &g_coll[(unsigned)time(NULL) % (unsigned)g_ncoll] : NULL);
    if (!c) return name ? 1 : no_collections();
    if (open_coll(c)) return 3;
    nvzim_entry_t e;
    const int r = nvzim_random(c->z, (uint32_t)time(NULL) ^ (uint32_t)clock() * 2654435761u, &e);
    if (r) { fprintf(stderr, "zim: %s: %s\n", c->name, nvzim_strerror(r)); return 1; }
    nvhtml_opts_t o = {0};
    o.intro = 1;
    o.max = 1500;
    return print_article(c, &e, &o, 0, NULL);
}

static int progress(void *ctx, uint64_t done, uint64_t total) {
    (void)ctx;
    static int last = -1;
    const int pct = total ? (int)(done * 100 / total) : 100;
    if (pct / 10 != last) { fprintf(stderr, "%d%% ", pct); last = pct / 10; }
    return 0;
}

static int cmd_check(const char *name) {
    coll_t *c = pick(name);
    if (!c) return 1;
    const int r = nvzim_verify(c->z, progress, NULL);
    fprintf(stderr, "\n");
    printf("%s: %s\n", c->name, r == NVZIM_OK ? "ok" : nvzim_strerror(r));
    return r == NVZIM_OK ? 0 : 1;
}

static int usage(void) {
    fprintf(stderr,
            "usage: zim ls | info NAME | search [-z NAME] [-n N] TEXT |\n"
            "       get [-z NAME] TITLE [--intro | --section S | --sections | --html | --out FILE] [--max N] [--all] |\n"
            "       random [-z NAME] | check NAME\n"
            "Collections are the ZIM files in ~/zim (Wikipedia, Wiktionary... from library.kiwix.org).\n");
    return 2;
}

int main(int argc, char **argv) {
    if (argc < 2) return usage();
    const char *cmd = argv[1], *name = NULL, *section = NULL, *outfile = NULL;
    int n = 0, mode = 0;
    nvhtml_opts_t o = {0};
    char text[1024] = "";
    for (int i = 2; i < argc; i++) {   // options anywhere; the other words make the TEXT/TITLE
        const char *a = argv[i];
        if (!strcmp(a, "-z") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(a, "-n") && i + 1 < argc) n = atoi(argv[++i]);
        else if (!strcmp(a, "--max") && i + 1 < argc) o.max = (size_t)atol(argv[++i]);
        else if (!strcmp(a, "--section") && i + 1 < argc) section = argv[++i];
        else if (!strcmp(a, "--out") && i + 1 < argc) outfile = argv[++i];
        else if (!strcmp(a, "--intro")) o.intro = 1;
        else if (!strcmp(a, "--sections")) mode = 's';
        else if (!strcmp(a, "--html")) mode = 'h';
        else if (!strcmp(a, "--all")) o.back_matter = 1;
        else if (a[0] == '-' && a[1] == '-') { fprintf(stderr, "zim: unknown option %s\n", a); return usage(); }
        else {
            if (text[0]) strncat(text, " ", sizeof text - strlen(text) - 1);
            strncat(text, a, sizeof text - strlen(text) - 1);
        }
    }
    o.section = section;
    scan();
    order_by_language();
    int r;
    if (!strcmp(cmd, "ls")) r = cmd_ls();
    else if (!strcmp(cmd, "info") && (text[0] || name)) r = cmd_info(text[0] ? text : name);
    else if (!strcmp(cmd, "search") && text[0]) r = cmd_search(name, n > 0 ? (n > 50 ? 50 : n) : (name ? 10 : 5), text);
    else if (!strcmp(cmd, "get") && text[0]) r = cmd_get(name, text, &o, mode, outfile);
    else if (!strcmp(cmd, "random")) r = cmd_random(name ? name : (text[0] ? text : NULL));
    else if (!strcmp(cmd, "check") && (text[0] || name)) r = cmd_check(text[0] ? text : name);
    else r = usage();
    for (int i = 0; i < g_ncoll; i++) close_coll(&g_coll[i]);
    return r;
}
