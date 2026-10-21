// reader.c — reading a book: chapters laid out on demand (the current one and its neighbour kept),
// spreads of one or two pages drawn into surfaces, the next one drawn ahead while the reader reads,
// turned with an animation that follows the finger (slide or cover; none, or the refresh flash of an
// e-ink screen on the paper themes). Fixed-layout books and comics are one document of pictures,
// one per page, two side by side in landscape. Around the page: the controls (tap the middle), the
// contents and bookmarks, search, the appearance settings, notes in a popup, a picture viewer with
// zoom, the book's information.
#include "app.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "nucleo_sdk.h"
#include "nvhtml.h"

#define TURN_MS 230
#define SAVE_AFTER_MS 1500

static int s_draw_chap;           // the chapter of the doc being drawn (pictures are keyed by it)
static int s_missing;             // a picture was not decoded yet while drawing
static int s_cur_missing, s_nxt_missing;
static int s_save_due;            // the position changed: saved once the page is still
static int s_last_dir = 1;
static int32_t *s_fx_off;         // fixed-layout books: where each chapter (page) starts in the one document
static uint64_t *s_before;        // bytes of the chapters before each one (progress)
static int s_slider_pm = -1;      // dragging the progress slider: where it is (permille)

// ---------------------------------------------------------------------------- pictures' sizes

typedef struct { uint32_t h; int w, hh; } isz_t;
static isz_t s_isz[256];

