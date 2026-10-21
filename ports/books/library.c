// library.c — the library screen: "Continue reading" (the last book, its cover and progress), then
// every book as a cover in a grid with its title, author and progress, in the order chosen (recent,
// title, author, progress). Metadata and covers are read while the screen is still, one book per
// frame, the visible ones first; at most COVERS_KEPT covers stay in memory.
#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "nucleo_sdk.h"

#define MARGIN 28
#define CELL_W 132
#define CELL_H 192
#define ROW_H 278
#define HERO_H 224
#define COVERS_KEPT 24

static int s_dirty_meta;   // library.tsv has news to save
static int s_meta_saved_at;

static int cols(void) {
    const int c = (g_w - 2 * MARGIN + 24) / (CELL_W + 24);
    return c < 1 ? 1 : c;
}
static int pitch(void) { const int c = cols(); return c > 1 ? (g_w - 2 * MARGIN - CELL_W) / (c - 1) : 0; }
static int has_hero(void) { return A.norder > 0 && g_lib[A.order[0]].opened > 0 && g_set.sort == 0; }
static int grid_top(void) { return BAR_H + 16 + (has_hero() ? HERO_H + 44 : 0); }

int lib_height(void) {
    const int rows = (A.norder + cols() - 1) / cols();
    return grid_top() - BAR_H + rows * ROW_H + 24;
}

static int cmp_entries(const void *a, const void *b) {
    const entry_t *x = &g_lib[*(const int *)a], *y = &g_lib[*(const int *)b];
    switch (g_set.sort) {
    case 1: { const int c = strcasecmp(x->title, y->title); if (c) return c; break; }
    case 2: { const int c = strcasecmp(x->author[0] ? x->author : "\x7f", y->author[0] ? y->author : "\x7f"); if (c) return c; break; }
    case 3: if (x->permille != y->permille) return y->permille - x->permille; break;
    default:
        if (x->opened != y->opened) return x->opened > y->opened ? -1 : 1;
        if (x->mtime != y->mtime) return x->mtime > y->mtime ? -1 : 1;
        break;
    }
    return strcasecmp(x->title, y->title);
}

void lib_order(void) {
    free(A.order);
    A.order = (int *)malloc(sizeof(int) * (size_t)(g_nlib ? g_nlib : 1));
    A.norder = 0;
    if (!A.order) return;
    for (int i = 0; i < g_nlib; i++) A.order[A.norder++] = i;
    qsort(A.order, (size_t)A.norder, sizeof(int), cmp_entries);
}

static void progress_bar(int x, int y, int w, int permille) {
    gfx_rrect(x, y, w, 4, 2, g_col[C_RULE]);
    if (permille > 0) gfx_rrect(x, y, w * (permille > 1000 ? 1000 : permille) / 1000 < 4 ? 4 : w * permille / 1000, 4, 2, g_col[C_ACCENT]);
}

static void progress_label(const entry_t *e, char *out, size_t cap) {
    if (e->permille < 0) snprintf(out, cap, "%s", tr(S_NEW));
    else if (e->permille >= 995) snprintf(out, cap, "%s", tr(S_FINISHED));
    else snprintf(out, cap, tr(S_PERCENT_READ), e->permille / 10);
}

static void draw_empty(void) {
    const int cx = g_w / 2;
    icon_draw(IC_BOOK, cx - 24, 150, g_col[C_DIM]);
    const char *t = tr(S_EMPTY_TITLE);
    text_draw(F_BOLD, 24, cx - text_width(F_BOLD, 24, t, (int)strlen(t)) / 2, 240, t, (int)strlen(t), g_col[C_TEXT]);
    const char *a = tr(S_EMPTY_1), *b = tr(S_EMPTY_2);
    text_draw(F_SANS, 17, cx - text_width(F_SANS, 17, a, (int)strlen(a)) / 2, 280, a, (int)strlen(a), g_col[C_DIM]);
    text_draw(F_SANS, 17, cx - text_width(F_SANS, 17, b, (int)strlen(b)) / 2, 306, b, (int)strlen(b), g_col[C_DIM]);
    const char *l = tr(S_CATALOG);
    const int w = text_width(F_BOLD, 18, l, (int)strlen(l)) + 80;
    const int r = reg_add(cx - w / 2, 340, w, 52, A_CATALOG, 0);
    gfx_rrect(cx - w / 2, 340, w, 52, 26, reg_state(r) == 2 ? g_col[C_PRESS] : g_col[C_ACCENT]);
    if (reg_state(r) == 1) gfx_ring(cx - w / 2 - 3, 337, w + 6, 58, 29, 2, g_col[C_FOCUS]);
    icon_draw(IC_STORE, cx - w / 2 + 22, 354, g_col[C_ON_ACCENT]);
    text_draw(F_BOLD, 18, cx - w / 2 + 56, 372, l, (int)strlen(l), g_col[C_ON_ACCENT]);
}

