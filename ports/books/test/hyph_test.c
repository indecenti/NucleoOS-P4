// hyph_test.c — hyphenation (hyph.c + hyphdata.c) against words whose breaks are known, in the
// languages the app has patterns for. Exit status 1 on a mismatch.
#include <stdio.h>
#include <string.h>

#include "bk.h"

static int check(const char *lang, const char *word, const char *want) {
    int pts[64];
    const int n = hyph_word(lang, word, (int)strlen(word), pts, 64);
    char got[256];
    size_t k = 0;
    int pi = 0;
    for (int i = 0; word[i] && k + 2 < sizeof got; i++) {
        if (pi < n && pts[pi] == i) { got[k++] = '-'; pi++; }
        got[k++] = word[i];
    }
    got[k] = 0;
    const int ok = !strcmp(got, want);
    printf("%s %-3s %-32s %s%s%s\n", ok ? "ok  " : "FAIL", lang, word, got, ok ? "" : "   expected ", ok ? "" : want);
    return ok;
}

int main(void) {
    int ok = 1;
    ok &= check("en", "hyphenation", "hy-phen-ation");
    ok &= check("en", "information", "in-for-ma-tion");
    ok &= check("en-GB", "computer", "com-puter");
    ok &= check("it", "precipitevolissimevolmente", "pre-ci-pi-te-vo-lis-si-me-vol-men-te");
    ok &= check("it", "letteratura", "let-te-ra-tu-ra");
    ok &= check("de", "Silbentrennung", "Sil-ben-tren-nung");
    ok &= check("fr", "constitution", "consti-tu-tion");
    ok &= check("es", "biblioteca", "bi-blio-te-ca");
    ok &= check("en", "a", "a");                        // too short: no breaks
    ok &= check("en", "\xE2\x80\x9Chyphenation,", "\xE2\x80\x9Chy-phen-ation,");   // quotes and punctuation around
    ok &= check("xx", "hyphenation", "hyphenation");     // no patterns: none
    printf("%s\n", ok ? "HYPH OK" : "HYPH FAILED");
    return ok ? 0 : 1;
}