static uint32_t hkey(int chap, const char *s, int n) {
    uint32_t h = 2166136261u ^ (uint32_t)chap;
    for (int i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h ? h : 1;
}

int lay_image_size(const char *src, int n, int *w, int *h) {
    if (!A.book) return -1;
    const uint32_t k = hkey(A.lay_chap, src, n);
    isz_t *e = &s_isz[k & 255];
    if (e->h == k) { *w = e->w; *h = e->hh; return e->w > 0 ? 0 : -1; }
    const int r = book_image_size(A.book, A.lay_chap, src, n, w, h);
    e->h = k;
    e->w = r ? 0 : *w;
    e->hh = r ? 0 : *h;
    return r ? -1 : 0;
}

void doc_draw_image(const doc_t *d, int idx, int x, int y, int w, int h) {
    const dimg_t *im = &d->imgs[idx];
    char key[340];
    snprintf(key, sizeof key, "%d|%.*s", s_draw_chap, (int)im->src_len, d->text + im->src_off);
    const int st = img_lookup(key, w, h, im->mask);
    if (st == 1 && img_draw(key, w, h, x, y, g_col[C_TEXT]) == 0) return;
    if (st == 0) s_missing = 1;
    if (!im->mask) {   // a placeholder: the frame of the picture
        gfx_rrect(x, y, w, h, 6, g_col[C_CARD]);
        if (w > 60 && h > 60) icon_draw(IC_BOOK, x + (w - 48) / 2, y + (h - 48) / 2, g_col[C_RULE]);
    }
}

static int img_read(void *ctx, const char *key, uint8_t **data, size_t *n) {
    (void)ctx;
    const char *bar = strchr(key, '|');
    if (!bar || !A.book) return -1;
    return book_resource(A.book, atoi(key), bar + 1, (int)strlen(bar + 1), data, n) ? -1 : 0;
}

// ---------------------------------------------------------------------------------- geometry

static int is_fixed(void) { return A.book && A.book->fixed; }

static void geometry(void) {
    static const int kMargin[4] = {28, 52, 80, 116};
    static const int kLine[4] = {125, 140, 155, 175};
    memset(&A.lo, 0, sizeof A.lo);
    if (is_fixed()) {
        A.cols = g_set.cols == 1 || g_w <= g_h ? 1 : 2;
        A.text_y = 0;
        A.text_h = g_h;
        A.lo.width = g_w / A.cols;
        A.col_x[0] = 0;
        A.col_x[1] = g_w / 2;
    } else {
        const int m = kMargin[g_set.margin];
        A.text_y = g_set.status ? 46 : 26;
        A.text_h = g_h - A.text_y - (g_set.status ? 42 : 26);
        const int avail = g_w - 2 * m, gutter = 64;
        const int two_w = (avail - gutter) / 2;
        A.cols = g_set.cols == 2 ? 2 : g_set.cols == 1 ? 1 : two_w * 10 >= g_set.size * 195 ? 2 : 1;
        if (A.cols == 2) {
            A.lo.width = two_w;
            A.col_x[0] = m;
            A.col_x[1] = m + two_w + gutter;
        } else {
            int w = avail, cap = g_set.size * 34;   // about 65 characters a line
            if (w > cap) w = cap;
            A.lo.width = w;
            A.col_x[0] = A.col_x[1] = (g_w - w) / 2;
        }
    }
    A.lo.page_h = A.text_h;
    A.lo.body = g_set.size;
    A.lo.serif = g_set.serif;
    A.lo.justify = g_set.justify;
    A.lo.hyphen = g_set.hyphen;
    A.lo.indent = g_set.indent;
    A.lo.center_heads = 1;
    A.lo.line_h = kLine[g_set.spacing];
    snprintf(A.lo.lang, sizeof A.lo.lang, "%s", A.book && A.book->lang[0] ? A.book->lang : ui_lang());
    for (char *p = A.lo.lang; *p; p++) *p = (char)tolower((unsigned char)*p);
}

// --------------------------------------------------------------------------------- chapters

// The rich text of a fixed-layout book: every page's line, one after the other (s_fx_off[i] = where
// page i starts), so spreads of two pictures are two pages of one document.
static char *fixed_text(size_t *out) {
    size_t cap = 4096, n = 0;
    char *t = (char *)malloc(cap);
    free(s_fx_off);
    s_fx_off = (int32_t *)malloc(sizeof(int32_t) * (size_t)(A.book->nch + 1));
    if (!t || !s_fx_off) { free(t); return NULL; }
    for (int i = 0; i < A.book->nch; i++) {
        char *c = NULL;
        size_t cn = 0;
        s_fx_off[i] = (int32_t)n;
        if (book_chapter(A.book, i, &c, &cn) || !cn) { free(c); c = strdup("\xC2\xA0"); cn = c ? 2 : 0; }   // an empty page
        if (n + cn + 3 > cap) {
            while (n + cn + 3 > cap) cap *= 2;
            char *nt = (char *)realloc(t, cap);
            if (!nt) { free(c); free(t); return NULL; }
            t = nt;
        }
        memcpy(t + n, c, cn);
        n += cn;
        t[n++] = '\n';
        t[n++] = '\n';
        free(c);
        if (!(i & 15)) pump();
    }
    s_fx_off[A.book->nch] = (int32_t)n;
    t[n] = 0;
    *out = n;
    return t;
}

static void draw_loading(void) {
    const char *s = tr(S_LOADING);
    const int w = text_width(F_SANS, 18, s, (int)strlen(s)) + 48;
    gfx_rrect((g_w - w) / 2, g_h / 2 - 26, w, 52, 26, g_col[C_CARD]);
    gfx_ring((g_w - w) / 2, g_h / 2 - 26, w, 52, 26, 1, g_col[C_RULE]);
    text_draw(F_SANS, 18, (g_w - w) / 2 + 24, g_h / 2 + 6, s, (int)strlen(s), g_col[C_TEXT]);
    pump();
}

static int layout_into(doc_t *d, int chap) {
    char *t = NULL;
    size_t n = 0;
    const int t0 = nv_millis();
    if (is_fixed()) t = fixed_text(&n);
    else if (book_chapter(A.book, chap, &t, &n)) { free(t); t = NULL; }
    if (!t) { t = strdup(" "); n = 1; }
    if (!t) return -1;
    const int t1 = nv_millis();
    A.lay_chap = chap;
    const int r = doc_layout(d, t, n, &A.lo);
    perf("chapter %d: %u KB text in %d ms, layout %d ms, %d pages, %d ops", chap, (unsigned)(n / 1024), t1 - t0,
         nv_millis() - t1, d->npages, d->nops);
    return r;
}

// The doc of chapter c: the current one, the neighbour, or laid out now into the neighbour's place.
static doc_t *doc_of(int c) {
    if (c == A.chap) return &A.doc;
    if (c == A.side_chap) return &A.side;
    doc_free(&A.side);
    A.side_chap = -1;
    if (A.cur_ok) draw_loading();
    if (layout_into(&A.side, c) < 0) return NULL;
    A.side_chap = c;
    return &A.side;
}

static void make_current(int c) {   // chapter c (laid out in A.side) becomes the current one
    if (c == A.chap) return;
    doc_t t = A.doc;
    A.doc = A.side;
    A.side = t;
    const int oc = A.chap;
    A.chap = c;
    A.side_chap = oc;
}

static int spread_of(int page) { return A.cols == 2 ? page - (page & 1) : page; }
static int last_spread(const doc_t *d) { return spread_of(d->npages - 1 < 0 ? 0 : d->npages - 1); }

static int nchaps(void) { return is_fixed() ? 1 : A.book->nch; }

// ------------------------------------------------------------------------------ progress

static void before_init(void) {
    free(s_before);
    s_before = (uint64_t *)malloc(sizeof(uint64_t) * (size_t)(A.book->nch + 1));
    if (!s_before) return;
    uint64_t acc = 0;
    for (int i = 0; i < A.book->nch; i++) { s_before[i] = acc; acc += A.book->ch[i].size; }
    s_before[A.book->nch] = acc;
}

static int permille_at(int chap, const doc_t *d, int page) {
    if (!d || !d->npages) return 0;
    if (is_fixed()) return d->npages > 1 ? (int)((long)page * 1000 / (d->npages - 1)) : 1000;
    if (!s_before || !A.book->total) return 0;
    const int lp = page + A.cols - 1 >= d->npages ? d->npages - 1 : page + A.cols - 1;
    const double in = d->n ? (double)(lp + 1 >= d->npages ? d->n : (size_t)d->page_off[lp + 1]) / (double)d->n : 1.0;
    const double v = ((double)s_before[chap] + in * A.book->ch[chap].size) / (double)A.book->total;
    int pm = (int)(v * 1000 + 0.5);
    if (chap == A.book->nch - 1 && lp >= d->npages - 1) pm = 1000;
    return pm < 0 ? 0 : pm > 1000 ? 1000 : pm;
}

static const char *chapter_title(int chap) {
    if (!A.book) return "";
    const tocent_t *best = NULL;
    for (int i = 0; i < A.book->ntoc; i++) {
        const tocent_t *t = &A.book->toc[i];
        if (t->chap == chap) return t->title;
        if (t->chap < chap && (!best || t->chap >= best->chap)) best = t;
    }
    return best ? best->title : "";
}

// --------------------------------------------------------------------------------- drawing

static void ribbon(int x, int y) {   // the bookmark on a page
    for (int j = 0; j < 34; j++) {
        const int inset = j > 24 ? (j - 24) : 0;
        gfx_fill(x, y + j, 11 - inset, 1, g_col[C_MARK]);
        gfx_fill(x + 11 + inset, y + j, 11 - inset, 1, g_col[C_MARK]);
    }
}

static int page_marked(const doc_t *d, int chap, int page) {
    if (!d->npages) return 0;
    const int32_t a = d->page_off[page < d->npages ? page : d->npages - 1];
    const int32_t b = page + A.cols < d->npages ? d->page_off[page + A.cols] : (int32_t)d->n + 1;
    for (int i = 0; i < A.nmarks; i++)
        if (A.marks[i].at.chap == chap && A.marks[i].at.off >= a && A.marks[i].at.off < b) return 1;
    return 0;
}

static void render_spread(surf_t *s, int chap, const doc_t *d, int page) {
    gfx_target(s);
    gfx_fill(0, 0, g_w, g_h, is_fixed() && g_rmode == RM_COLOR && !theme_dark(g_set.theme) ? rgb565(0x24, 0x26, 0x2A) : g_col[C_BG]);
    s_draw_chap = chap;
    s_missing = 0;
    for (int k = 0; k < A.cols; k++) {
        const int p = page + k;
        if (!d || p >= d->npages) break;
        int x = A.col_x[k];
        if (is_fixed() && A.cols == 2) {   // the two pictures meet in the middle, like an open book
            const op_t *o = NULL;
            for (int i = 0; i < d->nops; i++)
                if (d->ops[i].kind == OP_IMAGE && d->ops[i].y0 >= d->page_top[p] && d->ops[i].y0 < d->page_top[p + 1]) { o = &d->ops[i]; break; }
            if (o) x += k == 0 ? (A.lo.width - o->w) - o->x : -o->x;
        }
        doc_draw_page(d, p, x, A.text_y, k == 0 ? A.focus_link : -1);
    }
    if (!is_fixed() && g_set.status && d) {   // the status line: chapter at the top, place and progress below
        const char *ct = chapter_title(chap);
        const int tw = text_width(F_SANS, 13, ct, (int)strlen(ct));
        text_draw_fit(F_SANS, 13, (g_w - (tw < g_w - 200 ? tw : g_w - 200)) / 2, 28, ct, (int)strlen(ct), g_w - 200, g_col[C_DIM]);
        char b[80];
        const int left = d->npages - page - A.cols;
        if (A.cols == 2) {   // a page number under each page
            for (int k = 0; k < 2 && page + k < d->npages; k++) {
                snprintf(b, sizeof b, "%d", page + k + 1);
                const int w = text_width(F_SANS, 13, b, (int)strlen(b));
                text_draw(F_SANS, 13, A.col_x[k] + (A.lo.width - w) / 2, g_h - 16, b, (int)strlen(b), g_col[C_DIM]);
            }
        } else {
            snprintf(b, sizeof b, tr(S_PAGE_OF), page + 1, d->npages);
            text_draw(F_SANS, 13, A.col_x[0], g_h - 16, b, (int)strlen(b), g_col[C_DIM]);
        }
        snprintf(b, sizeof b, "%d%%", permille_at(chap, d, page) / 10);
        const int w = text_width(F_SANS, 13, b, (int)strlen(b));
        text_draw(F_SANS, 13, A.col_x[A.cols - 1] + A.lo.width - w, g_h - 16, b, (int)strlen(b), g_col[C_DIM]);
        if (A.cols == 2 && left > 0) {
            snprintf(b, sizeof b, tr(S_PAGES_LEFT), left);
            const int lw = text_width(F_SANS, 13, b, (int)strlen(b));
            (void)lw;
        }
    }
    if (d && page_marked(d, chap, page)) ribbon(g_w - 64, 0);
    gfx_target(NULL);
}

static void render_cur(void) {
    render_spread(&A.cur, A.chap, &A.doc, A.page);
    s_cur_missing = s_missing;
    A.cur_ok = 1;
}

static void show_cur(void) {
    if (!A.cur_ok) render_cur();
    gfx_copy(&A.cur, 0, g_h);
}

// --------------------------------------------------------------------------------- opening

static void marks_reload(void) {
    free(A.marks);
    A.marks = NULL;
    A.nmarks = marks_load(A.file, &A.marks);
}

int reader_open(int li) {
    if (li < 0 || li >= g_nlib) return -1;
    if (A.book) reader_close();
    char p[600];
    snprintf(p, sizeof p, "%s/%s", books_dir(), g_lib[li].file);
    gfx_fill(0, 0, g_w, g_h, g_col[C_BG]);
    draw_loading();
    book_t *b = NULL;
    const int t0 = nv_millis();
    const int r = book_open(p, &b);
    perf("book '%s' open in %d ms: %s, %d chapters, %d contents, fixed %d", g_lib[li].file, nv_millis() - t0,
         r ? "failed" : book_kind_name(b->kind), b ? b->nch : 0, b ? b->ntoc : 0, b ? b->fixed : 0);
    if (r != BKE_OK) {
        lib_update(li, NULL);
        toast(r == BKE_DRM ? tr(S_DRM) : r == BKE_FORMAT || r == BKE_EMPTY ? tr(S_FORMAT_BAD) : tr(S_OPEN_FAILED), NULL);
        A.redraw |= R_ALL;
        return -1;
    }
    if (!A.cur.px && (surf_init(&A.cur) < 0 || surf_init(&A.nxt) < 0)) {
        surf_free(&A.cur);
        surf_free(&A.nxt);
        book_close(b);
        toast(tr(S_OPEN_FAILED), NULL);
        return -1;
    }
    A.book = b;
    A.lib_idx = li;
    lib_update(li, b);
    lib_save();
    snprintf(A.file, sizeof A.file, "%s", g_lib[li].file);
    snprintf(g_set.last_file, sizeof g_set.last_file, "%s", A.file);
    memset(s_isz, 0, sizeof s_isz);
    img_clear();
    before_init();
    marks_reload();
    geometry();
    pos_t pos = {0, 0, 0};
    pos_load(A.file, &pos);
    if (pos.chap < 0 || pos.chap >= nchaps()) pos.chap = 0;
    A.chap = pos.chap;
    A.side_chap = -1;
    A.nback = 0;
    A.focus_link = -1;
    if (layout_into(&A.doc, A.chap) < 0) { reader_close(); toast(tr(S_OPEN_FAILED), NULL); return -1; }
    A.page = spread_of(doc_page_of_off(&A.doc, pos.off));
    A.cur_ok = A.nxt_ok = 0;
    A.screen = SCR_READER;
    A.sheet = SH_NONE;
    A.turns = 0;
    A.idle_since = A.now;
    render_cur();
    A.redraw |= R_ALL;
    settings_save();
    return 0;
}

void reader_save(void) {
    if (!A.book || !A.doc.npages) return;
    pos_t p;
    p.chap = A.chap;
    p.off = A.doc.page_off[A.page < A.doc.npages ? A.page : A.doc.npages - 1];
    p.permille = (int16_t)permille_at(A.chap, &A.doc, A.page);
    pos_save(A.file, &p);
    s_save_due = 0;
}

void reader_close(void) {
    if (A.book) reader_save();
    doc_free(&A.doc);
    doc_free(&A.side);
    book_close(A.book);
    A.book = NULL;
    A.side_chap = -1;
    img_clear();
    free(A.res);
    A.res = NULL;
    A.nres = 0;
    free(A.note);
    A.note = NULL;
    free(A.marks);
    A.marks = NULL;
    A.nmarks = 0;
    free(s_fx_off);
    s_fx_off = NULL;
    surf_free(&A.cur);   // the library has the memory back (covers)
    surf_free(&A.nxt);
    A.cur_ok = A.nxt_ok = 0;
    A.screen = SCR_LIBRARY;
    A.sheet = SH_NONE;
    A.osk_on = 0;
    lib_order();
    A.lib_scroll = 0;
    A.redraw |= R_ALL;
}

static void relayout(void);

void reader_reshape(void) {
    surf_free(&A.cur);
    surf_free(&A.nxt);
    if (surf_init(&A.cur) < 0 || surf_init(&A.nxt) < 0) { reader_close(); toast(tr(S_OPEN_FAILED), NULL); return; }
    img_clear();
    relayout();
}

// Lays out again after a change of settings, keeping the place.
static void relayout(void) {
    if (!A.book) return;
    const int32_t off = A.doc.npages ? A.doc.page_off[A.page < A.doc.npages ? A.page : A.doc.npages - 1] : 0;
    geometry();
    doc_free(&A.side);
    A.side_chap = -1;
    char *t = A.doc.text;
    const size_t n = A.doc.n;
    A.doc.text = NULL;
    doc_free(&A.doc);
    A.lay_chap = A.chap;
    const int t0 = nv_millis();
    doc_layout(&A.doc, t, n, &A.lo);
    perf("relayout: %d ms, %d pages", nv_millis() - t0, A.doc.npages);
    A.page = spread_of(doc_page_of_off(&A.doc, off));
    A.cur_ok = A.nxt_ok = 0;
    render_cur();
    A.redraw |= R_ALL;
}

// Goes to a place: chapter, text offset (a link, the contents, a search result, a bookmark).
static void go_to(int chap, int32_t off, int remember) {
    if (!A.book) return;
    if (is_fixed()) { off = chap >= 0 && chap < A.book->nch && s_fx_off ? s_fx_off[chap] + off : off; chap = 0; }
    if (chap < 0 || chap >= nchaps()) return;
    if (remember && A.doc.npages) {
        if (A.nback == BACK_MAX) { memmove(A.back, A.back + 1, sizeof(pos_t) * (BACK_MAX - 1)); A.nback--; }
        A.back[A.nback].chap = A.chap;
        A.back[A.nback].off = A.doc.page_off[A.page];
        A.back[A.nback].permille = (int16_t)(A.page + 1);
        A.nback++;
    }
    doc_t *d = doc_of(chap);
    if (!d) return;
    make_current(chap);
    A.page = spread_of(doc_page_of_off(&A.doc, off));
    A.cur_ok = A.nxt_ok = 0;
    A.focus_link = -1;
    s_save_due = 1;
    A.idle_since = A.now;
    render_cur();
    A.redraw |= R_ALL;
}

static void go_to_anchor(int chap, const char *anchor, int remember) {
    if (is_fixed()) { go_to(chap, 0, remember); return; }
    doc_t *d = doc_of(chap);
    if (!d) return;
    int32_t off = 0;
    if (anchor && *anchor) {
        int a = doc_anchor(d, anchor, (int)strlen(anchor));
        // a member read in parts: the id may be in a later part of it
        const chap_t *c0 = &A.book->ch[chap];
        for (int k = chap + 1; a < 0 && c0->nparts > 1 && k < A.book->nch && A.book->ch[k].path &&
                               !strcmp(A.book->ch[k].path, c0->path) && A.book->ch[k].part > 0; k++) {
            doc_t *dk = doc_of(k);
            if (!dk) break;
            a = doc_anchor(dk, anchor, (int)strlen(anchor));
            if (a >= 0) { chap = k; d = dk; }
        }
        if (a >= 0) {
            const int p = doc_page_of_y(d, d->anchors[a].y);
            off = d->page_off[p];
        }
    }
    go_to(chap, off, remember);
}

// ------------------------------------------------------------------------------- turning

// The spread after (dir > 0) or before the one on screen; 0 when there is none.
static int neighbour(int dir, int *chap, int *page) {
    if (dir > 0) {
        if (A.page + A.cols < A.doc.npages) { *chap = A.chap; *page = A.page + A.cols; return 1; }
        if (A.chap + 1 < nchaps()) { *chap = A.chap + 1; *page = 0; return 1; }
        return 0;
    }
    if (A.page > 0) { *chap = A.chap; *page = spread_of(A.page - 1); return 1; }
    if (A.chap > 0) { *chap = A.chap - 1; *page = -1; return 1; }   // its last spread, once laid out
    return 0;
}

// Draws the neighbouring spread into A.nxt (unless it is there already). 0 when there is none.
static int prepare(int dir) {
    int c, p;
    if (!neighbour(dir, &c, &p)) return 0;
    const doc_t *d = doc_of(c);
    if (!d) return 0;
    if (p < 0) p = last_spread(d);
    if (A.nxt_ok && A.nxt_chap == c && A.nxt_page == p) return 1;
    const int t0 = nv_millis();
    render_spread(&A.nxt, c, d, p);
    s_nxt_missing = s_missing;
    A.nxt_chap = c;
    A.nxt_page = p;
    A.nxt_ok = 1;
    perf("spread %d/%d drawn in %d ms", c, p, nv_millis() - t0);
    return 1;
}

// The screen during a turn: `prog` 0..1 of the way from the current spread to the prepared one.
static void compose_turn(int dir, float prog) {
    int d = (int)(prog * (float)g_w + 0.5f);
    if (d < 0) d = 0;
    if (d > g_w) d = g_w;
    // straight from the two surfaces to the canvas (gfx_turn_frame): no full-screen copy per frame
    if (g_set.anim == 1) {   // cover: the new page slides over the old one, with a shadow at its edge
        if (dir > 0) gfx_turn_frame(&A.cur, 0, &A.nxt, 0, g_w - d, g_w - d - 1, d > 4 ? 28 : 0, -1);
        else gfx_turn_frame(&A.nxt, g_w - d, &A.cur, d, d, d, d > 4 ? 28 : 0, 1);
    } else {   // slide: both pages move
        if (dir > 0) gfx_turn_frame(&A.cur, d, &A.nxt, 0, g_w - d, 0, 0, 0);
        else gfx_turn_frame(&A.nxt, g_w - d, &A.cur, 0, d, 0, 0, 0);
    }
}

static int animated(void) { return g_set.anim != 2 && g_rmode == RM_COLOR; }

static void animate(int dir, float from, float to) {
    const int ms = (int)(TURN_MS * (to > from ? to - from : from - to)) + 1;
    const int t0 = nv_millis();
    int frames = 0;
    for (;;) {
        float t = (float)(nv_millis() - t0) / (float)ms;
        if (t > 1 || (frames && input_during_turn())) t = 1;   // more input: this turn ends now
        const float e = 1 - (1 - t) * (1 - t) * (1 - t);   // ease out
        compose_turn(dir, from + (to - from) * e);
        frames++;
        if (!pump() || t >= 1) break;
    }
    perf("turn: %d frames in %d ms", frames, nv_millis() - t0);
}

// The prepared spread becomes the current one.
static void commit_turn(int dir) {
    if (A.nxt_chap != A.chap) make_current(A.nxt_chap);
    A.page = A.nxt_page;
    surf_t t = A.cur;
    A.cur = A.nxt;
    A.nxt = t;
    s_cur_missing = s_nxt_missing;
    A.cur_ok = 1;
    A.nxt_ok = 0;
    A.focus_link = -1;
    s_last_dir = dir;
    s_save_due = 1;
    A.idle_since = A.now;
}

void reader_turn(int dir, int anim) {
    if (A.book && A.book->rtl && is_fixed()) dir = -dir;   // manga: right to left
    if (!prepare(dir)) {
        if (dir > 0) toast(tr(S_BOOK_END), NULL);
        return;
    }
    if (g_rmode != RM_COLOR) {   // e-ink: instant, with the full refresh's flash every N pages
        if (++A.turns >= g_set.eink_every) {
            A.turns = 0;
            gfx_copy(&A.cur, 0, g_h);
            gfx_invert(0, g_h);
            pump();
            gfx_fill(0, 0, g_w, g_h, g_rmode == RM_MONO ? 0 : rgb565(0x11, 0x11, 0x11));
            pump();
        }
    } else if (anim && animated()) {
        animate(dir, 0, 1);
    }
    commit_turn(dir);
    A.redraw |= R_ALL;
}

// ------------------------------------------------------------------------------- the panels

static void draw_chrome(void) {
    // top: back to the library, the title, the tools
    gfx_fill(0, 0, g_w, BAR_H, g_col[C_BAR]);
    gfx_fill(0, BAR_H - 1, g_w, 1, g_col[C_RULE]);
    button(6, 6, IC_BACK, A_LIBRARY, 0, 1, g_col[C_BAR_TEXT]);   // the arrow goes back to the library
    const int right = g_w - 6 * (BTN + 4) - 8;
    text_draw_fit(F_BOLD, 18, 70, 28, A.book->title, (int)strlen(A.book->title), right - 80, g_col[C_BAR_TEXT]);
    const char *au = A.book->author[0] ? A.book->author : tr(S_UNKNOWN_AUTHOR);
    text_draw_fit(F_SANS, 14, 70, 48, au, (int)strlen(au), right - 80, g_col[C_DIM]);
    int x = right;
    button(x, 6, IC_ROTATE, A_ORIENT, 0, 1, g_col[C_BAR_TEXT]); x += BTN + 4;
    button(x, 6, IC_TOC, A_TOC, 0, 1, g_col[C_BAR_TEXT]); x += BTN + 4;
    button(x, 6, IC_SEARCH, A_SEARCH, 0, !is_fixed(), g_col[C_BAR_TEXT]); x += BTN + 4;
    button(x, 6, IC_TEXTSIZE, A_LOOK, 0, 1, g_col[C_BAR_TEXT]); x += BTN + 4;
    button(x, 6, page_marked(&A.doc, A.chap, A.page) ? IC_BOOKMARK_ON : IC_BOOKMARK, A_MARK, 0, 1,
           page_marked(&A.doc, A.chap, A.page) ? g_col[C_MARK] : g_col[C_BAR_TEXT]);
    x += BTN + 4;
    button(x, 6, IC_INFO, A_INFO, 0, 1, g_col[C_BAR_TEXT]);
    // bottom: where we are, the progress slider, the chapters before and after
    const int h = 104, y = g_h - h;
    gfx_fill(0, y, g_w, h, g_col[C_BAR]);
    gfx_fill(0, y, g_w, 1, g_col[C_RULE]);
    button(12, y + 26, IC_PREV, A_CHAP_PREV, 0, A.chap > 0 || A.page > 0, g_col[C_BAR_TEXT]);
    button(g_w - 12 - BTN, y + 26, IC_NEXT, A_CHAP_NEXT, 0, A.chap + 1 < nchaps() || A.page + A.cols < A.doc.npages,
           g_col[C_BAR_TEXT]);
    const int sx = 12 + BTN + 24, sw = g_w - 2 * sx, sy = y + 62;
    const int pm = s_slider_pm >= 0 ? s_slider_pm : permille_at(A.chap, &A.doc, A.page);
    reg_add(sx - 10, sy - 22, sw + 20, 44, A_SLIDER, 0);
    gfx_rrect(sx, sy - 3, sw, 6, 3, g_col[C_RULE]);
    // the chapters as ticks
    if (!is_fixed() && s_before && A.book->total && A.book->nch < 120)
        for (int i = 1; i < A.book->nch; i++) gfx_fill(sx + (int)((double)s_before[i] / A.book->total * sw), sy - 6, 1, 12, g_col[C_DIM]);
    gfx_rrect(sx, sy - 3, sw * pm / 1000, 6, 3, g_col[C_ACCENT]);
    const int kx = sx + sw * pm / 1000;
    gfx_rrect(kx - 11, sy - 11, 22, 22, 11, g_col[C_ACCENT]);
    gfx_rrect(kx - 5, sy - 5, 10, 10, 5, g_col[C_BAR]);
    char b[200];
    if (s_slider_pm >= 0 && !is_fixed() && s_before) {   // where the slider would go
        int c = 0;
        while (c + 1 < A.book->nch && s_before[c + 1] <= (uint64_t)((double)pm / 1000 * A.book->total)) c++;
        snprintf(b, sizeof b, "%s  \xC2\xB7  %d%%", chapter_title(c)[0] ? chapter_title(c) : "", pm / 10);
    } else if (is_fixed()) {
        snprintf(b, sizeof b, tr(S_PAGE_OF), A.page + 1, A.doc.npages);
    } else {
        char pg[40];
        snprintf(pg, sizeof pg, tr(S_PAGE_OF), A.page + 1, A.doc.npages);
        const char *ct = chapter_title(A.chap);
        snprintf(b, sizeof b, "%s%s%s  \xC2\xB7  %d%%", ct, ct[0] ? "  \xC2\xB7  " : "", pg, pm / 10);
    }
    const int bw = text_width(F_SANS, 15, b, (int)strlen(b));
    text_draw_fit(F_SANS, 15, (g_w - (bw < sw ? bw : sw)) / 2, y + 30, b, (int)strlen(b), sw, g_col[C_BAR_TEXT]);
    if (A.nback > 0) {   // back to where a link was followed from
        snprintf(b, sizeof b, tr(S_BACK_TO), (int)A.back[A.nback - 1].permille);
        const int pw = text_width(F_SANS, 17, b, (int)strlen(b)) + 32;
        pill((g_w - pw) / 2, y - 56, b, A_BACK_JUMP, 0, 1);
    }
}

static int toc_current(void) {   // the contents entry of the place on screen
    int best = -1;
    for (int i = 0; i < A.book->ntoc; i++) {
        const tocent_t *t = &A.book->toc[i];
        int c = t->chap;
        if (is_fixed() && s_fx_off) {   // pages: compare by page
            const int p = doc_page_of_off(&A.doc, s_fx_off[c < A.book->nch ? c : 0]);
            if (p <= A.page) best = i;
            continue;
        }
        if (c < A.chap) best = i;
        else if (c == A.chap) {
            if (!t->anchor[0]) best = i;
            else {
                const int a = doc_anchor(&A.doc, t->anchor, (int)strlen(t->anchor));
                if (a < 0 || doc_page_of_y(&A.doc, A.doc.anchors[a].y) <= A.page + A.cols - 1) best = i;
            }
        }
    }
    return best;
}

#define LIST_ROW 54

static void draw_toc(void) {
    scrim();
    const int w = g_w - 48 < 470 ? g_w - 48 : 470, x = 0, top = 0;
    gfx_fill(x, top, w, g_h, g_col[C_BG]);
    gfx_fill(w, 0, 1, g_h, g_col[C_RULE]);
    // tabs
    const char *tabs[2] = {tr(S_CONTENTS), tr(S_BOOKMARKS)};
    segmented(16, 12, w - 16 - BTN - 20, tabs, 2, A.list_tab, A_TOC_TAB);
    button(w - BTN - 8, 8, IC_CLOSE, A_CLOSE, 0, 1, g_col[C_DIM]);
    const int ly = 70;
    gfx_clip(0, ly, w, g_h);
    if (A.list_tab == 0) {
        const int cur = toc_current();
        if (!A.book->ntoc) {   // no contents: the chapters by number
            for (int i = 0; i < nchaps(); i++) {
                const int ry = ly + i * LIST_ROW - A.list_scroll;
                if (ry + LIST_ROW < ly || ry > g_h) continue;
                char t[40];
                snprintf(t, sizeof t, tr(S_CHAPTER_N), i + 1);
                row_box(8, ry + 2, w - 16, LIST_ROW - 4, A_TOC_ITEM, -1 - i, i == A.chap ? g_col[C_CARD] : g_col[C_BG]);
                text_draw(i == A.chap ? F_BOLD : F_SANS, 17, 28, ry + 34, t, (int)strlen(t), g_col[C_TEXT]);
            }
        }
        for (int i = 0; i < A.book->ntoc; i++) {
            const int ry = ly + i * LIST_ROW - A.list_scroll;
            if (ry + LIST_ROW < ly) continue;
            if (ry > g_h) break;
            const tocent_t *t = &A.book->toc[i];
            row_box(8, ry + 2, w - 16, LIST_ROW - 4, A_TOC_ITEM, i, i == cur ? g_col[C_CARD] : g_col[C_BG]);
            if (i == cur) gfx_rrect(8, ry + 12, 4, LIST_ROW - 24, 2, g_col[C_ACCENT]);
            const int ind = 28 + t->level * 22;
            text_draw_fit(i == cur || t->level == 0 ? F_BOLD : F_SANS, t->level ? 16 : 17, ind, ry + 34, t->title,
                          (int)strlen(t->title), w - ind - 28, i == cur ? g_col[C_ACCENT] : g_col[C_TEXT]);
        }
    } else {
        if (!A.nmarks) wrap_text(F_SANS, 16, 28, ly + 40, w - 56, 22, tr(S_NO_BOOKMARKS), 4, g_col[C_DIM]);
        for (int i = 0; i < A.nmarks; i++) {
            const int ry = ly + i * 76 - A.list_scroll;
            if (ry + 76 < ly) continue;
            if (ry > g_h) break;
            const mark_t *m = &A.marks[i];
            row_box(8, ry + 2, w - 16 - BTN, 72, A_MARK_ITEM, i, g_col[C_BG]);
            char b[120];
            snprintf(b, sizeof b, "%s  \xC2\xB7  %d%%", chapter_title(m->at.chap), m->at.permille / 10);
            text_draw_fit(F_BOLD, 14, 28, ry + 26, b, (int)strlen(b), w - 56 - BTN, g_col[C_DIM]);
            wrap_text(F_SERIF, 16, 28, ry + 50, w - 56 - BTN, 20, m->note, 1, g_col[C_TEXT]);
            button(w - BTN - 8, ry + 12, IC_TRASH, A_MARK_DEL, i, 1, g_col[C_DIM]);
        }
    }
    gfx_noclip();
}

static void look_row_label(int x, int y, int sid) { text_draw(F_BOLD, 14, x, y, tr(sid), (int)strlen(tr(sid)), g_col[C_DIM]); }

static void toggle(int x, int y, int w, int sid, int on, int arg) {
    const int r = reg_add(x, y, w, 44, A_TOGGLE, arg);
    const int st = reg_state(r);
    gfx_rrect(x, y, w, 44, 12, st == 2 ? g_col[C_PRESS] : g_col[C_CARD]);
    if (st == 1) gfx_ring(x, y, w, 44, 12, 2, g_col[C_FOCUS]);
    text_draw_fit(F_SANS, 15, x + 14, y + 28, tr(sid), (int)strlen(tr(sid)), w - 80, g_col[C_TEXT]);
    const int sx = x + w - 56;   // a switch
    gfx_rrect(sx, y + 11, 44, 22, 11, on ? g_col[C_ACCENT] : g_col[C_RULE]);
    gfx_rrect(on ? sx + 24 : sx + 2, y + 13, 18, 18, 9, on ? g_col[C_ON_ACCENT] : g_col[C_BG]);
}

static void draw_look(void) {
    // landscape: choices on the left, switches on the right; portrait: one column, taller
    const int wide = g_w >= 900, nl = is_fixed() ? 2 : 5, nt = is_fixed() ? 1 : 5;
    const int h = 168 + (wide ? (nl > nt ? nl : nt) : nl + nt) * 56, y = g_h - h;
    gfx_fill(0, y, g_w, h, g_col[C_BG]);
    gfx_fill(0, y, g_w, 1, g_col[C_RULE]);
    button(g_w - BTN - 8, y + 6, IC_CLOSE, A_CLOSE, 0, 1, g_col[C_DIM]);
    const char *t = tr(S_APPEARANCE);
    text_draw(F_BOLD, 20, 24, y + 36, t, (int)strlen(t), g_col[C_TEXT]);
    // themes: swatches showing their paper and ink
    static const int kName[TH_COUNT] = {S_TH_WHITE, S_TH_SEPIA, S_TH_DARK, S_TH_BLACK, S_TH_EINK, S_TH_MONO};
    static const uint32_t kPaper[TH_COUNT] = {0xFFFFFF, 0xF4ECD8, 0x1E2024, 0x000000, 0xEEEEEE, 0xFFFFFF};
    static const uint32_t kInk[TH_COUNT] = {0x1E1F22, 0x3B2E1E, 0xD6D8DC, 0xB4B8BE, 0x111111, 0x000000};
    const int gap = wide ? 14 : 8, sw = wide ? 112 : (g_w - 48 - 5 * gap) / 6, sh = 62, sx0 = 24, sy = y + 58;
    for (int i = 0; i < TH_COUNT; i++) {
        const int x = sx0 + i * (sw + gap);
        const int r = reg_add(x, sy, sw, sh + 26, A_THEME, i);
        const uint16_t paper = rgb565((int)(kPaper[i] >> 16), (int)(kPaper[i] >> 8 & 255), (int)(kPaper[i] & 255));
        const uint16_t ink = rgb565((int)(kInk[i] >> 16), (int)(kInk[i] >> 8 & 255), (int)(kInk[i] & 255));
        gfx_rrect(x, sy, sw, sh, 12, g_rmode == RM_COLOR ? paper : (i >= TH_DARK && i <= TH_BLACK ? g_col[C_TEXT] : g_col[C_BG]));
        gfx_ring(x, sy, sw, sh, 12, i == g_set.theme ? 3 : 1, i == g_set.theme ? g_col[C_ACCENT] : g_col[C_RULE]);
        if (reg_state(r) == 1) gfx_ring(x - 4, sy - 4, sw + 8, sh + 8, 15, 2, g_col[C_FOCUS]);
        const uint16_t tc = g_rmode == RM_COLOR ? ink : (i >= TH_DARK && i <= TH_BLACK ? g_col[C_BG] : g_col[C_TEXT]);
        text_draw(F_SERIF, 26, x + sw / 2 - text_width(F_SERIF, 26, "Aa", 2) / 2, sy + 42, "Aa", 2, tc);
        if (i == TH_EINK) for (int k = 0; k < 4; k++) gfx_fill(x + 14 + k * 6, sy + sh - 12, 4, 4, g_rmode == RM_COLOR ? ink : tc);
        const char *n = tr(kName[i]);
        text_draw(i == g_set.theme ? F_BOLD : F_SANS, 14, x + (sw - text_width(F_SANS, 14, n, (int)strlen(n))) / 2, sy + sh + 20,
                  n, (int)strlen(n), g_col[C_TEXT]);
    }
    // two columns of settings, LOOK_ROW apart: choices on the left, switches on the right
    const int c1 = 24, c2 = wide ? g_w / 2 + 12 : c1, cw = wide ? g_w / 2 - 48 : g_w - 48, lw = 130, r0 = y + 160;
    int row = 0;
#define LOOK_ROW 56
    if (!is_fixed()) {
        int ry = r0 + row * LOOK_ROW;
        look_row_label(c1, ry + 28, S_FONT);
        const char *fonts[2] = {tr(S_SERIF), tr(S_SANS)};
        segmented(c1 + lw, ry, 180, fonts, 2, g_set.serif ? 0 : 1, A_FONT);
        const int bx = c1 + lw + 192;   // size: < N >
        button(bx, ry - 4, IC_PREV, A_SIZE, -2, g_set.size > 14, g_col[C_TEXT]);
        char n[8];
        snprintf(n, sizeof n, "%d", g_set.size);
        text_draw(F_BOLD, 18, bx + BTN + 4, ry + 29, n, (int)strlen(n), g_col[C_TEXT]);
        button(bx + BTN + 32, ry - 4, IC_NEXT, A_SIZE, 2, g_set.size < 40, g_col[C_TEXT]);
        ry = r0 + ++row * LOOK_ROW;
        look_row_label(c1, ry + 28, S_SPACING);
        const char *sp[4] = {"1.25", "1.4", "1.55", "1.75"};
        segmented(c1 + lw, ry, cw - lw, sp, 4, g_set.spacing, A_SPACING);
        ry = r0 + ++row * LOOK_ROW;
        look_row_label(c1, ry + 28, S_MARGINS);
        const char *mg[4] = {"S", "M", "L", "XL"};
        segmented(c1 + lw, ry, cw - lw, mg, 4, g_set.margin, A_MARGIN);
        row++;
    }
    int ry = r0 + row * LOOK_ROW;
    look_row_label(c1, ry + 28, S_COLUMNS);
    const char *co[3] = {tr(S_AUTO), "1", "2"};
    segmented(c1 + lw, ry, cw - lw, co, 3, g_set.cols, A_COLS);
    ry = r0 + ++row * LOOK_ROW;
    if (g_rmode == RM_COLOR) {
        look_row_label(c1, ry + 28, S_TURN);
        const char *an[3] = {tr(S_TURN_SLIDE), tr(S_TURN_COVER), tr(S_TURN_NONE)};
        segmented(c1 + lw, ry, cw - lw, an, 3, g_set.anim, A_ANIM);
    } else {   // paper themes turn without animation: how often the screen refreshes in full instead
        char b[80];
        snprintf(b, sizeof b, tr(S_EINK_REFRESH), g_set.eink_every);
        gfx_rrect(c1, ry, cw, 44, 12, g_col[C_CARD]);
        text_draw_fit(F_SANS, 15, c1 + 14, ry + 28, b, (int)strlen(b), cw - 2 * BTN - 24, g_col[C_TEXT]);
        button(c1 + cw - 2 * BTN - 4, ry - 4, IC_PREV, A_EINK_N, -1, g_set.eink_every > 1, g_col[C_TEXT]);
        button(c1 + cw - BTN, ry - 4, IC_NEXT, A_EINK_N, 1, g_set.eink_every < 10, g_col[C_TEXT]);
    }
    int ty = wide ? r0 : r0 + nl * LOOK_ROW;
    if (!is_fixed()) {
        toggle(c2, ty, cw, S_JUSTIFY, g_set.justify, TG_JUSTIFY); ty += LOOK_ROW;
        toggle(c2, ty, cw, S_HYPHEN, g_set.hyphen, TG_HYPHEN); ty += LOOK_ROW;
        toggle(c2, ty, cw, S_INDENT, g_set.indent, TG_INDENT); ty += LOOK_ROW;
        toggle(c2, ty, cw, S_STATUS_LINE, g_set.status, TG_STATUS); ty += LOOK_ROW;
    }
    toggle(c2, ty, cw, S_TAP_ZONES, g_set.tap_zones, TG_TAP);
#undef LOOK_ROW
}

static void draw_search(void) {
    scrim();
    const int w = g_w - 32 < 640 ? g_w - 32 : 640, x = (g_w - w) / 2, h = A.osk_on ? g_h - OSK_H - 16 : g_h - 32;
    sheet_frame(x, 16, w, h);
    draw_field(x + 16, 30, w - 32 - BTN - 8, tr(S_SEARCH_IN));
    button(x + w - BTN - 12, 26, IC_CLOSE, A_CLOSE, 0, 1, g_col[C_DIM]);
    const int ly = 90;
    if (A.searched) {
        char b[60];
        if (!A.nres) snprintf(b, sizeof b, "%s", tr(S_NO_RESULTS));
        else snprintf(b, sizeof b, tr(S_RESULTS), A.nres);
        text_draw(F_BOLD, 14, x + 28, ly, b, (int)strlen(b), g_col[C_DIM]);
    }
    gfx_clip(x, ly + 10, x + w, 16 + h - 8);
    for (int i = 0; i < A.nres; i++) {
        const int ry = ly + 12 + i * 70 - A.list_scroll;
        if (ry + 70 < ly) continue;
        if (ry > 16 + h) break;
        const result_t *r = &A.res[i];
        row_box(x + 12, ry, w - 24, 66, A_RESULT, i, g_col[C_BG]);
        const char *ct = chapter_title(r->chap);
        text_draw_fit(F_BOLD, 13, x + 28, ry + 22, ct, (int)strlen(ct), w - 56, g_col[C_DIM]);
        // the snippet with the match in bold
        int px = x + 28;
        px += text_draw_fit(F_SERIF, 16, px, ry + 48, r->snip, r->sl, w - 56, g_col[C_TEXT]);
        px += text_draw(F_SERIF_BOLD, 16, px, ry + 48, r->snip + r->sl, r->sn, g_col[C_ACCENT]);
        const char *rest = r->snip + r->sl + r->sn;
        if (px < x + w - 40) text_draw_fit(F_SERIF, 16, px, ry + 48, rest, (int)strlen(rest), x + w - 28 - px, g_col[C_TEXT]);
    }
    gfx_noclip();
}

static void draw_note(void) {
    scrim();
    const int w = g_w - 32 < 620 ? g_w - 32 : 620, x = (g_w - w) / 2, h = 300, y = g_h - h - 24;
    sheet_frame(x, y, w, h);
    const char *t = tr(S_NOTE);
    text_draw(F_BOLD, 15, x + 24, y + 34, t, (int)strlen(t), g_col[C_DIM]);
    button(x + w - BTN - 8, y + 6, IC_CLOSE, A_CLOSE, 0, 1, g_col[C_DIM]);
    wrap_text(F_SERIF, 18, x + 24, y + 70, w - 48, 26, A.note ? A.note : "", 7, g_col[C_TEXT]);
    const char *g = tr(S_GO_TO);
    const int bw = text_width(F_BOLD, 16, g, (int)strlen(g)) + 48;
    pill(x + w - bw - 24, y + h - 58, g, A_NOTE_GO, 0, 1);
}

static uint16_t *s_view_px;   // the picture viewer's pixels
static int s_view_w, s_view_h, s_view_zoom;

static void viewer_load(void) {
    free(s_view_px);
    s_view_px = NULL;
    if (A.view_img < 0 || A.view_img >= A.doc.nimgs) return;
    const dimg_t *im = &A.doc.imgs[A.view_img];
    uint8_t *data;
    size_t n;
    if (book_resource(A.book, s_draw_chap = A.chap, A.doc.text + im->src_off, im->src_len, &data, &n)) return;
    int iw, ih;
    if (img_size(data, n, &iw, &ih)) { free(data); return; }
    // fitted to the screen, times the zoom (1x, 2x), at most a million pixels (2 MB, 6 MB while it is
    // decoded); with less memory free, the page pictures' cache goes first, then the size comes down
    long s = (long)g_w * 1000 / iw < (long)g_h * 1000 / ih ? (long)g_w * 1000 / iw : (long)g_h * 1000 / ih;
    s = s * A.zoom100 / 100;
    int w = (int)(iw * s / 1000), h = (int)(ih * s / 1000);
    while ((long)w * h > 1000000L) { w = w * 9 / 10; h = h * 9 / 10; }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    s_view_px = img_render565(data, n, w, h, g_col[C_SCRIM], 0);
    if (!s_view_px) {
        img_clear();
        s_view_px = img_render565(data, n, w, h, g_col[C_SCRIM], 0);
    }
    while (!s_view_px && (long)w * h > 90000L) {
        w = w * 4 / 5;
        h = h * 4 / 5;
        s_view_px = img_render565(data, n, w, h, g_col[C_SCRIM], 0);
    }
    if (!s_view_px) perf("viewer: no memory for the picture");
    free(data);
    s_view_w = w;
    s_view_h = h;
    s_view_zoom = A.zoom100;
}

static void draw_viewer(void) {
    gfx_fill(0, 0, g_w, g_h, g_rmode == RM_COLOR ? 0 : g_col[C_BG]);
    if (!s_view_px || s_view_zoom != A.zoom100) viewer_load();
    if (s_view_px) {
        // centred, then panned (clamped to the picture)
        const int mx = s_view_w > g_w ? (s_view_w - g_w) / 2 : 0, my = s_view_h > g_h ? (s_view_h - g_h) / 2 : 0;
        if (A.pan_x > mx) A.pan_x = mx;
        if (A.pan_x < -mx) A.pan_x = -mx;
        if (A.pan_y > my) A.pan_y = my;
        if (A.pan_y < -my) A.pan_y = -my;
        const int x0 = (g_w - s_view_w) / 2 + A.pan_x, y0 = (g_h - s_view_h) / 2 + A.pan_y;
        for (int j = 0; j < s_view_h; j++) {
            const int y = y0 + j;
            if (y < 0 || y >= g_h) continue;
            int a = 0, b = s_view_w;
            if (x0 < 0) a = -x0;
            if (x0 + b > g_w) b = g_w - x0;
            if (b > a) memcpy(g_rows[y] + x0 + a, s_view_px + (size_t)j * s_view_w + a, (size_t)(b - a) * 2);
        }
        gfx_dirty(0, g_h);
    }
    button(g_w - BTN - 12, 12, IC_CLOSE, A_CLOSE, 0, 1, g_rmode == RM_COLOR ? rgb565(0xEE, 0xEE, 0xEE) : g_col[C_TEXT]);
}

static void info_line(int x, int *y, int w, int key, const char *v) {
    if (!v || !*v) return;
    text_draw(F_SANS, 14, x, *y, tr(key), (int)strlen(tr(key)), g_col[C_DIM]);
    text_draw_fit(F_SANS, 16, x + 150, *y, v, (int)strlen(v), w - 150, g_col[C_TEXT]);
    *y += 30;
}

static void draw_info(void) {
    scrim();
    const int w = g_w - 32 < 720 ? g_w - 32 : 720, h = g_h - 80 < 520 ? g_h - 80 : 520, x = (g_w - w) / 2, y = (g_h - h) / 2;
    sheet_frame(x, y, w, h);
    button(x + w - BTN - 8, y + 8, IC_CLOSE, A_CLOSE, 0, 1, g_col[C_DIM]);
    if (A.lib_idx >= 0) draw_cover(A.lib_idx, x + 28, y + 28, COVER_W, COVER_H);
    const int tx = x + 28 + COVER_W + 28, tw = w - (tx - x) - 28;
    wrap_text(F_SERIF_BOLD, 22, tx, y + 58, tw - 40, 28, A.book->title, 2, g_col[C_TEXT]);
    int ry = y + 128;
    char sz[32], ch[16];
    fmt_size((uint64_t)A.book->file_size, sz, sizeof sz);
    snprintf(ch, sizeof ch, "%d", A.book->nch);
    info_line(tx, &ry, tw, S_F_AUTHOR, A.book->author);
    info_line(tx, &ry, tw, S_F_LANG, lang_name(A.book->lang));
    info_line(tx, &ry, tw, S_F_PUBLISHER, A.book->publisher);
    info_line(tx, &ry, tw, S_F_DATE, A.book->date);
    info_line(tx, &ry, tw, S_F_SUBJECT, A.book->subject);
    info_line(tx, &ry, tw, S_F_FORMAT, book_kind_name(A.book->kind));
    info_line(tx, &ry, tw, S_F_SIZE, sz);
    info_line(tx, &ry, tw, S_F_CHAPTERS, ch);
    info_line(tx, &ry, tw, S_F_RIGHTS, A.book->rights);
    if (A.book->desc && *A.book->desc) {
        const int dy = ry > y + 28 + COVER_H + 20 ? ry + 10 : y + 28 + COVER_H + 34;
        wrap_text(F_SERIF, 16, x + 28, dy, w - 56, 22, A.book->desc, (y + h - 20 - dy) / 22, g_col[C_TEXT]);
    }
}

void reader_draw(void) {
    if (A.sheet == SH_VIEWER) { draw_viewer(); return; }
    show_cur();
    switch (A.sheet) {
    case SH_CHROME: draw_chrome(); break;
    case SH_TOC: draw_toc(); break;
    case SH_LOOK: draw_look(); break;
    case SH_SEARCH: draw_search(); break;
    case SH_NOTE: draw_note(); break;
    case SH_INFO: draw_info(); break;
    default: break;
    }
}

// --------------------------------------------------------------------------------- search

static int fold(int c) {   // ASCII case folding (accents compare as bytes)
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

// The plain text of a chapter (markers, link targets and picture fields dropped) for searching.
static char *plain_of(const char *t, size_t n, int32_t **map, size_t *out) {
    char *o = (char *)malloc(n + 1);
    int32_t *m = (int32_t *)malloc(sizeof(int32_t) * (n + 1));
    if (!o || !m) { free(o); free(m); return NULL; }
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)t[i];
        if (c == NVHTML_LINK) { while (i < n && t[i] != NVHTML_LINK_TEXT) i++; continue; }
        if (c == NVHTML_IMAGE) { while (i < n && t[i] != NVHTML_IMAGE_END) i++; continue; }
        if (c == RT_ID) { while (i < n && t[i] != RT_ID_END) i++; continue; }
        if (c == NVHTML_ANCHOR) { while (i < n && t[i] != '\n') i++; if (i < n) { o[k] = ' '; m[k++] = (int32_t)i; } continue; }
        if (c == '\n') { if (k && o[k - 1] != ' ') { o[k] = ' '; m[k++] = (int32_t)i; } continue; }
        if (c < 0x20) continue;
        o[k] = (char)c;
        m[k++] = (int32_t)i;
    }
    o[k] = 0;
    *map = m;
    *out = k;
    return o;
}