static void draw_hero(int y) {
    const int i = A.order[0];
    const entry_t *e = &g_lib[i];
    const int x = MARGIN, w = g_w - 2 * MARGIN;
    const char *cap = tr(S_CONTINUE);
    text_draw(F_BOLD, 15, x, y + 14, cap, (int)strlen(cap), g_col[C_DIM]);
    y += 26;
    row_box(x, y, w, HERO_H, A_CONTINUE, i, g_col[C_CARD]);
    draw_cover(i, x + 18, y + 16, 132, HERO_H - 32);
    const int narrow = w < 700;   // portrait: the whole card opens the book, no button
    const int tx = x + 176, tw = w - 176 - (narrow ? 24 : 220);
    wrap_text(F_SERIF_BOLD, 26, tx, y + 52, tw, 32, e->title, 2, g_col[C_TEXT]);
    text_draw_fit(F_SANS, 18, tx, y + 122, e->author[0] ? e->author : tr(S_UNKNOWN_AUTHOR),
                  (int)strlen(e->author[0] ? e->author : tr(S_UNKNOWN_AUTHOR)), tw, g_col[C_DIM]);
    char pl[64];
    progress_label(e, pl, sizeof pl);
    text_draw(F_SANS, 15, tx, y + HERO_H - 46, pl, (int)strlen(pl), g_col[C_DIM]);
    progress_bar(tx, y + HERO_H - 34, tw, e->permille);
    if (narrow) return;
    // the big button
    const char *rd = tr(S_CAT_READ);
    const int bw = text_width(F_BOLD, 18, rd, (int)strlen(rd)) + 56, bx = x + w - bw - 24, by = y + HERO_H / 2 - 24;
    gfx_rrect(bx, by, bw, 48, 24, g_col[C_ACCENT]);
    text_draw(F_BOLD, 18, bx + 28, by + 30, rd, (int)strlen(rd), g_col[C_ON_ACCENT]);
}

static void lib_bar(void) {   // the title, the catalog, sorting and the orientation: always there
    bar_bg();
    const char *t = tr(S_LIBRARY);
    text_draw(F_BOLD, 26, MARGIN, 42, t, (int)strlen(t), g_col[C_BAR_TEXT]);
    if (g_nlib) {
        char n[32];
        snprintf(n, sizeof n, "%d", g_nlib);
        text_draw(F_SANS, 16, MARGIN + text_width(F_BOLD, 26, t, (int)strlen(t)) + 12, 41, n, (int)strlen(n), g_col[C_DIM]);
    }
    const char *cl = tr(S_CATALOG);
    const int cw = text_width(F_BOLD, 16, cl, (int)strlen(cl)) + 64, cx = g_w - MARGIN - cw;
    const int r = reg_add(cx, 12, cw, 40, A_CATALOG, 0);
    gfx_rrect(cx, 12, cw, 40, 20, reg_state(r) == 2 ? g_col[C_PRESS] : g_col[C_ACCENT]);
    if (reg_state(r) == 1) gfx_ring(cx - 3, 9, cw + 6, 46, 23, 2, g_col[C_FOCUS]);
    icon_draw(IC_STORE, cx + 16, 20, g_col[C_ON_ACCENT]);
    text_draw(F_BOLD, 16, cx + 48, 38, cl, (int)strlen(cl), g_col[C_ON_ACCENT]);
    button(cx - BTN - 8, 6, IC_SORT, A_SORT, 0, g_nlib > 1, g_col[C_BAR_TEXT]);
    button(cx - 2 * BTN - 12, 6, IC_ROTATE, A_ORIENT, 0, 1, g_col[C_BAR_TEXT]);
    button(cx - 3 * BTN - 16, 6, IC_INFO, A_ABOUT, 0, 1, g_col[C_BAR_TEXT]);
}

