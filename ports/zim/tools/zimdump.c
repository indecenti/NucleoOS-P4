// zimdump.c — nvzim on the PC: dump entries for the differential test against python-libzim
// (diff_libzim.py), and quick checks of a ZIM file.
//
//   zimdump info FILE                    header, MIME types, metadata, main page, title count
//   zimdump entries FILE [N] [SEED]      N entries (0 = all) as TSV: index ns path title kind mime|target size md5
//   zimdump get FILE PATH                the content of an article (redirects followed) to stdout
//   zimdump suggest FILE QUERY [MAX]     title suggestions
//   zimdump titles FILE [N] [SEED]       N random positions of the title listing: pos index title
//   zimdump verify FILE                  the MD5 checksum
//   zimdump stats FILE N [SEED]          N lookups by path of random entries, with I/O statistics
#include "nvzim.h"
#include "nvzim_md5.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint32_t g_rng = 2463534242u;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void clean(char *s) {
    for (; *s; s++) if (*s == '\t' || *s == '\n' || *s == '\r') *s = ' ';
}

static int fail(const char *what, int r) {
    fprintf(stderr, "%s: %s\n", what, nvzim_strerror(r));
    return 1;
}

static int cmd_info(nvzim_t *z) {
    const nvzim_info_t *i = nvzim_get_info(z);
    printf("version %u.%u, %s namespaces, %u entries, %u clusters, %llu bytes in %u part(s), checksum %s\n",
           i->major, i->minor, i->new_scheme ? "new" : "old", i->entry_count, i->cluster_count,
           (unsigned long long)i->size, i->parts, i->has_checksum ? "yes" : "no");
    printf("mime:");
    for (uint16_t m = 0; nvzim_mime_name(z, m)[0]; m++) printf(" [%u]%s", m, nvzim_mime_name(z, m));
    printf("\n");
    static const char *const keys[] = {"Title", "Name", "Language", "Date", "Creator", "Publisher", "Flavour", "Counter"};
    char buf[256];
    for (size_t k = 0; k < sizeof keys / sizeof keys[0]; k++)
        if (nvzim_metadata(z, keys[k], buf, sizeof buf) >= 0) printf("%s: %s\n", keys[k], buf);
    nvzim_entry_t e;
    int r = nvzim_main_page(z, &e);
    printf("main page: %s\n", r ? nvzim_strerror(r) : e.path);
    r = nvzim_title_count(z);
    printf("titles: %d\n", r);
    return 0;
}

static int cmd_entries(nvzim_t *z, int n, uint32_t seed) {
    const uint32_t count = nvzim_get_info(z)->entry_count;
    if (seed) g_rng = seed;
    const uint32_t total = n > 0 ? (uint32_t)n : count;
    for (uint32_t k = 0; k < total && count; k++) {
        const uint32_t idx = n > 0 ? rnd() % count : k;
        nvzim_entry_t e;
        int r = nvzim_entry_at(z, idx, &e);
        if (r) { printf("%u\tERR\t%s\n", idx, nvzim_strerror(r)); continue; }
        clean(e.path);
        clean(e.title);
        if (e.mime == NVZIM_MIME_REDIRECT) {
            nvzim_entry_t t;
            r = nvzim_entry_at(z, e.target, &t);
            clean(t.path);
            printf("%u\t%c\t%s\t%s\tredirect\t%c/%s\t0\t-\n", idx, e.ns, e.path, e.title, r ? '?' : t.ns, r ? "?" : t.path);
            continue;
        }
        if (e.mime >= NVZIM_MIME_DELETED) {
            printf("%u\t%c\t%s\t%s\tother\t-\t0\t-\n", idx, e.ns, e.path, e.title);
            continue;
        }
        nvzim_blob_t b;
        r = nvzim_read(z, &e, &b);
        if (r) { printf("%u\t%c\t%s\t%s\tcontent\t%s\tERR\t%s\n", idx, e.ns, e.path, e.title, nvzim_mime_name(z, e.mime), nvzim_strerror(r)); continue; }
        nvzim_md5_t m;
        uint8_t d[16];
        nvzim_md5_init(&m);
        nvzim_md5_update(&m, b.data, (size_t)b.size);
        nvzim_md5_final(&m, d);
        printf("%u\t%c\t%s\t%s\tcontent\t%s\t%llu\t", idx, e.ns, e.path, e.title, nvzim_mime_name(z, e.mime),
               (unsigned long long)b.size);
        for (int i = 0; i < 16; i++) printf("%02x", d[i]);
        printf("\n");
        nvzim_blob_release(z, &b);
    }
    return 0;
}

