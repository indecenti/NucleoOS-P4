// doc.c — the reader's layout: the rich text of nvhtml (nvhtml.h) becomes a list of positioned
// operations (text runs and rectangles) sorted by their top, so drawing any band of the page only
// visits the operations in it. Paragraphs wrap at spaces (a word too long for the column breaks
// anywhere), list items hang their bullets, preformatted lines wrap anywhere on a tinted band,
// tables get their columns from the widths of their words, headings feed the table of contents.
#include "bib.h"

#include <stdlib.h>
#include <string.h>

#include "nvhtml.h"

// OP_STATIC: text from kStatic (off = index): bullets. OP_IMAGE: a picture (off = index in d->imgs,
// y0..y1 its own box, not the line's).
static const char *const kStatic[] = {"\xE2\x80\xA2", "\xE2\x97\xA6", "\xE2\x96\xAA"};   // • ◦ ▪

// ------------------------------------------------------------------------------------ the output

static op_t *push_op(doc_t *d) {
    if (d->nops == d->cap) {
        int cap = d->cap ? d->cap * 2 : 1024;
        op_t *o = (op_t *)realloc(d->ops, sizeof(op_t) * cap);
        if (!o) return NULL;
        d->ops = o;
        d->cap = cap;
    }
    op_t *o = &d->ops[d->nops++];
    memset(o, 0, sizeof *o);
    o->link = -1;
    return o;
}

// A rectangle; h may be 0 for one whose height is set later (returns its index, or -1).
static int add_rect_h(doc_t *d, int x, int32_t y, int w, int32_t h, int color) {
    if (w <= 0 || h < 0) return -1;
    op_t *o = push_op(d);
    if (!o) return -1;
    o->kind = OP_RECT;
    o->x = (int16_t)x;
    o->w = (int16_t)w;
    o->y0 = y;
    o->y1 = y + h;
    o->color = (uint8_t)color;
    return d->nops - 1;
}

static void add_rect(doc_t *d, int x, int32_t y, int w, int32_t h, int color) {
    if (w <= 0 || h <= 0) return;
    op_t *o = push_op(d);
    if (!o) return;
    o->kind = OP_RECT;
    o->x = (int16_t)x;
    o->w = (int16_t)w;
    o->y0 = y;
    o->y1 = y + h;
    o->color = (uint8_t)color;
}

static int add_img(doc_t *d, const dimg_t *im) {
    if (d->nimgs == d->icap) {
        int cap = d->icap ? d->icap * 2 : 64;
        dimg_t *n = (dimg_t *)realloc(d->imgs, sizeof(dimg_t) * cap);
        if (!n) return -1;
        d->imgs = n;
        d->icap = cap;
    }
    d->imgs[d->nimgs] = *im;
    return d->nimgs++;
}

static int add_link(doc_t *d, int32_t off, int len) {
    if (d->nlinks == d->lcap) {
        int cap = d->lcap ? d->lcap * 2 : 256;
        link_t *l = (link_t *)realloc(d->links, sizeof(link_t) * cap);
        if (!l) return -1;
        d->links = l;
        d->lcap = cap;
    }
    d->links[d->nlinks].off = off;
    d->links[d->nlinks].len = (uint16_t)(len > 65535 ? 65535 : len);
    return d->nlinks++;
}

// ---------------------------------------------------------------------------------- line builder

typedef struct {
    int32_t off;
    int len;
    uint8_t font, px, color, code;
    int8_t raise;            // baseline shift (sup < 0, sub > 0)
    int16_t link;
    int w;                   // measured, 1/16 px
    int image;               // >= 0: a picture (index in d->imgs), else text
    int ih, below;           // a picture: height, and how far it goes below the baseline
} frag_t;

#define LINE_OPS 192
#define WORD_FRAGS 32

typedef struct {
    doc_t *d;
    int body;
    int x0, x1;              // the column
    int indent;              // continuation lines of the current block start here (1/16 px from x0)
    int32_t y;               // top of the line being built
    op_t line[LINE_OPS];
    int32_t sx16[LINE_OPS];  // where each run of the line starts, 1/16 px
    int nline;
    int lx;                  // pen, 1/16 px from x0 (positions follow the glyphs as drawn)
    int asc, desc;           // tallest font in the line
    int min_lh;              // minimum line height of the block
    int space_w;             // a space is due before the next word (its width, 1/16 px), 0 = none
    frag_t word[WORD_FRAGS];
    int nword, word_w;
    int wrap_any;            // preformatted text: break anywhere, never at spaces only
    int code_bg;             // the block paints code backgrounds
    int32_t bottom;          // lowest pixel used
    int img_above, img_below;   // the pictures of the line: room they need above and below the baseline
} lay_t;