extern const char bk_notices[];   // NOTICES.txt (gen_text.py)

// About the app: its version and the third-party notices, scrolled with a finger or the arrows.
void about_draw(void) {
    scrim();
    const int w = g_w - 32 < 760 ? g_w - 32 : 760, h = g_h - 48, x = (g_w - w) / 2, y = 24;
    sheet_frame(x, y, w, h);
    button(x + w - BTN - 8, y + 8, IC_CLOSE, A_CLOSE, 0, 1, g_col[C_DIM]);
    char t[96];
    snprintf(t, sizeof t, "%s  \xC2\xB7  %s", tr(S_ABOUT), BK_VERSION);
    text_draw(F_BOLD, 20, x + 24, y + 40, t, (int)strlen(t), g_col[C_TEXT]);
    text_draw(F_BOLD, 14, x + 24, y + 70, tr(S_NOTICES), (int)strlen(tr(S_NOTICES)), g_col[C_DIM]);
    const int top = y + 84, bottom = y + h - 12, lh = 20, tw = w - 48;
    gfx_clip(x, top, x + w, bottom);
    int ly = top + 18 - A.list_scroll;
    for (const char *p = bk_notices; *p;) {   // a paragraph per line of the file, wrapped
        const char *e = strchr(p, '\n');
        if (!e) e = p + strlen(p);
        const char *s = p;
        int n = (int)(e - p);
        if (!n) ly += lh / 2;
        while (n > 0) {
            int k = text_fit(F_SANS, 14, s, n, tw);
            if (k < n) { int b = k; while (b > 0 && s[b] != ' ') b--; if (b > 0) k = b; }
            if (k <= 0) k = n;
            if (ly > top - lh && ly < bottom + lh) text_draw(F_SANS, 14, x + 24, ly, s, k, g_col[C_TEXT]);
            ly += lh;
            s += k;
            n -= k;
            while (n > 0 && *s == ' ') { s++; n--; }
        }
        p = *e ? e + 1 : e;
    }
    gfx_noclip();
}

void lib_draw(void) {
    gfx_fill(0, 0, g_w, g_h, g_col[C_BG]);
    if (!g_nlib) {
        draw_empty();
        lib_bar();
        return;
    }
    const int top = BAR_H;
    gfx_clip(0, top, g_w, g_h);
    int y = top + 16 - A.lib_scroll;
    if (has_hero()) {
        if (y + HERO_H + 40 > top) draw_hero(y);
        y += HERO_H + 44;
    }
    const int c = cols(), p = pitch();
    for (int k = 0; k < A.norder; k++) {
        const int row = k / c, col = k % c;
        const int x = MARGIN + col * p, cy = y + row * ROW_H;
        if (cy + ROW_H < top) continue;
        if (cy > g_h) break;
        const int i = A.order[k];
        const entry_t *e = &g_lib[i];
        const int r = reg_add(x - 8, cy - 8, CELL_W + 16, ROW_H - 8, A_OPEN, i);
        const int st = reg_state(r);
        if (st == 2) gfx_rrect(x - 8, cy - 8, CELL_W + 16, ROW_H - 12, 14, g_col[C_PRESS]);
        if (st == 1) gfx_ring(x - 8, cy - 8, CELL_W + 16, ROW_H - 12, 14, 2, g_col[C_FOCUS]);
        draw_cover(i, x, cy, CELL_W, CELL_H);
        if (e->bad) gfx_blend_rect(x, cy, CELL_W, CELL_H, g_col[C_BG], 150);
        wrap_text(F_BOLD, 15, x, cy + CELL_H + 22, CELL_W, 19, e->title, 2, g_col[C_TEXT]);
        text_draw_fit(F_SANS, 13, x, cy + CELL_H + 60, e->author, (int)strlen(e->author), CELL_W, g_col[C_DIM]);
        if (e->permille >= 0) progress_bar(x, cy + CELL_H + 70, CELL_W, e->permille);
    }
    gfx_noclip();
    lib_bar();
    // the scroll position, thin, on the right
    const int h = lib_height(), view = g_h - BAR_H;
    if (h > view) {
        const int th = view * view / h < 40 ? 40 : view * view / h;
        const int ty = BAR_H + (int)((long)(view - th) * A.lib_scroll / (h - view));
        gfx_rrect(g_w - 6, ty, 4, th, 2, g_col[C_RULE]);
    }
}