void reader_search_run(void) {
    if (!A.book || !A.q[0]) return;
    free(A.res);
    A.res = (result_t *)calloc(RES_MAX, sizeof(result_t));
    A.nres = 0;
    A.list_scroll = 0;
    if (!A.res) return;
    const size_t ql = strlen(A.q);
    const int t0 = nv_millis();
    for (int c = 0; c < A.book->nch && A.nres < RES_MAX; c++) {
        char msg[80];
        snprintf(msg, sizeof msg, tr(S_SEARCHING), c + 1, A.book->nch);
        A.redraw |= R_ALL;
        snprintf(A.toast, sizeof A.toast, "%s", msg);
        A.toast_until = A.now + 1000;
        if (!(c & 3)) {
            A.nregs = 0;
            reader_draw();
            draw_toast();
            if (!pump()) return;
        }
        char *t = NULL;
        size_t n = 0;
        if (book_chapter(A.book, c, &t, &n)) { free(t); continue; }
        int32_t *map;
        size_t pn;
        char *p = plain_of(t, n, &map, &pn);
        free(t);
        if (!p) continue;
        for (size_t i = 0; i + ql <= pn && A.nres < RES_MAX; i++) {
            size_t j = 0;
            while (j < ql && fold((unsigned char)p[i + j]) == fold((unsigned char)A.q[j])) j++;
            if (j < ql) continue;
            result_t *r = &A.res[A.nres++];
            r->chap = c;
            r->off = map[i];
            // a snippet: a few words before, the match, what follows
            size_t s = i > 40 ? i - 40 : 0;
            while (s > 0 && s < i && p[s] != ' ') s++;
            while (s < i && ((unsigned char)p[s] & 0xC0) == 0x80) s++;
            const size_t pre = i - s;
            size_t e = i + ql + 90 < pn ? i + ql + 90 : pn;
            while (e > i + ql && e < pn && ((unsigned char)p[e] & 0xC0) == 0x80) e--;
            snprintf(r->snip, sizeof r->snip, "%s%.*s", s > 0 ? "\xE2\x80\xA6" : "", (int)(e - s), p + s);
            r->sl = (int)pre + (s > 0 ? 3 : 0);
            r->sn = (int)ql;
            if (r->sl + r->sn > (int)strlen(r->snip)) { r->sl = 0; r->sn = 0; }
            i += ql - 1;
        }
        free(p);
        free(map);
    }
    perf("search '%s': %d results in %d ms", A.q, A.nres, nv_millis() - t0);
    A.searched = 1;
    A.toast[0] = 0;
    A.redraw |= R_ALL;
}