static void line_flush(lay_t *L) {
    if (!L->nline && !L->asc) return;
    int asc = L->asc ? L->asc : font_ascent(F_SANS, L->body);
    int desc = L->desc ? L->desc : font_descent(F_SANS, L->body);
    int lh = (asc + desc) * 135 / 100;
    if (lh < L->min_lh) lh = L->min_lh;
    int above = (lh - asc - desc) / 2 + asc;
    if (L->img_above || L->img_below) {   // pictures: the room the tallest needs, a little air around
        int below = lh - above;
        if (L->img_above + 4 > above) above = L->img_above + 4;
        if (L->img_below + 4 > below) below = L->img_below + 4;
        lh = above + below;
    }
    const int32_t base = L->y + above;
    for (int i = 0; i < L->nline; i++) {   // backgrounds first: the text is drawn over them
        op_t *o = &L->line[i];
        if (o->kind == OP_TEXT && o->color == C_CODE_BG) {   // marked: code in running text
            const int r = add_rect_h(L->d, L->x0 + o->x - 3, L->y, o->w + 6, lh, C_CODE_BG);
            if (r >= 0) L->d->ops[r].base = 2;   // a rectangle's base: its inset, top and bottom
            o->color = o->link >= 0 ? C_LINK : C_TEXT;
        }
    }
    for (int i = 0; i < L->nline; i++) {
        op_t *o = push_op(L->d);
        if (!o) break;
        *o = L->line[i];
        o->x = (int16_t)(o->x + L->x0);
        if (o->kind == OP_IMAGE) {   // len held the height, base how far below the baseline
            o->y1 = base + o->base;
            o->y0 = o->y1 - o->len;
            o->base = base;
            continue;
        }
        o->y0 = L->y;
        o->y1 = L->y + lh;
        o->base += base;   // base held the raise
    }
    L->nline = 0;
    L->y += lh;
    if (L->y > L->bottom) L->bottom = L->y;
    L->lx = L->indent;
    L->asc = L->desc = 0;
    L->img_above = L->img_below = 0;
    L->space_w = 0;
}

static void place_image(lay_t *L, const frag_t *f, int space) {
    L->lx += space;
    if (L->nline == LINE_OPS) line_flush(L);
    L->sx16[L->nline] = L->lx;
    op_t *o = &L->line[L->nline++];
    memset(o, 0, sizeof *o);
    o->kind = OP_IMAGE;
    o->x = (int16_t)((L->lx + 8) >> 4);
    o->w = (int16_t)((f->w + 8) >> 4);
    o->off = f->image;
    o->len = (uint16_t)f->ih;
    o->base = f->below;
    o->link = f->link;
    L->lx += f->w;
    if (f->ih - f->below > L->img_above) L->img_above = f->ih - f->below;
    if (f->below > L->img_below) L->img_below = f->below;
}

// Places a fragment (or the first `len` bytes of it) at the pen; merges with the previous run when
// the style matches and the source is contiguous (a single space between them).
static void place(lay_t *L, const frag_t *f, int len, int w, int space) {
    const doc_t *d = L->d;
    if (L->nline) {
        op_t *p = &L->line[L->nline - 1];
        const int32_t end = p->off + p->len;
        const int gap = space ? 1 : 0;
        const int color = f->code && L->code_bg ? C_CODE_BG : f->color;
        if (p->kind == OP_TEXT && p->font == f->font && p->px == f->px && p->link == f->link && p->color == color &&
            p->base == f->raise &&
            end + gap == f->off && (!gap || d->text[end] == ' ') && p->len + gap + len < 65000) {
            p->len = (uint16_t)(p->len + gap + len);
            L->lx += space + w;
            p->w = (int16_t)((L->lx - L->sx16[L->nline - 1] + 8) >> 4);
            goto metrics;
        }
    }
    L->lx += space;
    if (L->nline == LINE_OPS) line_flush(L);
    {
        L->sx16[L->nline] = L->lx;
        op_t *o = &L->line[L->nline++];
        memset(o, 0, sizeof *o);
        o->kind = OP_TEXT;
        o->x = (int16_t)((L->lx + 8) >> 4);
        o->w = (int16_t)((w + 8) >> 4);
        o->font = f->font;
        o->px = f->px;
        o->color = f->code && L->code_bg ? C_CODE_BG : f->color;   // C_CODE_BG: paint a background
        o->off = f->off;
        o->len = (uint16_t)len;
        o->link = f->link;
        o->base = f->raise;
        L->lx += w;
    }
metrics:;
    const int a = font_ascent(f->font, f->px) - f->raise, de = font_descent(f->font, f->px) + f->raise;
    if (a > L->asc) L->asc = a;
    if (de > L->desc) L->desc = de;
}

static void word_commit(lay_t *L) {
    if (!L->nword) return;
    const int col = (L->x1 - L->x0) << 4;
    int space = L->space_w;
    if (L->lx > L->indent && L->lx + space + L->word_w > col) {   // does not fit: next line
        line_flush(L);
        space = 0;
    }
    if (L->lx == L->indent) space = 0;
    if (L->lx + space + L->word_w <= col) {
        for (int i = 0; i < L->nword; i++) {
            if (L->word[i].image >= 0) place_image(L, &L->word[i], i == 0 ? space : 0);
            else place(L, &L->word[i], L->word[i].len, L->word[i].w, i == 0 ? space : 0);
        }
    } else {   // longer than the column: break it anywhere
        for (int i = 0; i < L->nword; i++) {
            frag_t f = L->word[i];
            int sp = i == 0 ? space : 0;
            if (f.image >= 0) {   // a picture never breaks: a line of its own if need be
                if (L->lx > L->indent && L->lx + sp + f.w > col) { line_flush(L); sp = 0; }
                place_image(L, &f, sp);
                continue;
            }
            while (f.len > 0) {
                const int room = col - L->lx - sp;
                int k = text_fit(f.font, f.px, L->d->text + f.off, f.len, room > 0 ? room >> 4 : 0);
                if (k <= 0) {
                    if (L->lx > L->indent) { line_flush(L); sp = 0; continue; }
                    const char *p = L->d->text + f.off;
                    utf8_next(&p, L->d->text + f.off + f.len);   // at least one character
                    k = (int)(p - (L->d->text + f.off));
                }
                const int w = text_width16(f.font, f.px, L->d->text + f.off, k);
                place(L, &f, k, w, sp);
                sp = 0;
                f.off += k;
                f.len -= k;
                if (f.len > 0) line_flush(L);
            }
        }
    }
    L->nword = 0;
    L->word_w = 0;
    L->space_w = 0;
}

