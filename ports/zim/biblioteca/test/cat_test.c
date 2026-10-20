// cat_test.c — cat.c on its own, over test/opds_ita.xml (eight real entries of the Kiwix catalog, October
// 2026: three flavours of "Il meglio di Wikipedia", Wikimed, Vikidia, a TED talk). Built and run by
// test/run_native.sh with ASan and UBSan.
#include "bib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const cat_t *by(const cat_t *c, int n, const char *name, const char *flavour) {
    for (int i = 0; i < n; i++) if (!strcmp(c[i].name, name) && !strcmp(c[i].flavour, flavour)) return &c[i];
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    static char xml[64 * 1024];
    const size_t n = fread(xml, 1, sizeof xml, f);
    fclose(f);
    cat_t c[16];
    int total = 0;
    const int k = cat_parse(xml, n, c, 16, &total);
    CHECK(k == 8 && total == 382);
    const cat_t *top = by(c, k, "wikipedia_it_top", "nopic");
    CHECK(top != NULL);
    if (top) {
        CHECK(!strcmp(top->title, "Il meglio di Wikipedia"));
        CHECK(!strcmp(top->lang, "ita") && !strcmp(top->category, "wikipedia"));
        CHECK(top->size > 900000000ull && top->size < 1000000000ull);
        CHECK(top->articles > 1000 && !top->pictures && !top->videos);
        CHECK(!strncmp(top->meta4, "https://", 8) && strstr(top->meta4, ".zim.meta4"));
        CHECK(!strncmp(top->icon, "/catalog/v2/illustration/", 25));
        CHECK(strlen(top->date) == 10 && top->date[4] == '-');
        char name[128];
        CHECK(cat_file_name(top, name, sizeof name) && !strncmp(name, "wikipedia_it_top_nopic_", 23) &&
              !strcmp(name + strlen(name) - 4, ".zim"));
        CHECK(!cat_file_name(top, name, 10));
    }
    const cat_t *maxi = by(c, k, "wikipedia_it_top", "maxi"), *mini = by(c, k, "wikipedia_it_top", "mini");
    CHECK(maxi && mini && maxi->pictures && !mini->pictures);
    // a list offers the nopic flavour of "the best of": the maxi is over 1 GB, the mini only has intros
    if (maxi && mini && top) CHECK(cat_flavour_rank(top) < cat_flavour_rank(maxi) && cat_flavour_rank(top) < cat_flavour_rank(mini));
    const cat_t *med = by(c, k, "wikipedia_it_medicine", "maxi");
    CHECK(med && med->pictures && cat_flavour_rank(med) == 0);       // 398 MB with pictures
    CHECK(by(c, k, "vikidia_it_all", "nopic") != NULL);
    CHECK(cat_recommended("wikipedia_it_top", "it") == 0 && cat_recommended("wikipedia_it_medicine", "it") == 1);
    CHECK(cat_recommended("vikidia_it_all", "it") > 1 && cat_recommended("ted_mul_coronavirus", "it") < 0);
    CHECK(cat_recommended("wikipedia_en_top", "it") < 0 && cat_recommended("wikipedia_en_top", "en") == 0);
    CHECK(cat_recommended("wikipedia", "it") < 0 && cat_recommended("", "it") < 0);
    int ted = 0;
    for (int i = 0; i < k; i++) ted += !strncmp(c[i].name, "ted_", 4) && c[i].videos;
    CHECK(ted == 1);

    // every prefix of the feed: never out of bounds, never more entries than the whole
    for (size_t cut = 0; cut < n; cut += 97) CHECK(cat_parse(xml, cut, c, 16, &total) <= k);
    // entities and white space; an entry without a .meta4 is skipped
    const char *t = "<feed><totalResults>2</totalResults><entry><name>a_it_b</name><title> A  &amp;\n B &#233;&#x1F600; </title>"
                    "<link type=\"application/x-zim\" href=\"https://x/a.zim.meta4\" length=\"5\"/></entry>"
                    "<entry><name>c</name><link type=\"application/x-zim\" href=\"http://x/c.zim.meta4\"/></entry></feed>";
    CHECK(cat_parse(t, strlen(t), c, 16, &total) == 1 && total == 2 && !strcmp(c[0].title, "A & B \xc3\xa9\xf0\x9f\x98\x80") &&
          c[0].size == 5);
    CHECK(cat_parse(t, strlen(t), c, 0, &total) == 0 && cat_parse(NULL, 0, c, 16, &total) == 0);
    cat_t odd = {0};
    snprintf(odd.meta4, sizeof odd.meta4, "https://x/../evil:name.zim.meta4");
    char nm[64];
    CHECK(!cat_file_name(&odd, nm, sizeof nm));
    snprintf(odd.meta4, sizeof odd.meta4, "https://x/notzim.meta4");
    CHECK(!cat_file_name(&odd, nm, sizeof nm));

    printf("%s cat: %d checks, %d failed\n", fails ? "FAIL" : "ok", checks, fails);
    return fails ? 1 : 0;
}
