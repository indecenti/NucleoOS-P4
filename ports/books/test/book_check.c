// book_check.c — the Libri app's engine on the PC, without the screen: opens each book given
// (book.c), lays out every chapter at the app's default settings (lay.c, the real fonts), reads
// every picture's size and the cover, and prints what it found. Exit status 1 when a book fails.
// The catalog pipeline (tools/books) runs it on every EPUB before it goes into the store.
//
//   book_check [-q] FILE...      (-q: one line per book)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bk.h"

static book_t *s_book;
static int s_chap;

int lay_image_size(const char *src, int n, int *w, int *h) {
    return book_image_size(s_book, s_chap, src, n, w, h) ? -1 : 0;
}
void doc_draw_image(const doc_t *d, int idx, int x, int y, int w, int h) { (void)d; (void)idx; (void)x; (void)y; (void)w; (void)h; }

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

int main(int argc, char **argv) {
    int quiet = 0, fails = 0;
    if (font_init() < 0) { fprintf(stderr, "fonts\n"); return 2; }
    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "-q")) { quiet = 1; continue; }
        const double t0 = now_ms();
        book_t *b = NULL;
        const int r = book_open(argv[a], &b);
        if (r != BKE_OK) { printf("FAIL %s: open %d\n", argv[a], r); fails++; continue; }
        s_book = b;
        const double t1 = now_ms();
        lopt_t o;
        memset(&o, 0, sizeof o);
        o.width = 748;
        o.page_h = 512;
        o.body = 22;
        o.serif = 1;
        o.justify = o.hyphen = o.indent = o.center_heads = 1;
        o.line_h = 140;
        snprintf(o.lang, sizeof o.lang, "%s", b->lang);
        long pages = 0, imgs = 0, bad_imgs = 0, empty = 0;
        size_t text = 0;
        double worst = 0;
        int worst_ch = -1;
        for (int i = 0; i < b->nch; i++) {
            char *t = NULL;
            size_t n = 0;
            const double r0 = now_ms();
            if (book_chapter(b, i, &t, &n)) { free(t); empty++; continue; }
            const double rms = now_ms() - r0;   // reading the member and nvhtml
            text += n;
            s_chap = i;
            doc_t d;
            memset(&d, 0, sizeof d);
            const double c0 = now_ms();
            if (doc_layout(&d, t, n, &o) < 0) { printf("FAIL %s: layout of chapter %d\n", argv[a], i); fails++; }
            const double ms = now_ms() - c0 + rms;
            if (!quiet && ms > 200) printf("  (chapter %d: text %.0f ms, layout %.0f ms)\n", i, rms, ms - rms);
            if (ms > worst) { worst = ms; worst_ch = i; }
            pages += d.npages;
            for (int k = 0; k < d.nimgs; k++) {
                int w, h;
                imgs++;
                if (book_image_size(b, i, d.text + d.imgs[k].src_off, d.imgs[k].src_len, &w, &h)) bad_imgs++;
            }
            if (!quiet && (b->nch <= 60 || ms > 200 || getenv("BK_CHAPTERS")))   // long books: the slow chapters only
                printf("  chapter %3d %-48.48s %7u B  %4d pages  %3d pictures  %6.1f ms\n", i, b->ch[i].path ? b->ch[i].path : "-",
                       (unsigned)n, d.npages, d.nimgs, ms);
            doc_free(&d);
        }
        uint8_t *cv;
        size_t cn;
        int cw = 0, chh = 0;
        const int has_cover = !book_cover(b, &cv, &cn) && !img_size(cv, cn, &cw, &chh);
        if (has_cover) free(cv);
        printf("%s %s: %s | %s | %s | %s, %d chapters, %d contents, %ld pages, %ld pictures (%ld unreadable), "
               "%u KB text, cover %s %dx%d, open %.0f ms, layout worst %.0f ms (chapter %d)%s\n",
               bad_imgs || empty ? "WARN" : "OK", argv[a], b->title, b->author, b->lang, book_kind_name(b->kind), b->nch, b->ntoc,
               pages, imgs, bad_imgs, (unsigned)(text / 1024), has_cover ? "yes" : "no", cw, chh, t1 - t0, worst, worst_ch,
               b->fixed ? ", fixed layout" : "");
        if (!quiet)
            for (int i = 0; i < b->ntoc && i < 12; i++)
                printf("    toc %*s%s -> %d#%s\n", b->toc[i].level * 2, "", b->toc[i].title, b->toc[i].chap, b->toc[i].anchor);
        if (!pages) { printf("FAIL %s: no pages\n", argv[a]); fails++; }
        book_close(b);
    }
    return fails ? 1 : 0;
}