static void word_add(lay_t *L, const frag_t *f) {
    if (L->nword == WORD_FRAGS) {   // a "word" of many styles: let it break here
        const int sw = L->space_w;
        word_commit(L);
        L->space_w = sw;
    }
    L->word[L->nword++] = *f;
    L->word_w += f->w;
}

// ------------------------------------------------------------------------------- inline parsing

typedef struct {
    int bold, italic, code, sup, sub, link;
    int head;                // heading: bold, no links
    int px;                  // the block's text size
    int color;
} ist_t;

static void frag_style(const ist_t *s, frag_t *f, int body) {
    (void)body;
    int px = s->px;
    if (s->code) { f->font = F_MONO; px = px * 92 / 100; }
    else if ((s->bold || s->head) && s->italic) f->font = F_BOLDITALIC;
    else if (s->bold || s->head) f->font = F_BOLD;
    else if (s->italic) f->font = F_ITALIC;
    else f->font = F_SANS;
    f->raise = 0;
    if (s->sup || s->sub) {
        f->raise = (int8_t)(s->sup ? -(s->px * 36 / 100) : s->px * 18 / 100);
        px = px * 72 / 100;
    }
    f->px = (uint8_t)(px < 6 ? 6 : px > 200 ? 200 : px);
    f->link = (int16_t)(s->head ? -1 : s->link);
    f->color = (uint8_t)(f->link >= 0 ? C_LINK : s->color);
    f->code = (uint8_t)s->code;
}

// "20.66ex" / "50px" / "" -> pixels on this screen (0 when unknown).
static int length_px(const char *s, int n, int body) {
    char b[24];
    if (n <= 0 || n >= (int)sizeof b) return 0;
    memcpy(b, s, (size_t)n);
    b[n] = 0;
    char *end = NULL;
    const double v = strtod(b, &end);
    if (!end || end == b) return 0;
    if (!strcmp(end, "ex")) return (int)(v * body * 0.536 + 0.5);   // Noto Sans' x-height: formulas fit the text
    if (!strcmp(end, "px")) return (int)(v * body / 16.0 + 0.5);   // CSS pixels for a 16 px body
    return 0;
}

// A picture of the page, as the next fragment of the word (inline) or on a line of its own (block).
// Returns its width (1/16 px) for the table measure, which places nothing.
static int image_frag(lay_t *L, const ist_t *st, const int32_t *fs, const int *fl, int measure) {
    doc_t *d = L->d;
    const char *t = d->text;
    const int body = L->body, col = L->x1 - L->x0;
    int w = length_px(t + fs[1], fl[1], body), h = length_px(t + fs[2], fl[2], body);
    const int va = length_px(t + fs[3], fl[3], body);
    int mask = 0, block = 0, figure = 0;
    for (int k = 0; k < fl[4]; k++) {
        mask |= t[fs[4] + k] == 'm';
        block |= t[fs[4] + k] == 'd';
        figure |= t[fs[4] + k] == 'f';
    }
    if (w <= 0 && h <= 0) { w = body * 8; h = body * 6; }   // no size given: a 4:3 box
    else if (w <= 0) w = h;
    else if (h <= 0) h = w;
    if (w > col && col > 0) { h = (int)((long)h * col / w); w = col; }
    if (h > body * 30) { w = (int)((long)w * body * 30 / h); h = body * 30; }   // taller than a screen: shrunk
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (!mask && (figure || w > col * 6 / 10)) block = 1;
    if (measure) return w << 4;
    dimg_t im = {fs[0], fs[5], (uint16_t)fl[0], (uint16_t)fl[5], (int16_t)w, (int16_t)h, (uint8_t)mask, (uint8_t)block};
    frag_t f;
    memset(&f, 0, sizeof f);
    f.image = add_img(d, &im);
    if (f.image < 0) return 0;
    f.w = w << 4;
    f.ih = h;
    f.below = mask ? -va : 0;   // a formula's vertical-align: negative goes under the baseline
    f.link = (int16_t)(st->head ? -1 : st->link);
    if (!block) {
        word_add(L, &f);
        return f.w;
    }
    // on its own line, in the middle of the column
    const int sw = L->space_w;
    word_commit(L);
    (void)sw;
    if (L->nline || L->lx > L->indent) line_flush(L);
    L->lx = ((col << 4) - f.w) / 2;
    if (L->lx < 0) L->lx = 0;
    place_image(L, &f, 0);
    line_flush(L);
    L->space_w = 0;
    return f.w;
}

// Feeds the text of [off, off + len) into the line builder. measure: only collect word widths.
typedef struct { int minw, maxw, cur; } meas_t;