// -------------------------------------------------------------------------------- links

static int is_note_text(const char *s, int n) {   // "12", "[3]", "*", "†": a note's call
    int vis = 0;
    for (int i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == ' ') continue;
        if (!(isdigit(c) || strchr("[]()*", c) || c >= 0x80)) return 0;
        vis++;
    }
    return vis > 0 && vis <= 6;
}

static void follow(int link) {
    if (link < 0 || link >= A.doc.nlinks) return;
    const link_t *l = &A.doc.links[link];
    const char *href = A.doc.text + l->off;
    char anchor[NVHTML_HREF_MAX];
    const int c = book_link(A.book, is_fixed() ? 0 : A.chap, href, l->len, anchor, sizeof anchor);
    if (c == -1) { toast(tr(S_EXTERNAL_LINK), anchor); return; }
    if (c < 0) { toast(tr(S_NOT_FOUND), NULL); return; }
    // the link's own text: a note's number becomes a popup instead of a jump
    const char *lt = memchr(href, NVHTML_LINK_TEXT, (size_t)(A.doc.n - (size_t)l->off));
    int tn = 0;
    if (lt) { const char *e = memchr(lt, NVHTML_LINK_END, (size_t)(A.doc.text + A.doc.n - lt)); tn = e ? (int)(e - lt - 1) : 0; }
    // FB2 notes live in their own body: an id anywhere in the book
    int tc = c;
    if (A.book->kind == BK_FB2 && anchor[0]) {
        const doc_t *d = doc_of(tc);
        if (d && doc_anchor(d, anchor, (int)strlen(anchor)) < 0)
            for (int i = 0; i < A.book->nch; i++) {
                if (i == tc) continue;
                char *t = NULL;
                size_t n = 0;
                if (book_chapter(A.book, i, &t, &n)) { free(t); continue; }
                char idm[NVHTML_HREF_MAX + 4];
                snprintf(idm, sizeof idm, "%c%s%c", RT_ID, anchor, RT_ID_END);
                const int hit = n && strstr(t, idm) != NULL;
                free(t);
                if (hit) { tc = i; break; }
            }
    }
    if (anchor[0] && lt && is_note_text(lt + 1, tn)) {
        doc_t *d = doc_of(tc);
        const int a = d ? doc_anchor(d, anchor, (int)strlen(anchor)) : -1;
        if (a >= 0) {
            free(A.note);
            A.note = doc_para_text(d, d->anchors[a].toff, 1400);
            A.note_chap = tc;
            A.note_off = d->page_off[doc_page_of_y(d, d->anchors[a].y)];
            A.sheet = SH_NOTE;
            A.redraw |= R_ALL;
            return;
        }
    }
    go_to_anchor(tc, anchor, 1);
}