// ------------------------------------------------------------------ the book's sheet, sorting

static void info_row(int x, int *y, int w, const char *k, const char *v) {
    if (!v || !*v) return;
    text_draw(F_SANS, 14, x, *y, k, (int)strlen(k), g_col[C_DIM]);
    text_draw_fit(F_SANS, 16, x + 130, *y, v, (int)strlen(v), w - 130, g_col[C_TEXT]);
    *y += 28;
}

void book_sheet_draw(void) {
    if (A.sheet == SH_SORT) {
        const int w = 260, x = g_w - MARGIN - w - 120 > 8 ? g_w - MARGIN - w - 120 : 8, y = BAR_H - 4;
        sheet_frame(x, y, w, 4 * 52 + 16);
        static const int k[4] = {S_SORT_RECENT, S_SORT_TITLE, S_SORT_AUTHOR, S_SORT_PROGRESS};
        for (int i = 0; i < 4; i++) {
            const int ry = y + 8 + i * 52;
            row_box(x + 8, ry, w - 16, 48, A_SORT_SET, i, i == g_set.sort ? g_col[C_CARD] : g_col[C_BG]);
            text_draw(i == g_set.sort ? F_BOLD : F_SANS, 17, x + 24, ry + 31, tr(k[i]), (int)strlen(tr(k[i])), g_col[C_TEXT]);
            if (i == g_set.sort) icon_draw(IC_CHECK, x + w - 48, ry + 12, g_col[C_ACCENT]);
        }
        return;
    }
    const int i = A.book_sel;
    if (i < 0 || i >= g_nlib) { A.sheet = SH_NONE; return; }
    const entry_t *e = &g_lib[i];
    scrim();
    const int w = g_w - 32 < 620 ? g_w - 32 : 620, h = 420, x = (g_w - w) / 2, y = (g_h - h) / 2;
    sheet_frame(x, y, w, h);
    button(x + w - BTN - 8, y + 8, IC_CLOSE, A_CLOSE, 0, 1, g_col[C_DIM]);
    draw_cover(i, x + 28, y + 28, CELL_W, CELL_H);
    const int tx = x + 28 + CELL_W + 28, tw = w - (tx - x) - 60;
    wrap_text(F_SERIF_BOLD, 22, tx, y + 56, tw, 28, e->title, 3, g_col[C_TEXT]);
    int ry = y + 56 + 28 * 3 + 6;
    char sz[32], pl[64];
    fmt_size((uint64_t)e->size, sz, sizeof sz);
    progress_label(e, pl, sizeof pl);
    info_row(tx, &ry, tw + 40, tr(S_F_AUTHOR), e->author);
    info_row(tx, &ry, tw + 40, tr(S_F_LANG), lang_name(e->lang));
    info_row(tx, &ry, tw + 40, tr(S_F_FORMAT), book_kind_name(e->kind));
    info_row(tx, &ry, tw + 40, tr(S_F_SIZE), sz);
    info_row(tx, &ry, tw + 40, tr(S_SORT_PROGRESS), pl);
    info_row(tx, &ry, tw + 40, tr(S_F_FILE), e->file);
    // buttons
    const char *rd = tr(S_CAT_READ), *dl = A.del_armed ? tr(S_DELETE_SURE) : tr(S_DELETE);
    const int bw = text_width(F_BOLD, 17, rd, (int)strlen(rd)) + 56;
    int r = reg_add(x + w - 28 - bw, y + h - 72, bw, 48, A_OPEN, i);
    gfx_rrect(x + w - 28 - bw, y + h - 72, bw, 48, 24, reg_state(r) == 2 ? g_col[C_PRESS] : g_col[C_ACCENT]);
    if (reg_state(r) == 1) gfx_ring(x + w - 31 - bw, y + h - 75, bw + 6, 54, 27, 2, g_col[C_FOCUS]);
    text_draw(F_BOLD, 17, x + w - 28 - bw + 28, y + h - 42, rd, (int)strlen(rd), g_col[C_ON_ACCENT]);
    const int dw = text_width(F_SANS, 17, dl, (int)strlen(dl)) + 70;
    r = reg_add(x + 28, y + h - 72, dw, 48, A_DELETE, i);
    gfx_rrect(x + 28, y + h - 72, dw, 48, 24, reg_state(r) == 2 ? g_col[C_PRESS] : A.del_armed ? g_col[C_MARK] : g_col[C_CARD]);
    if (reg_state(r) == 1) gfx_ring(x + 25, y + h - 75, dw + 6, 54, 27, 2, g_col[C_FOCUS]);
    icon_draw(IC_TRASH, x + 44, y + h - 60, A.del_armed ? g_col[C_ON_ACCENT] : g_col[C_TEXT]);
    text_draw(F_SANS, 17, x + 78, y + h - 42, dl, (int)strlen(dl), A.del_armed ? g_col[C_ON_ACCENT] : g_col[C_TEXT]);
}