static void flow(lay_t *L, int32_t off, int len, ist_t *st, meas_t *m) {
    doc_t *d = L->d;
    const char *t = d->text;
    int32_t i = off;
    const int32_t end = off + len;
    int pending_space = 0;
    int word_w = 0, pend_sw = 0;   // measure mode
    while (i < end) {
        const unsigned char c = (unsigned char)t[i];
        if (c < 0x20) {   // a marker
            i++;
            switch (c) {
            case NVHTML_BOLD_ON: st->bold = 1; break;
            case NVHTML_BOLD_OFF: st->bold = 0; break;
            case NVHTML_ITALIC_ON: st->italic = 1; break;
            case NVHTML_ITALIC_OFF: st->italic = 0; break;
            case NVHTML_CODE_ON: st->code = 1; break;
            case NVHTML_CODE_OFF: st->code = 0; break;
            case NVHTML_SUP_ON: st->sup = 1; break;
            case NVHTML_SUP_OFF: st->sup = 0; break;
            case NVHTML_SUB_ON: st->sub = 1; break;
            case NVHTML_SUB_OFF: st->sub = 0; break;
            case NVHTML_LINK: {
                const int32_t h0 = i;
                while (i < end && t[i] != NVHTML_LINK_TEXT) i++;
                st->link = m ? -1 : add_link(d, h0, (int)(i - h0));
                if (i < end) i++;
                break;
            }
            case NVHTML_LINK_END: st->link = -1; break;
            case NVHTML_ANCHOR: i = end; break;   // the rest of a heading line
            case NVHTML_IMAGE: {
                int32_t fs[6] = {0};
                int fl[6] = {0}, nf = 0;
                fs[0] = i;
                while (i < end && t[i] != NVHTML_IMAGE_END) {
                    if (t[i] == NVHTML_FIELD && nf < 5) {
                        fl[nf] = (int)(i - fs[nf]);
                        fs[++nf] = i + 1;
                    }
                    i++;
                }
                if (nf == 5) fl[5] = (int)(i - fs[5]);
                if (i < end) i++;
                if (nf == 5) {
                    if (m) word_w += image_frag(L, st, fs, fl, 1);
                    else image_frag(L, st, fs, fl, 0);
                    pending_space = 0;
                }
                break;
            }
            default: break;                        // line markers, cells: handled by the caller
            }
            continue;
        }
        if (c == ' ' && !L->wrap_any) {
            frag_t f;
            frag_style(st, &f, L->body);
            const int sw = text_width16(f.font, f.px, " ", 1);
            if (m) {
                if (word_w) {
                    if (word_w > m->minw) m->minw = word_w;
                    m->cur += (m->cur ? pend_sw : 0) + word_w;
                    word_w = 0;
                }
                pend_sw = sw;
            } else {
                word_commit(L);
                L->space_w = sw;
            }
            pending_space = 1;
            while (i < end && t[i] == ' ') i++;
            continue;
        }
        // a run of visible bytes up to a space or a marker
        int32_t j = i;
        while (j < end && (t[j] != ' ' || L->wrap_any) && (unsigned char)t[j] >= 0x20) j++;
        frag_t f;
        frag_style(st, &f, L->body);
        f.image = -1;
        f.off = i;
        f.len = (int)(j - i);
        f.w = text_width16(f.font, f.px, t + i, f.len);
        if (m) word_w += f.w;
        else word_add(L, &f);
        pending_space = 0;
        i = j;
    }
    (void)pending_space;
    if (m) {
        if (word_w > m->minw) m->minw = word_w;
        if (word_w) m->cur += (m->cur ? pend_sw : 0) + word_w;
        if (m->cur > m->maxw) m->maxw = m->cur;
    } else {
        word_commit(L);
    }
}

// --------------------------------------------------------------------------------------- blocks

typedef struct {
    int32_t off;
    int len;
    char kind;               // 'p' text, 'h' heading, 'i' item, 'd' definition, 'r' row, 'x' pre, 0 blank
    int level;               // heading level, item depth
    int32_t body_off;        // the text after the line's own prefix
    int body_len;
    int32_t mark_off;        // item: its "N." mark
    int mark_len;
    int numbered;
} line_t;

static void classify(const doc_t *d, line_t *l) {
    const char *t = d->text + l->off;
    int n = l->len;
    l->kind = 'p';
    l->level = 0;
    l->body_off = l->off;
    l->body_len = n;
    l->mark_len = 0;
    l->numbered = 0;
    int vis = 0;
    for (int i = 0; i < n && !vis; i++) vis = (unsigned char)t[i] >= 0x21;
    if (!vis) { l->kind = 0; return; }
    if (t[0] == NVHTML_ROW) { l->kind = 'r'; l->body_off++; l->body_len--; return; }
    if (t[0] == NVHTML_PRE) { l->kind = 'x'; l->body_off++; l->body_len--; return; }
    if (t[0] == NVHTML_DEF) { l->kind = 'd'; l->body_off++; l->body_len--; return; }
    if (t[0] == NVHTML_ITEM) {
        int i = 1, sp = 0;
        while (i < n && t[i] == ' ') { i++; sp++; }
        l->kind = 'i';
        l->level = sp / 2;
        const int ms = i;
        while (i < n && t[i] != ' ') i++;
        l->mark_off = l->off + ms;
        l->mark_len = i - ms;
        l->numbered = !(l->mark_len == 1 && t[ms] == '-');
        if (i < n) i++;
        l->body_off = l->off + i;
        l->body_len = n - i;
        return;
    }
    int h = 0;
    while (h < n && h < 7 && t[h] == '#') h++;
    if (h >= 1 && h <= 6 && h < n && t[h] == ' ') {
        l->kind = 'h';
        l->level = h;
        l->body_off = l->off + h + 1;
        l->body_len = n - h - 1;
    }
}

typedef struct {
    int32_t off;
    int len;
} cell_t;

#define TABLE_COLS 12
#define TABLE_PAD 8