// ------------------------------------------------------------------------------- actions

static void bookmark_toggle(void) {
    if (!A.doc.npages) return;
    const int32_t a = A.doc.page_off[A.page], b = A.page + A.cols < A.doc.npages ? A.doc.page_off[A.page + A.cols] : (int32_t)A.doc.n + 1;
    int found = -1;
    for (int i = 0; i < A.nmarks && found < 0; i++)
        if (A.marks[i].at.chap == A.chap && A.marks[i].at.off >= a && A.marks[i].at.off < b) found = i;
    if (found >= 0) {
        marks_remove(A.file, &A.marks[found].at);
        toast(tr(S_BOOKMARK_REMOVED), NULL);
    } else {
        mark_t m;
        memset(&m, 0, sizeof m);
        m.at.chap = A.chap;
        m.at.off = a;
        m.at.permille = (int16_t)permille_at(A.chap, &A.doc, A.page);
        m.when = nv_time_unix();
        char *t = doc_para_text(&A.doc, a, 180);
        snprintf(m.note, sizeof m.note, "%s", t ? t : "");
        free(t);
        marks_add(A.file, &m);
        toast(tr(S_BOOKMARK_ADDED), NULL);
    }
    marks_reload();
    A.cur_ok = A.nxt_ok = 0;
    render_cur();
}

