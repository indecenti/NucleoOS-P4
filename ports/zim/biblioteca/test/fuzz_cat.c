// fuzz_cat.c — libFuzzer target for cat.c, the Kiwix catalog parser of the download screen: the feed comes
// from the network, so any bytes must parse or be skipped, and every row it keeps must be usable (a
// https .meta4, NUL-terminated fields, a file name that stays a plain .zim name). Run by test/fuzz_cat.sh.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bib.h"

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    static cat_t rows[64];
    int total = 0;
    const int k = cat_parse((const char *)d, n, rows, 64, &total);
    if (k < 0 || k > 64) abort();
    for (int i = 0; i < k; i++) {
        const cat_t *c = &rows[i];
        if (!c->name[0] || strncmp(c->meta4, "https://", 8)) abort();
        if (strlen(c->meta4) < 7 || strcmp(c->meta4 + strlen(c->meta4) - 6, ".meta4")) abort();
        if (memchr(c->title, 0, sizeof c->title) == NULL || memchr(c->summary, 0, sizeof c->summary) == NULL) abort();
        char file[128];
        if (cat_file_name(c, file, sizeof file)) {
            if (strchr(file, '/') || strchr(file, '\\') || strncmp(file + strlen(file) - 4, ".zim", 4)) abort();
        }
        (void)cat_recommended(c->name, "it");
        (void)cat_flavour_rank(c);
    }
    return 0;
}