static void lay_paragraph(lay_t *L, int32_t off, int len, int px, int color, int head) {
    ist_t st;
    memset(&st, 0, sizeof st);
    st.link = -1;
    st.px = px;
    st.color = color;
    st.head = head;
    L->lx = 0;
    L->asc = L->desc = 0;
    L->space_w = 0;
    flow(L, off, len, &st, NULL);
    line_flush(L);
}

static int split_cells(const doc_t *d, const line_t *l, cell_t *c, int max) {
    int n = 0;
    int32_t s = l->body_off;
    const int32_t e = l->body_off + l->body_len;
    for (int32_t i = s; i <= e; i++) {
        if (i == e || d->text[i] == NVHTML_CELL) {
            if (n < max) { c[n].off = s; c[n].len = (int)(i - s); n++; }
            else c[max - 1].len = (int)(i - c[max - 1].off);   // extra cells join the last one
            s = i + 1;
        }
    }
    return n;
}

static int cell_empty(const doc_t *d, const cell_t *c) {
    for (int i = 0; i < c->len; i++) {
        const unsigned char ch = (unsigned char)d->text[c->off + i];
        if (ch == NVHTML_LINK) {   // skip the href
            while (i < c->len && d->text[c->off + i] != NVHTML_LINK_TEXT) i++;
            continue;
        }
        if (ch > 0x20) return 0;
    }
    return 1;
}

static int cell_bold(const doc_t *d, const cell_t *c) {   // a header cell: its text all bold
    const char *t = d->text + c->off;
    int n = c->len;
    while (n && t[n - 1] == ' ') n--;
    return n >= 2 && t[0] == NVHTML_BOLD_ON && t[n - 1] == NVHTML_BOLD_OFF && !memchr(t + 1, NVHTML_BOLD_OFF, n - 2);
}

static void lay_table(lay_t *L, const line_t *rows, int nrows) {
    doc_t *d = L->d;
    const int px = L->body * 90 / 100;
    const int col = L->x1 - L->x0;
    cell_t (*cells)[TABLE_COLS] = (cell_t(*)[TABLE_COLS])malloc(sizeof(cell_t) * TABLE_COLS * nrows);
    int *ncell = (int *)malloc(sizeof(int) * nrows);
    if (!cells || !ncell) { free(cells); free(ncell); return; }
    int ncols = 0;
    for (int r = 0; r < nrows; r++) {
        ncell[r] = split_cells(d, &rows[r], cells[r], TABLE_COLS);
        // trailing empty cells do not make columns
        while (ncell[r] > 1 && cell_empty(d, &cells[r][ncell[r] - 1])) ncell[r]--;
        int any = 0;
        for (int k = 0; k < ncell[r]; k++) any |= !cell_empty(d, &cells[r][k]);
        if (!any) ncell[r] = 0;
        if (ncell[r] > ncols) ncols = ncell[r];
    }
    if (!ncols) { free(cells); free(ncell); return; }
    // column widths from the words: at least the widest word, at most the whole text on one line
    int minw[TABLE_COLS] = {0}, maxw[TABLE_COLS] = {0};
    for (int r = 0; r < nrows; r++) {
        if (ncols > 1 && ncell[r] == 1) continue;   // a row spanning the table
        for (int k = 0; k < ncell[r]; k++) {
            ist_t st;
            memset(&st, 0, sizeof st);
            st.link = -1;
            st.px = px;
            st.color = C_TEXT;
            meas_t m = {0, 0, 0};
            flow(L, cells[r][k].off, cells[r][k].len, &st, &m);
            if ((m.minw + 15) >> 4 > minw[k]) minw[k] = (m.minw + 15) >> 4;   // 1/16 px -> px
            if ((m.maxw + 15) >> 4 > maxw[k]) maxw[k] = (m.maxw + 15) >> 4;
        }
    }
    const int pad = 2 * TABLE_PAD + 1;
    int avail = col - 1 - ncols * pad, summin = 0, summax = 0;
    for (int k = 0; k < ncols; k++) {
        if (maxw[k] > avail) maxw[k] = avail;
        if (minw[k] > maxw[k]) minw[k] = maxw[k];
        if (maxw[k] < 8) maxw[k] = minw[k] = 8;
        summin += minw[k];
        summax += maxw[k];
    }
    int w[TABLE_COLS];
    if (summax <= avail) {
        for (int k = 0; k < ncols; k++) w[k] = maxw[k];
    } else if (summin >= avail) {
        for (int k = 0; k < ncols; k++) w[k] = (int)((long)avail * minw[k] / (summin ? summin : 1));
    } else {
        // Narrow columns first: one whose whole text fits in a fair share of what is left gets it
        // (a header such as "List des groupes" stays on one line); the wide ones then share the rest,
        // the minimum for each and the remainder by how much each wants.
        int fixed[TABLE_COLS] = {0}, left = avail, nfree = ncols;
        for (int changed = 1; changed && nfree > 0;) {
            changed = 0;
            for (int k = 0; k < ncols; k++)
                if (!fixed[k] && maxw[k] <= left / nfree) {
                    w[k] = maxw[k];
                    fixed[k] = 1;
                    left -= maxw[k];
                    nfree--;
                    changed = 1;
                }
        }
        long fmin = 0, fwant = 0;
        for (int k = 0; k < ncols; k++)
            if (!fixed[k]) { fmin += minw[k]; fwant += maxw[k] - minw[k]; }
        for (int k = 0; k < ncols; k++) {
            if (fixed[k]) continue;
            if (fmin >= left) w[k] = (int)((long)left * minw[k] / (fmin ? fmin : 1));
            else w[k] = minw[k] + (int)((left - fmin) * (maxw[k] - minw[k]) / (fwant ? fwant : 1));
        }
    }
    int tw = 1;
    int cx[TABLE_COLS + 1];
    cx[0] = 0;
    for (int k = 0; k < ncols; k++) {
        if (w[k] < 8) w[k] = 8;
        cx[k + 1] = cx[k] + w[k] + pad;
        tw = cx[k + 1] + 1;
    }
    const int x0 = L->x0, x1 = L->x1, indent = L->indent;
    const int32_t top = L->y;
    int32_t y = top;
    int drawn = 0;
    for (int r = 0; r < nrows; r++) {
        if (!ncell[r]) continue;
        const int span = ncols > 1 && ncell[r] == 1;
        const int n = span ? 1 : ncell[r];
        const int first_op = d->nops;
        add_rect(d, x0, y, tw, 1, C_RULE);   // the line above the row
        int32_t row_bottom = y + 1 + 2 * 4;
        for (int k = 0; k < n; k++) {
            const int a = cx[k], b = span ? cx[ncols] : cx[k + 1];
            if (cell_bold(d, &cells[r][k]) || span) add_rect_h(d, x0 + a + 1, y + 1, b - a - 1, 0, C_CELL_BG);   // sized below
            L->x0 = x0 + a + 1 + TABLE_PAD;
            L->x1 = x0 + b - TABLE_PAD;
            L->indent = 0;
            L->y = y + 1 + 4;
            L->min_lh = 0;
            lay_paragraph(L, cells[r][k].off, cells[r][k].len, px, C_TEXT, 0);
            if (L->y + 4 > row_bottom) row_bottom = L->y + 4;
        }
        // the header backgrounds get the row height now
        for (int i = first_op; i < d->nops; i++)
            if (d->ops[i].kind == OP_RECT && d->ops[i].color == C_CELL_BG && d->ops[i].y1 == d->ops[i].y0)
                d->ops[i].y1 = row_bottom;
        for (int k = 0; k <= ncols; k++)   // column lines
            if (k == 0 || k == ncols || !span) add_rect(d, x0 + cx[k], y, 1, row_bottom - y, C_RULE);
        y = row_bottom;
        drawn = 1;
    }
    if (drawn) add_rect(d, x0, y, tw, 1, C_RULE);
    L->x0 = x0;
    L->x1 = x1;
    L->indent = indent;
    L->y = y + 1;
    if (L->y > L->bottom) L->bottom = L->y;
    free(cells);
    free(ncell);
}