static void set_changed(int relay) {
    settings_save();
    if (relay) relayout();
    else { A.cur_ok = A.nxt_ok = 0; render_cur(); }
    A.redraw |= R_ALL;
}

int reader_action(int act, int arg) {
    A.idle_since = A.now;
    switch (act) {
    case A_CLOSE:
        if (A.sheet == SH_VIEWER) { free(s_view_px); s_view_px = NULL; }
        A.sheet = A.sheet == SH_SEARCH || A.sheet == SH_LOOK || A.sheet == SH_TOC || A.sheet == SH_INFO ? SH_NONE : SH_NONE;
        A.osk_on = 0;
        A.redraw |= R_ALL;
        return 1;
    case A_TOC: A.sheet = SH_TOC; A.list_scroll = 0; A.list_tab = 0; marks_reload();
        {   // the current entry in view
            const int c = toc_current();
            if (c > 4) A.list_scroll = (c - 4) * LIST_ROW;
        }
        return 1;
    case A_TOC_TAB: A.list_tab = arg; A.list_scroll = 0; return 1;
    case A_LIBRARY: reader_close(); return 1;
    case A_TOC_ITEM:
        A.sheet = SH_NONE;
        if (arg < 0) go_to(-1 - arg, 0, 1);
        else if (arg < A.book->ntoc) go_to_anchor(A.book->toc[arg].chap, A.book->toc[arg].anchor, 1);
        return 1;
    case A_MARK_ITEM:
        if (arg >= 0 && arg < A.nmarks) { A.sheet = SH_NONE; go_to(A.marks[arg].at.chap, A.marks[arg].at.off, 1); }
        return 1;
    case A_MARK_DEL:
        if (arg >= 0 && arg < A.nmarks) { marks_remove(A.file, &A.marks[arg].at); marks_reload(); A.cur_ok = A.nxt_ok = 0; render_cur(); }
        return 1;
    case A_SEARCH: A.sheet = SH_SEARCH; A.osk_on = !kbd_present(); A.list_scroll = 0; return 1;
    case A_RESULT:
        if (arg >= 0 && arg < A.nres) { A.sheet = SH_NONE; A.osk_on = 0; go_to(A.res[arg].chap, A.res[arg].off, 1); }
        return 1;
    case A_LOOK: A.sheet = SH_LOOK; return 1;
    case A_INFO: A.sheet = SH_INFO; return 1;
    case A_MARK: bookmark_toggle(); return 1;
    case A_THEME: g_set.theme = arg; theme_apply(); img_clear(); set_changed(0); return 1;
    case A_FONT: g_set.serif = arg == 0; set_changed(1); return 1;
    case A_SIZE: g_set.size += arg; if (g_set.size < 14) g_set.size = 14; if (g_set.size > 40) g_set.size = 40; set_changed(1); return 1;
    case A_SPACING: g_set.spacing = arg; set_changed(1); return 1;
    case A_MARGIN: g_set.margin = arg; set_changed(1); return 1;
    case A_ANIM: g_set.anim = arg; settings_save(); return 1;
    case A_COLS: g_set.cols = arg; set_changed(1); return 1;
    case A_EINK_N: g_set.eink_every += arg; if (g_set.eink_every < 1) g_set.eink_every = 1; if (g_set.eink_every > 10) g_set.eink_every = 10; settings_save(); return 1;
    case A_TOGGLE:
        switch (arg) {
        case TG_JUSTIFY: g_set.justify = !g_set.justify; set_changed(1); break;
        case TG_HYPHEN: g_set.hyphen = !g_set.hyphen; set_changed(1); break;
        case TG_INDENT: g_set.indent = !g_set.indent; set_changed(1); break;
        case TG_STATUS: g_set.status = !g_set.status; set_changed(1); break;
        case TG_TAP: g_set.tap_zones = !g_set.tap_zones; settings_save(); break;
        default: break;
        }
        return 1;
    case A_NOTE_GO: A.sheet = SH_NONE; go_to(A.note_chap, A.note_off, 1); return 1;
    case A_BACK_JUMP:
        if (A.nback > 0) {
            const pos_t p = A.back[--A.nback];
            go_to(p.chap, p.off, 0);
        }
        return 1;
    case A_CHAP_PREV:   // the start of this chapter, or of the one before
        if (A.page > 0) go_to(A.chap, 0, 0);
        else if (A.chap > 0) go_to(A.chap - 1, 0, 0);
        return 1;
    case A_CHAP_NEXT:
        if (A.chap + 1 < nchaps()) go_to(A.chap + 1, 0, 0);
        else go_to(A.chap, A.doc.page_off[last_spread(&A.doc)], 0);
        return 1;
    case A_SLIDER: {
        const int sx = 12 + BTN + 24, sw = g_w - 2 * sx;
        if (arg >= 0) {   // dragging: where it would go
            int pm = (arg - sx) * 1000 / sw;
            s_slider_pm = pm < 0 ? 0 : pm > 1000 ? 1000 : pm;
            A.redraw |= R_ALL;
            return 1;
        }
        const int pm = s_slider_pm;   // released: go there
        s_slider_pm = -1;
        if (pm < 0) return 1;
        if (is_fixed()) { go_to(0, A.doc.page_off[(A.doc.npages - 1) * pm / 1000], 1); return 1; }
        const uint64_t at = (uint64_t)((double)pm / 1000 * A.book->total);
        int c = 0;
        while (c + 1 < A.book->nch && s_before[c + 1] <= at) c++;
        doc_t *d = doc_of(c);
        if (!d) return 1;
        const double in = A.book->ch[c].size ? (double)(at - s_before[c]) / A.book->ch[c].size : 0;
        go_to(c, (int32_t)(in * (double)d->n), 1);
        return 1;
    }
    default: return 0;
    }
}