void library_rescan(void) {
    for (int i = 0; i < LIB_MAX; i++) { free(A.covers[i]); A.covers[i] = NULL; A.cover_tried[i] = 0; }
    lib_scan(NULL, NULL);
    lib_order();
}

// ------------------------------------------------------------------------------- idle work

static int ncovers(void) {
    int n = 0;
    for (int i = 0; i < g_nlib; i++) n += A.covers[i] != NULL;
    return n;
}

static void visible_range(int *k0, int *k1) {
    const int c = cols();
    const int y0 = A.lib_scroll - (grid_top() - BAR_H);
    int r0 = y0 / ROW_H - 1, r1 = (y0 + g_h) / ROW_H + 1;
    if (r0 < 0) r0 = 0;
    *k0 = r0 * c;
    *k1 = (r1 + 1) * c;
    if (*k1 > A.norder) *k1 = A.norder;
}

// One book per call: its cover (cached, else from the book) when it is on screen, else the
// metadata of a book never read. Returns 1 when something changed on screen.
static int idle_step(void) {
    int k0, k1;
    visible_range(&k0, &k1);
    int target = -1, need_open = 0;
    if (has_hero() && !A.cover_tried[A.order[0]]) target = A.order[0];
    for (int k = k0; k < k1 && target < 0; k++) {
        const int i = A.order[k];
        if (!A.cover_tried[i]) target = i;
    }
    if (target >= 0) {
        A.cover_tried[target] = 1;
        if (ncovers() >= COVERS_KEPT) {   // make room: the cover farthest from the screen
            int far = -1, fd = -1;
            for (int k = 0; k < A.norder; k++) {
                const int i = A.order[k];
                if (!A.covers[i]) continue;
                const int d = k < k0 ? k0 - k : k >= k1 ? k - k1 + 1 : 0;
                if (d > fd) { fd = d; far = i; }
            }
            if (far >= 0 && fd > 0) { free(A.covers[far]); A.covers[far] = NULL; A.cover_tried[far] = 0; }
        }
        A.covers[target] = lib_cover(target, NULL);
        need_open = !A.covers[target] || g_lib[target].nch < 0;
        if (!need_open) return 1;
    } else {
        for (int i = 0; i < g_nlib && target < 0; i++) if (g_lib[i].nch < 0) target = i;
        if (target < 0) return 0;
        need_open = 1;
    }
    char p[600];
    snprintf(p, sizeof p, "%s/%s", books_dir(), g_lib[target].file);
    book_t *b = NULL;
    const int t0 = nv_millis();
    const int r = book_open_meta(p, &b);
    lib_update(target, r == BKE_OK ? b : NULL);
    if (b && A.cover_tried[target] && !A.covers[target]) A.covers[target] = lib_cover(target, b);
    book_close(b);
    perf("library: '%s' read in %d ms%s", g_lib[target].file, nv_millis() - t0, r ? " (failed)" : "");
    s_dirty_meta = 1;
    return 1;
}

void lib_idle(void) {
    if (A.screen != SCR_LIBRARY || !g_nlib || A.now - A.idle_since < 120) return;
    if (idle_step()) A.redraw |= R_ALL;
    if (s_dirty_meta && A.now - s_meta_saved_at > 3000) {
        lib_save();
        s_dirty_meta = 0;
        s_meta_saved_at = A.now;
        if (g_set.sort) lib_order();   // titles and authors are known now
    }
}