// ----------------------------------------------------------------------------------- the layout

static int op_cmp_y(const op_t *a, const op_t *b) { return a->y0 < b->y0 ? -1 : a->y0 > b->y0; }

static void sort_ops(doc_t *d) {   // stable merge sort by top
    const int n = d->nops;
    op_t *tmp = (op_t *)malloc(sizeof(op_t) * (n ? n : 1));
    if (!tmp) return;
    for (int w = 1; w < n; w *= 2) {
        for (int lo = 0; lo < n; lo += 2 * w) {
            int mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
            int i = lo, j = mid, k = lo;
            while (i < mid && j < hi) tmp[k++] = op_cmp_y(&d->ops[j], &d->ops[i]) < 0 ? d->ops[j++] : d->ops[i++];
            while (i < mid) tmp[k++] = d->ops[i++];
            while (j < hi) tmp[k++] = d->ops[j++];
        }
        memcpy(d->ops, tmp, sizeof(op_t) * n);
    }
    free(tmp);
}

static void add_toc(doc_t *d, int32_t y, const line_t *l) {
    if (d->ntoc == d->tcap) {
        int cap = d->tcap ? d->tcap * 2 : 32;
        toc_t *t = (toc_t *)realloc(d->toc, sizeof(toc_t) * cap);
        if (!t) return;
        d->toc = t;
        d->tcap = cap;
    }
    toc_t *t = &d->toc[d->ntoc++];
    t->y = y;
    t->level = (uint8_t)l->level;
    t->off = l->body_off;
    t->len = l->body_len;
    t->aoff = t->alen = 0;
    const char *a = (const char *)memchr(d->text + l->body_off, NVHTML_ANCHOR, l->body_len);
    if (a) {
        t->len = (int32_t)(a - (d->text + l->body_off));
        t->aoff = (int32_t)(a + 1 - d->text);
        t->alen = (int32_t)(l->body_off + l->body_len - t->aoff);
    }
}