void reader_key(const kev_t *k) {
    if (A.sheet == SH_TOC || A.sheet == SH_SEARCH) {   // lists scroll with the arrows
        if (k->key == K_DOWN || k->key == K_PGDN) { A.list_scroll += k->key == K_PGDN ? 400 : LIST_ROW; A.redraw |= R_ALL; return; }
        if (k->key == K_UP || k->key == K_PGUP) {
            A.list_scroll -= k->key == K_PGUP ? 400 : LIST_ROW;
            if (A.list_scroll < 0) A.list_scroll = 0;
            A.redraw |= R_ALL;
            return;
        }
        if (A.sheet == SH_SEARCH && k->key == K_ENTER) { reader_search_run(); return; }
        return;
    }
    if (A.sheet == SH_VIEWER) {
        if (k->key == K_CHAR && (k->ch == '+' || k->ch == '=')) { A.zoom100 = 200; A.redraw |= R_ALL; }
        if (k->key == K_CHAR && k->ch == '-') { A.zoom100 = 100; A.pan_x = A.pan_y = 0; A.redraw |= R_ALL; }
        if (k->key == K_LEFT) { A.pan_x += 80; A.redraw |= R_ALL; }
        if (k->key == K_RIGHT) { A.pan_x -= 80; A.redraw |= R_ALL; }
        if (k->key == K_UP) { A.pan_y += 80; A.redraw |= R_ALL; }
        if (k->key == K_DOWN) { A.pan_y -= 80; A.redraw |= R_ALL; }
        return;
    }
    const int ctrl = k->mods & (M_CTRL | M_GUI);
    switch (k->key) {
    case K_RIGHT: case K_DOWN: case K_PGDN: reader_turn(1, 1); return;
    case K_SPACE: reader_turn(k->mods & M_SHIFT ? -1 : 1, 1); return;
    case K_LEFT: case K_UP: case K_PGUP: case K_BACKSPACE: reader_turn(-1, 1); return;
    case K_HOME: reader_action(A_CHAP_PREV, 0); return;
    case K_END: reader_action(A_CHAP_NEXT, 0); return;
    case K_ENTER:
        if (A.focus_link >= 0) { follow(A.focus_link); return; }
        A.sheet = A.sheet == SH_CHROME ? SH_NONE : SH_CHROME;
        A.redraw |= R_ALL;
        return;
    case K_TAB: return;
    case K_CHAR: break;
    default: return;
    }
    const uint32_t c = k->ch >= 'A' && k->ch <= 'Z' ? k->ch + 32 : k->ch;
    if (ctrl && c == 'f') { reader_action(A_SEARCH, 0); return; }
    if (ctrl && (c == 'd' || c == 'b')) { bookmark_toggle(); A.redraw |= R_ALL; return; }
    switch (c) {
    case '/': reader_action(A_SEARCH, 0); break;
    case 't': case 'c': reader_action(A_TOC, 0); break;
    case 'b': bookmark_toggle(); break;
    case 'a': reader_action(A_LOOK, 0); break;
    case 'i': reader_action(A_INFO, 0); break;
    case 'm': A.sheet = A.sheet == SH_CHROME ? SH_NONE : SH_CHROME; break;
    case '+': case '=': reader_action(A_SIZE, 2); break;
    case '-': reader_action(A_SIZE, -2); break;
    case 'l': {   // the links of the page, one after the other (Enter follows)
        int ls[64];
        const int n = doc_links_on(&A.doc, A.page, ls, 64);
        if (!n) break;
        int i = 0;
        while (i < n && ls[i] != A.focus_link) i++;
        A.focus_link = ls[(i + 1) % n];
        A.cur_ok = 0;
        render_cur();
        break;
    }
    case 'n': reader_turn(1, 1); break;
    case 'p': reader_turn(-1, 1); break;
    default: return;
    }
    A.redraw |= R_ALL;
}

// ------------------------------------------------------------------------------- touch