static int cmd_get(nvzim_t *z, const char *path) {
    nvzim_entry_t e;
    int r = nvzim_find_article(z, path, &e);
    if (!r) r = nvzim_resolve(z, &e);
    nvzim_blob_t b;
    if (!r) r = nvzim_read(z, &e, &b);
    if (r) return fail(path, r);
    fwrite(b.data, 1, (size_t)b.size, stdout);
    nvzim_blob_release(z, &b);
    return 0;
}

static int cmd_suggest(nvzim_t *z, const char *q, int max) {
    nvzim_entry_t *out = (nvzim_entry_t *)malloc(sizeof(nvzim_entry_t) * (size_t)max);
    const double t0 = now_ms();
    const int n = nvzim_suggest(z, q, out, max);
    const double t1 = now_ms();
    if (n < 0) return fail("suggest", n);
    for (int i = 0; i < n; i++) printf("%s%s\n", out[i].title, out[i].mime == NVZIM_MIME_REDIRECT ? "  (redirect)" : "");
    fprintf(stderr, "%d suggestions in %.1f ms, %llu reads\n", n, t1 - t0, (unsigned long long)nvzim_get_stats(z)->reads);
    free(out);
    return 0;
}

static int cmd_titles(nvzim_t *z, int n, uint32_t seed) {
    const int count = nvzim_title_count(z);
    if (count <= 0) return fail("titles", count ? count : NVZIM_ENOTFOUND);
    if (seed) g_rng = seed;
    for (int k = 0; k < n; k++) {
        const uint32_t pos = rnd() % (uint32_t)count;
        nvzim_entry_t e;
        const int r = nvzim_title_at(z, pos, &e);
        if (r) { printf("%u\tERR\t%s\n", pos, nvzim_strerror(r)); continue; }
        clean(e.title);
        printf("%u\t%u\t%s\n", pos, e.index, e.title);
    }
    return 0;
}

static int progress(void *ctx, uint64_t done, uint64_t total) {
    (void)ctx;
    if (done == total || (done & ((64u << 20) - 1)) == 0) fprintf(stderr, "\r%llu / %llu MB", (unsigned long long)(done >> 20), (unsigned long long)(total >> 20));
    return 0;
}

static int cmd_stats(nvzim_t *z, int n, uint32_t seed) {
    const uint32_t count = nvzim_get_info(z)->entry_count;
    if (seed) g_rng = seed;
    double worst = 0, sum = 0;
    for (int k = 0; k < n; k++) {
        nvzim_entry_t e, f;
        if (nvzim_entry_at(z, rnd() % count, &e)) continue;
        const nvzim_stats_t before = *nvzim_get_stats(z);
        const double t0 = now_ms();
        const int r = nvzim_find(z, e.ns, e.path, &f);
        const double t = now_ms() - t0;
        if (r || f.index != e.index) printf("lookup mismatch for %c/%s: %s\n", e.ns, e.path, nvzim_strerror(r));
        sum += t;
        if (t > worst) worst = t;
        (void)before;
    }
    const nvzim_stats_t *s = nvzim_get_stats(z);
    printf("%d lookups: mean %.3f ms, worst %.3f ms; reads %llu, page hits %llu misses %llu, tree hits %llu\n", n,
           sum / n, worst, (unsigned long long)s->reads, (unsigned long long)s->page_hits,
           (unsigned long long)s->page_misses, (unsigned long long)s->tree_hits);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: zimdump info|entries|get|suggest|titles|verify|stats FILE [ARGS]\n");
        return 2;
    }
    nvzim_t *z;
    int r = nvzim_open(&z, argv[2], NULL);
    if (r) return fail(argv[2], r);
    const char *c = argv[1];
    int rc = 2;
    if (!strcmp(c, "info")) rc = cmd_info(z);
    else if (!strcmp(c, "entries")) rc = cmd_entries(z, argc > 3 ? atoi(argv[3]) : 0, argc > 4 ? (uint32_t)strtoul(argv[4], 0, 10) : 0);
    else if (!strcmp(c, "get") && argc > 3) rc = cmd_get(z, argv[3]);
    else if (!strcmp(c, "suggest") && argc > 3) rc = cmd_suggest(z, argv[3], argc > 4 ? atoi(argv[4]) : 10);
    else if (!strcmp(c, "titles")) rc = cmd_titles(z, argc > 3 ? atoi(argv[3]) : 20, argc > 4 ? (uint32_t)strtoul(argv[4], 0, 10) : 0);
    else if (!strcmp(c, "verify")) {
        r = nvzim_verify(z, progress, NULL);
        fprintf(stderr, "\n");
        printf("verify: %s\n", nvzim_strerror(r));
        rc = r ? 1 : 0;
    } else if (!strcmp(c, "stats")) rc = cmd_stats(z, argc > 3 ? atoi(argv[3]) : 100, argc > 4 ? (uint32_t)strtoul(argv[4], 0, 10) : 0);
    nvzim_close(z);
    return rc;
}