int doc_relayout(doc_t *d, int width, int body) {
    d->nops = d->nlinks = d->ntoc = d->nimgs = 0;
    d->width = width;
    d->body = body;
    // lines
    int nl = 1;
    for (size_t i = 0; i < d->n; i++) nl += d->text[i] == '\n';
    line_t *ls = (line_t *)malloc(sizeof(line_t) * nl);
    if (!ls) return -1;
    nl = 0;
    size_t from = 0;
    for (size_t i = 0; i <= d->n; i++)
        if (i == d->n || d->text[i] == '\n') {
            ls[nl].off = (int32_t)from;
            ls[nl].len = (int)(i - from);
            classify(d, &ls[nl]);
            nl++;
            from = i + 1;
        }
    lay_t *L = (lay_t *)calloc(1, sizeof(lay_t));
    if (!L) { free(ls); return -1; }
    L->d = d;
    L->body = body;
    L->x0 = 0;
    L->x1 = width;
    L->code_bg = 1;
    const int gap = body * 70 / 100;
    int32_t y = 0;
    int pending_gap = 0, prev_item_indent = -1;
    for (int i = 0; i < nl; i++) {
        line_t *l = &ls[i];
        if (!l->kind) { pending_gap = 1; prev_item_indent = -1; continue; }
        if (l->kind == 'h') {
            // a heading with nothing under it before the next heading of its level (or the end) is skipped
            int j = i + 1;
            while (j < nl && !ls[j].kind) j++;
            if (l->level >= 2 && (j == nl || (ls[j].kind == 'h' && ls[j].level <= l->level))) continue;
            static const int kScale[7] = {100, 170, 140, 120, 108, 104, 100};
            const int px = body * kScale[l->level] / 100;
            if (y > 0) y += l->level <= 2 ? body * 120 / 100 : body * 80 / 100;
            pending_gap = 0;
            prev_item_indent = -1;
            add_toc(d, y, l);
            L->x0 = 0;
            L->x1 = width;
            L->indent = 0;
            L->y = y;
            L->min_lh = 0;
            lay_paragraph(L, l->body_off, l->body_len, px, C_HEAD, 1);
            y = L->y;
            if (l->level <= 2) {
                add_rect(d, 0, y + 2, width, 1, C_RULE);
                y += 4;
            }
            y += body * 35 / 100;
            continue;
        }
        if (pending_gap && y > 0) y += gap;
        pending_gap = 0;
        L->y = y;
        L->min_lh = 0;
        if (l->kind == 'r') {
            int j = i;
            while (j < nl && ls[j].kind == 'r') j++;
            L->x0 = 0;
            L->x1 = width;
            L->indent = 0;
            lay_table(L, ls + i, j - i);
            y = L->y;
            i = j - 1;
            pending_gap = 1;
            prev_item_indent = -1;
            continue;
        }
        if (l->kind == 'x') {   // preformatted: one tinted band per line, wrapped anywhere
            L->x0 = 12;
            L->x1 = width - 12;
            L->indent = 0;
            const int px = body * 88 / 100;
            const int32_t top = L->y;
            const int band = add_rect_h(d, 0, top, width, 0, C_CODE_BG);   // under the text: first
            ist_t st;
            memset(&st, 0, sizeof st);
            st.link = -1;
            st.px = px;
            st.color = C_TEXT;
            st.code = 0;
            L->code_bg = 0;
            L->wrap_any = 1;   // spaces are kept as they are, lines break anywhere
            L->lx = 0;
            L->asc = L->desc = 0;
            // the line's text in the mono font: code on for the whole line
            st.code = 1;
            int32_t o = l->body_off, e = l->body_off + l->body_len;
            // leading spaces are kept as an indent
            int lead = 0;
            while (o < e && d->text[o] == ' ') { o++; lead++; }
            L->indent = 0;
            L->lx = lead * text_width16(F_MONO, px * 92 / 100, " ", 1);
            if (L->lx > ((L->x1 - L->x0) << 4) / 2) L->lx = 0;
            L->indent = L->lx;
            flow(L, o, (int)(e - o), &st, NULL);
            line_flush(L);
            L->code_bg = 1;
            L->wrap_any = 0;
            if (band >= 0) d->ops[band].y1 = L->y;
            y = L->y;
            continue;
        }
        int indent = 0;
        int px = body;
        if (l->kind == 'i') {
            indent = body * 160 / 100 * (l->level + 1);
            if (indent > width / 2) indent = width / 2;
            // the mark, hanging left of the text
            const int32_t base = y + (font_ascent(F_SANS, px) * 135 / 100 + font_descent(F_SANS, px) * 135 / 100 -
                                      font_ascent(F_SANS, px) - font_descent(F_SANS, px)) / 2 + font_ascent(F_SANS, px);
            op_t *m = push_op(d);
            if (m) {
                m->y0 = y;
                m->y1 = y + (font_ascent(F_SANS, px) + font_descent(F_SANS, px)) * 135 / 100;
                m->base = base;
                m->font = F_SANS;
                m->px = (uint8_t)px;
                m->color = C_DIM;
                if (l->numbered) {
                    const int mw = text_width(F_SANS, px, d->text + l->mark_off, l->mark_len);
                    m->kind = OP_TEXT;
                    m->off = l->mark_off;
                    m->len = (uint16_t)l->mark_len;
                    m->x = (int16_t)(indent - mw - body * 40 / 100);
                    m->w = (int16_t)mw;
                } else {
                    const int b = l->level > 2 ? 2 : l->level;
                    m->kind = OP_STATIC;
                    m->off = b;
                    m->x = (int16_t)(indent - body * 110 / 100);
                    m->w = (int16_t)text_width(F_SANS, px, kStatic[b], (int)strlen(kStatic[b]));
                }
            }
            prev_item_indent = indent;
        } else if (l->kind == 'd') {
            indent = body * 160 / 100;
            prev_item_indent = -1;
        } else if (prev_item_indent >= 0) {
            indent = prev_item_indent;   // a line of the item above (a <br> in it)
        }
        L->x0 = indent;
        L->x1 = width;
        L->indent = 0;
        lay_paragraph(L, l->body_off, l->body_len, px, C_TEXT, 0);
        y = L->y;
    }
    d->height = y + body * 2;
    free(L);
    free(ls);
    sort_ops(d);
    d->max_op_h = 0;
    for (int i = 0; i < d->nops; i++)
        if (d->ops[i].y1 - d->ops[i].y0 > d->max_op_h) d->max_op_h = d->ops[i].y1 - d->ops[i].y0;
    return 0;
}

int doc_layout(doc_t *d, char *text, size_t n, int width, int body) {
    doc_free(d);
    d->text = text;
    d->n = n;
    return doc_relayout(d, width, body);
}