static int page_hit(int x, int y, int *col, int *px, int *py) {   // screen -> a page's coordinates
    for (int k = A.cols - 1; k >= 0; k--)
        if (x >= A.col_x[k] - 20) {
            *col = k;
            *px = x - A.col_x[k];
            *py = y - A.text_y;
            return A.page + k < A.doc.npages;
        }
    return 0;
}

static void viewer_touch(int down, int x, int y) {
    const int fingers = nv_touch_count();
    if (down && !A.touch_down) {
        A.touch_down = 1;
        A.tx0 = A.tx = x;
        A.ty0 = A.ty = y;
        A.t0 = A.now;
        A.moved = 0;
        A.drag_scroll0 = A.pan_x;
        A.pinch = 0;
        return;
    }
    if (down) {
        if (fingers >= 2) {
            int x1, y1, x2, y2;
            nv_touch_at(0, &x1, &y1);
            nv_touch_at(1, &x2, &y2);
            const int d = abs(x1 - x2) + abs(y1 - y2);
            if (!A.pinch) { A.pinch = 1; A.pinch_d0 = d; A.pinch_size0 = A.zoom100; }
            else if (A.pinch_d0 > 0) {
                int z = A.pinch_size0 * d / A.pinch_d0;
                z = z < 100 ? 100 : z > 300 ? 300 : z;
                if (abs(z - A.zoom100) >= 25) { A.zoom100 = z; A.redraw |= R_ALL; }
            }
            A.moved = 1;
            return;
        }
        if (!A.moved && (abs(x - A.tx0) > 10 || abs(y - A.ty0) > 10)) A.moved = 1;
        if (A.moved && !A.pinch) {
            A.pan_x += x - A.tx;
            A.pan_y += y - A.ty;
            A.redraw |= R_ALL;
        }
        A.tx = x;
        A.ty = y;
        return;
    }
    A.touch_down = 0;
    if (A.moved) return;
    if (x > g_w - BTN - 24 && y < BTN + 24) { reader_action(A_CLOSE, 0); return; }
    if (A.now - A.last_tap_t < 350 && abs(x - A.last_tap_x) < 40) {   // double tap: zoom in / out there
        A.zoom100 = A.zoom100 > 100 ? 100 : 220;
        A.pan_x = (g_w / 2 - x) * (A.zoom100 > 100 ? 1 : 0);
        A.pan_y = (g_h / 2 - y) * (A.zoom100 > 100 ? 1 : 0);
        A.last_tap_t = 0;
        A.redraw |= R_ALL;
        return;
    }
    A.last_tap_t = A.now;
    A.last_tap_x = x;
}

void reader_touch(int down, int x, int y) {
    A.idle_since = A.now;
    if (A.sheet == SH_VIEWER) { viewer_touch(down, x, y); return; }
    if (down && !A.touch_down) {   // press
        A.touch_down = 1;
        A.tx0 = A.tx = x;
        A.ty0 = A.ty = y;
        A.t0 = A.now;
        A.moved = A.long_fired = A.drag_turn = A.pinch = 0;
        A.nvel = 0;
        return;
    }
    if (down) {   // move
        A.tx = x;
        A.ty = y;
        if (nv_touch_count() >= 2 && !is_fixed() && !A.drag_turn) {   // pinch: the text size
            int x1, y1, x2, y2;
            nv_touch_at(0, &x1, &y1);
            nv_touch_at(1, &x2, &y2);
            const int d = abs(x1 - x2) + abs(y1 - y2);
            if (!A.pinch) { A.pinch = 1; A.pinch_d0 = d; A.pinch_size0 = g_set.size; A.moved = 1; }
            else if (A.pinch_d0 > 40) {
                int s = A.pinch_size0 * d / A.pinch_d0;
                s = (s / 2) * 2;
                s = s < 14 ? 14 : s > 40 ? 40 : s;
                if (s != g_set.size) { g_set.size = s; set_changed(1); }
            }
            return;
        }
        if (A.pinch) return;
        const int dx = x - A.tx0, dy = y - A.ty0;
        if (!A.moved && (abs(dx) > 14 || abs(dy) > 14)) {
            A.moved = 1;
            if (abs(dx) > abs(dy)) {   // a horizontal drag turns the page under the finger
                A.drag_dir = dx < 0 ? 1 : -1;
                int dir = A.drag_dir;
                if (A.book->rtl && is_fixed()) dir = -dir;
                A.drag_turn = prepare(dir) ? 1 : -1;
                if (A.drag_turn < 0 && A.drag_dir > 0) toast(tr(S_BOOK_END), NULL);
            }
        }
        if (A.drag_turn > 0) {
            if (A.nvel == 6) { memmove(A.vx_t, A.vx_t + 1, 5 * sizeof(int)); memmove(A.vx_x, A.vx_x + 1, 5 * sizeof(int)); A.nvel--; }
            A.vx_t[A.nvel] = A.now;
            A.vx_x[A.nvel] = x;
            A.nvel++;
            if (animated()) {
                float prog = (float)(A.drag_dir > 0 ? -dx : dx) / (float)g_w;
                if (prog < 0) prog = 0;
                compose_turn(A.drag_dir, prog);
            }
        }
        if (!A.moved && !A.long_fired && A.now - A.t0 > 600) {   // a long press: a picture opens, a link tells where it goes
            A.long_fired = 1;
            int col, px, py;
            if (page_hit(x, y, &col, &px, &py)) {
                const int im = doc_image_at(&A.doc, A.page + col, px, py);
                const int l = doc_link_at(&A.doc, A.page + col, px, py);
                if (im >= 0 && !is_fixed()) { A.view_img = im; A.zoom100 = 100; A.pan_x = A.pan_y = 0; A.sheet = SH_VIEWER; A.redraw |= R_ALL; return; }
                if (l >= 0) {
                    char h[NVHTML_HREF_MAX];
                    snprintf(h, sizeof h, "%.*s", (int)A.doc.links[l].len, A.doc.text + A.doc.links[l].off);
                    toast("%s", h);
                    return;
                }
            }
            A.sheet = SH_CHROME;
            A.redraw |= R_ALL;
        }
        return;
    }
    // release
    x = A.tx;
    y = A.ty;
    A.touch_down = 0;
    if (A.pinch) { A.pinch = 0; return; }
    if (A.long_fired) return;
    if (A.drag_turn) {
        if (A.drag_turn < 0) { A.drag_turn = 0; return; }
        const int dx = x - A.tx0;
        float prog = (float)(A.drag_dir > 0 ? -dx : dx) / (float)g_w;
        if (prog < 0) prog = 0;
        float v = 0;   // px per ms, in the turning direction
        if (A.nvel >= 2) {
            const int dt = A.vx_t[A.nvel - 1] - A.vx_t[0];
            if (dt > 0) v = (float)(A.vx_x[A.nvel - 1] - A.vx_x[0]) / (float)dt * (A.drag_dir > 0 ? -1.f : 1.f);
        }
        A.drag_turn = 0;
        const int go = prog > 0.18f || v > 0.45f;
        int dir = A.drag_dir;
        if (go) {
            if (g_rmode != RM_COLOR) { reader_turn(A.book->rtl && is_fixed() ? -dir : dir, 0); return; }
            if (animated()) animate(dir, prog, 1);
            commit_turn(dir);
        } else if (animated()) {
            animate(dir, prog, 0);
        }
        A.redraw |= R_ALL;
        return;
    }
    if (A.moved) return;
    // a tap: a link or a picture of the page, else the side to turn, else the controls
    if (A.sheet == SH_CHROME) { A.sheet = SH_NONE; A.redraw |= R_ALL; return; }
    int col, px, py;
    if (page_hit(x, y, &col, &px, &py)) {
        const int l = doc_link_at(&A.doc, A.page + col, px, py);
        if (l >= 0) { follow(l); return; }
        const int im = is_fixed() ? -1 : doc_image_at(&A.doc, A.page + col, px, py);
        if (im >= 0 && A.doc.imgs[im].block) { A.view_img = im; A.zoom100 = 100; A.pan_x = A.pan_y = 0; A.sheet = SH_VIEWER; A.redraw |= R_ALL; return; }
    }
    if (is_fixed() && A.now - A.last_tap_t < 350 && abs(x - A.last_tap_x) < 40) {   // a comic: double tap to zoom
        const int k = x >= g_w / 2 && A.cols == 2 ? 1 : 0;
        for (int i = 0; i < A.doc.nimgs; i++) {
            const op_t *o = NULL;
            for (int j = 0; j < A.doc.nops; j++)
                if (A.doc.ops[j].kind == OP_IMAGE && A.doc.ops[j].off == i) { o = &A.doc.ops[j]; break; }
            if (o && doc_page_of_y(&A.doc, o->y0) == A.page + k) {
                A.view_img = i;
                A.zoom100 = 220;
                A.pan_x = A.pan_y = 0;
                A.sheet = SH_VIEWER;
                A.redraw |= R_ALL;
                break;
            }
        }
        A.last_tap_t = 0;
        return;
    }
    A.last_tap_t = A.now;
    A.last_tap_x = x;
    if (g_set.tap_zones && x < g_w * 28 / 100) { reader_turn(-1, 1); return; }
    if (g_set.tap_zones && x > g_w * 72 / 100) { reader_turn(1, 1); return; }
    A.sheet = SH_CHROME;
    A.redraw |= R_ALL;
}

// ------------------------------------------------------------------------- per frame

void reader_frame(void) {
    if (!A.book) return;
    if (A.touch_down) return;
    const int idle = A.now - A.idle_since;
    // pictures: one decoded per frame once the page is still
    if (img_pending() && idle > 60) {
        const int t0 = nv_millis();
        if (img_work(img_read, NULL)) {
            perf("picture: %s in %d ms", img_last(), nv_millis() - t0);
            if (!img_pending()) {
                if (s_cur_missing) { A.cur_ok = 0; render_cur(); A.redraw |= R_ALL; }
                if (s_nxt_missing) A.nxt_ok = 0;
            }
        }
        return;
    }
    // the next spread drawn ahead, in the direction of reading
    if (!A.nxt_ok && idle > 250 && A.sheet == SH_NONE) {
        int c, p;
        if (neighbour(s_last_dir, &c, &p) && (c == A.chap || c == A.side_chap || idle > 900)) prepare(s_last_dir);
        return;
    }
    if (s_save_due && idle > SAVE_AFTER_MS) reader_save();
}