void doc_free(doc_t *d) {
    free(d->text);
    free(d->ops);
    free(d->links);
    free(d->toc);
    free(d->imgs);
    memset(d, 0, sizeof *d);
}

// ---------------------------------------------------------------------------------- the drawing

static int first_op(const doc_t *d, int32_t top) {   // first op that may reach below top
    int32_t from = top - d->max_op_h;
    int lo = 0, hi = d->nops;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (d->ops[mid].y0 < from) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

void doc_draw(const doc_t *d, int x, int y, int top, int h, int focus_link) {
    gfx_clip(0, y, g_w, y + h);
    for (int i = first_op(d, top); i < d->nops; i++) {
        const op_t *o = &d->ops[i];
        if (o->y0 >= top + h) break;
        if (o->y1 <= top) continue;
        const int sy = y + (int)(o->y0 - top);
        if (o->kind == OP_RECT) {
            const int in = (int)o->base;
            gfx_fill(x + o->x, sy + in, o->w, (int)(o->y1 - o->y0) - 2 * in, g_col[o->color]);
        } else if (o->kind == OP_TEXT) {
            uint16_t c = g_col[o->color];
            if (o->link >= 0 && o->link == focus_link) {
                gfx_rrect(x + o->x - 3, sy + 1, o->w + 6, (int)(o->y1 - o->y0) - 2, 4, g_col[C_FOCUS]);
                c = g_col[C_ON_ACCENT];
            }
            text_draw(o->font, o->px, x + o->x, y + (int)(o->base - top), d->text + o->off, o->len, c);
        } else if (o->kind == OP_IMAGE) {
            const int ih = (int)(o->y1 - o->y0);
            if (o->link >= 0 && o->link == focus_link)
                gfx_ring(x + o->x - 4, sy - 4, o->w + 8, ih + 8, 6, 2, g_col[C_FOCUS]);
            doc_draw_image(d, (int)o->off, x + o->x, sy, o->w, ih);
        } else if (o->kind == OP_STATIC) {
            text_draw(o->font, o->px, x + o->x, y + (int)(o->base - top), kStatic[o->off],
                      (int)strlen(kStatic[o->off]), g_col[o->color]);
        }
    }
    gfx_noclip();
}

int doc_link_at(const doc_t *d, int dx, int dy) {
    int best = -1, bestd = 1 << 30;
    for (int i = first_op(d, dy - 24); i < d->nops; i++) {
        const op_t *o = &d->ops[i];
        if (o->y0 > dy + 24) break;
        if ((o->kind != OP_TEXT && o->kind != OP_IMAGE) || o->link < 0) continue;
        // a little slack around the run: fingers are wider than letters
        const int ex = dx < o->x ? o->x - dx : dx > o->x + o->w ? dx - o->x - o->w : 0;
        const int ey = dy < o->y0 ? (int)(o->y0 - dy) : dy >= o->y1 ? (int)(dy - o->y1 + 1) : 0;
        if (ex > 12 || ey > 10) continue;
        const int dist = ex * ex + ey * ey * 4;
        if (dist < bestd) { bestd = dist; best = o->link; }
    }
    return best;
}

int doc_image_at(const doc_t *d, int dx, int dy) {
    for (int i = first_op(d, dy); i < d->nops; i++) {
        const op_t *o = &d->ops[i];
        if (o->y0 > dy) break;
        if (o->kind == OP_IMAGE && dx >= o->x && dx < o->x + o->w && dy >= o->y0 && dy < o->y1) return (int)o->off;
    }
    return -1;
}

int doc_link_rect(const doc_t *d, int link, int32_t from_y, int32_t *y0, int32_t *y1) {
    for (int i = first_op(d, from_y); i < d->nops; i++) {
        const op_t *o = &d->ops[i];
        if ((o->kind == OP_TEXT || o->kind == OP_IMAGE) && o->link == link && o->y1 > from_y) {
            *y0 = o->y0;
            *y1 = o->y1;
            return 0;
        }
    }
    return -1;
}

int doc_links_in(const doc_t *d, int32_t top, int32_t bottom, int *out, int max) {
    int n = 0;
    for (int i = first_op(d, top); i < d->nops && n < max; i++) {
        const op_t *o = &d->ops[i];
        if (o->y0 >= bottom) break;
        if ((o->kind != OP_TEXT && o->kind != OP_IMAGE) || o->link < 0 || o->y0 < top || o->y1 > bottom) continue;
        int dup = 0;
        for (int k = 0; k < n && !dup; k++) dup = out[k] == o->link;
        if (!dup) out[n++] = o->link;
    }
    return n;
}

static int id_eq(const char *a, int an, const char *b, int bn) {   // '_' and ' ' are the same
    if (an != bn) return 0;
    for (int i = 0; i < an; i++) {
        char x = a[i] == '_' ? ' ' : a[i], y = b[i] == '_' ? ' ' : b[i];
        if (x != y) return 0;
    }
    return 1;
}

int doc_anchor_y(const doc_t *d, const char *id, int n) {
    for (int i = 0; i < d->ntoc; i++) {
        const toc_t *t = &d->toc[i];
        if (t->alen && id_eq(d->text + t->aoff, t->alen, id, n)) return t->y;
    }
    for (int i = 0; i < d->ntoc; i++) {   // by the heading's text
        const toc_t *t = &d->toc[i];
        if (id_eq(d->text + t->off, t->len, id, n)) return t->y;
    }
    return -1;
}
