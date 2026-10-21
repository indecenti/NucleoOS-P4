// lay.c — the reader's layout: a chapter's rich text (bk.h) becomes positioned operations (text
// runs, rectangles, pictures) sorted by their top, then pages. Paragraphs are justified (the space
// of a line shared among its gaps), hyphenated (hyph.c, soft hyphens, hard hyphens), with a first-
// line indent or a gap between them; headings can be centred; pictures take their real proportions
// and never grow past a page; a fixed-layout line is a page of its own. Pages break between lines,
// never inside one: a heading is kept with what follows it, a paragraph's first or last line is not
// left alone at the bottom or top of a page (widows and orphans).
// From the Biblioteca app's doc.c (ports/zim/biblioteca), which scrolls instead.
#include "bk.h"

#include <stdlib.h>
#include <string.h>

#include "nvhtml.h"

// The app tells the layout how big a picture is (its file's own size, px); 0 = known.
int lay_image_size(const char *src, int n, int *w, int *h);

// OP_STATIC: text from kStatic (off = index): bullets and the hyphen of a broken word.
static const char *const kStatic[] = {"\xE2\x80\xA2", "\xE2\x97\xA6", "\xE2\x96\xAA", "-"};   // • ◦ ▪ -
enum { ST_HYPHEN = 3 };

// ------------------------------------------------------------------------------------ the output

#define GROW(arr, n, cap, first)                                                          \
    do {                                                                                  \
        if ((n) == (cap)) {                                                               \
            int c_ = (cap) ? (cap) * 2 : (first);                                         \
            void *p_ = realloc((arr), sizeof(*(arr)) * (size_t)c_);                       \
            if (!p_) return -1;                                                           \
            (arr) = p_;                                                                   \
            (cap) = c_;                                                                   \
        }                                                                                 \
    } while (0)

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
    if (h > 0) add_rect_h(d, x, y, w, h, color);
}

static int add_img(doc_t *d, const dimg_t *im) {
    GROW(d->imgs, d->nimgs, d->icap, 64);
    d->imgs[d->nimgs] = *im;
    return d->nimgs++;
}

static int add_link(doc_t *d, int32_t off, int len) {
    GROW(d->links, d->nlinks, d->lcap, 256);
    d->links[d->nlinks].off = off;
    d->links[d->nlinks].len = (uint16_t)(len > 65535 ? 65535 : len);
    return d->nlinks++;
}

static int add_anchor(doc_t *d, int32_t off, int len, int32_t y, int32_t toff) {
    if (len <= 0) return 0;
    GROW(d->anchors, d->nanchors, d->acap, 64);
    anchor_t *a = &d->anchors[d->nanchors++];
    a->off = off;
    a->len = len;
    a->y = y;
    a->toff = toff;
    return 0;
}

static int add_line(doc_t *d, int32_t y0, int32_t y1, int32_t off, int32_t para, int flags) {
    GROW(d->lines, d->nlines, d->lncap, 256);
    dline_t *l = &d->lines[d->nlines++];
    l->y0 = y0;
    l->y1 = y1;
    l->off = off;
    l->para = para;
    l->flags = (uint8_t)flags;
    return 0;
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

#define LINE_OPS 256
#define WORD_FRAGS 32

typedef struct {
    doc_t *d;
    const lopt_t *o;
    int body;
    int x0, x1;              // the column
    int indent;              // continuation lines of the current block start here (1/16 px from x0)
    int32_t y;               // top of the line being built
    op_t line[LINE_OPS];
    int32_t sx16[LINE_OPS];  // where each run of the line starts, 1/16 px
    uint8_t gap[LINE_OPS];   // a space before this run (where justification adds room)
    int nline;
    int lx;                  // pen, 1/16 px from x0
    int asc, desc;           // tallest font in the line
    int min_lh;
    int space_w;             // a space is due before the next word (its width, 1/16 px), 0 = none
    frag_t word[WORD_FRAGS];
    int nword, word_w;
    int wrap_any;            // preformatted text: break anywhere
    int code_bg;
    int justify, center;     // this block's alignment
    int head;                // a heading's lines
    int32_t para;            // the block being laid out
    int first_line;          // the next line is the block's first
    int line_flags;          // extra LF_* for the next line
    int32_t bottom;
    int img_above, img_below;
    int max_px;              // the largest text size of the line
} lay_t;

// Line height: line_h percent of the text size (as in CSS), never less than the font's own height.
static int line_height(const lay_t *L, int asc, int desc) {
    const int px = L->max_px ? L->max_px : L->body;
    const int lh = px * L->o->line_h / 100;
    return lh > asc + desc ? lh : asc + desc;
}

static void line_flush_end(lay_t *L, int last) {
    if (!L->nline && !L->asc) {   // nothing on the line yet: only the pen goes back (a first-line indent
        L->lx = L->indent;        // too narrow for the first word) — the callers' loops rely on that
        L->space_w = 0;
        return;
    }
    const int fam = L->o->serif ? F_SERIF : F_SANS;
    int asc = L->asc ? L->asc : font_ascent(fam, L->body);
    int desc = L->desc ? L->desc : font_descent(fam, L->body);
    int lh = line_height(L, asc, desc);
    if (lh < L->min_lh) lh = L->min_lh;
    int above = (lh - asc - desc) / 2 + asc;
    int has_img = 0;
    if (L->img_above || L->img_below) {   // pictures: the room the tallest needs, a little air around
        int below = lh - above;
        if (L->img_above + 4 > above) above = L->img_above + 4;
        if (L->img_below + 4 > below) below = L->img_below + 4;
        lh = above + below;
        has_img = 1;
    }
    // alignment: justified lines share the room left among their gaps, centred lines are moved
    const int col16 = (L->x1 - L->x0) << 4;
    int32_t shift[LINE_OPS];
    int ngaps = 0;
    for (int i = 1; i < L->nline; i++) ngaps += L->gap[i];
    const int room = col16 - L->lx;
    int32_t acc = 0, per = 0, rest = 0;
    if (L->justify && !last && ngaps && room > 0 && room < col16 / 3) { per = room / ngaps; rest = room % ngaps; }
    int gi = 0;
    for (int i = 0; i < L->nline; i++) {
        if (i && L->gap[i] && per) { acc += per + (gi < rest ? 1 : 0); gi++; }
        shift[i] = acc;
    }
    if (L->center && room > 0) for (int i = 0; i < L->nline; i++) shift[i] = room / 2;
    const int32_t base = L->y + above;
    for (int i = 0; i < L->nline; i++) {   // backgrounds first: the text is drawn over them
        op_t *o = &L->line[i];
        if (o->kind == OP_TEXT && o->color == C_CODE_BG) {
            const int r = add_rect_h(L->d, L->x0 + ((L->sx16[i] + shift[i] + 8) >> 4) - 3, L->y, o->w + 6, lh, C_CODE_BG);
            if (r >= 0) L->d->ops[r].base = 2;   // a rectangle's base: its inset, top and bottom
            o->color = o->link >= 0 ? C_LINK : C_TEXT;
        }
    }
    int32_t first_off = -1;
    for (int i = 0; i < L->nline; i++) {
        op_t *o = push_op(L->d);
        if (!o) break;
        *o = L->line[i];
        o->x = (int16_t)(((L->sx16[i] + shift[i] + 8) >> 4) + L->x0);
        if (o->kind == OP_IMAGE) {   // len held the height, base how far below the baseline
            o->y1 = base + o->base;
            o->y0 = o->y1 - o->len;
            o->base = base;
            continue;
        }
        if (o->kind == OP_TEXT && first_off < 0) first_off = o->off;
        o->y0 = L->y;
        o->y1 = L->y + lh;
        o->base += base;   // base held the raise
    }
    int flags = L->line_flags | (L->first_line ? LF_FIRST : 0) | (last ? LF_LAST : 0) | (L->head ? LF_HEAD : 0) |
                (has_img ? LF_IMAGE : 0);
    add_line(L->d, L->y, L->y + lh, first_off, L->para, flags);
    L->first_line = 0;
    L->line_flags = 0;
    L->nline = 0;
    L->y += lh;
    if (L->y > L->bottom) L->bottom = L->y;
    L->lx = L->indent;
    L->asc = L->desc = 0;
    L->img_above = L->img_below = 0;
    L->space_w = 0;
    L->max_px = 0;
}

static void line_flush(lay_t *L) { line_flush_end(L, 0); }

static int line_push(lay_t *L, int space) {   // a new run at the pen; returns its index
    if (L->nline == LINE_OPS) line_flush(L);
    const int i = L->nline++;
    L->sx16[i] = L->lx;
    L->gap[i] = (uint8_t)(space > 0);
    memset(&L->line[i], 0, sizeof L->line[i]);
    L->line[i].link = -1;
    return i;
}

static void place_image(lay_t *L, const frag_t *f, int space) {
    L->lx += space;
    const int i = line_push(L, space);
    op_t *o = &L->line[i];
    o->kind = OP_IMAGE;
    o->w = (int16_t)((f->w + 8) >> 4);
    o->off = f->image;
    o->len = (uint16_t)f->ih;
    o->base = f->below;
    o->link = f->link;
    L->lx += f->w;
    if (f->ih - f->below > L->img_above) L->img_above = f->ih - f->below;
    if (f->below > L->img_below) L->img_below = f->below;
}

static void metrics(lay_t *L, int font, int px, int raise) {
    if (!raise && px > L->max_px) L->max_px = px;
    const int a = font_ascent(font, px) - raise, de = font_descent(font, px) + raise;
    if (a > L->asc) L->asc = a;
    if (de > L->desc) L->desc = de;
}

// Places a fragment (or `len` bytes of it) at the pen. Without justification, runs of one style
// with a single space between them are merged (fewer operations to draw).
static void place(lay_t *L, const frag_t *f, int len, int w, int space) {
    const doc_t *d = L->d;
    const int color = f->code && L->code_bg ? C_CODE_BG : f->color;
    if (L->nline && !L->justify) {
        op_t *p = &L->line[L->nline - 1];
        const int32_t end = p->off + p->len;
        const int gap = space ? 1 : 0;
        if (p->kind == OP_TEXT && p->font == f->font && p->px == f->px && p->link == f->link && p->color == color &&
            p->base == f->raise && end + gap == f->off && (!gap || d->text[end] == ' ') && p->len + gap + len < 65000) {
            p->len = (uint16_t)(p->len + gap + len);
            L->lx += space + w;
            p->w = (int16_t)((L->lx - L->sx16[L->nline - 1] + 8) >> 4);
            metrics(L, f->font, f->px, f->raise);
            return;
        }
    }
    L->lx += space;
    const int i = line_push(L, space);
    op_t *o = &L->line[i];
    o->kind = OP_TEXT;
    o->w = (int16_t)((w + 8) >> 4);
    o->font = f->font;
    o->px = f->px;
    o->color = (uint8_t)color;
    o->off = f->off;
    o->len = (uint16_t)len;
    o->link = f->link;
    o->base = f->raise;
    L->lx += w;
    metrics(L, f->font, f->px, f->raise);
}

static void place_hyphen(lay_t *L, const frag_t *f) {
    const int w = text_width16(f->font, f->px, kStatic[ST_HYPHEN], 1);
    const int i = line_push(L, 0);
    op_t *o = &L->line[i];
    o->kind = OP_STATIC;
    o->off = ST_HYPHEN;
    o->font = f->font;
    o->px = f->px;
    o->color = f->color;
    o->w = (int16_t)((w + 8) >> 4);
    o->base = f->raise;
    L->lx += w;
}

// Break points of a word of one style: hyphenation patterns, soft hyphens, after a hard hyphen.
// out[] are byte offsets (the break goes before that byte); 1 = the piece before needs a hyphen.
static int word_breaks(const lay_t *L, const frag_t *f, int *out, uint8_t *needs, int max) {
    const char *t = L->d->text + f->off;
    const int n = f->len;
    int k = 0;
    for (int i = 1; i + 1 < n && k < max; i++) {
        if ((unsigned char)t[i - 1] == 0xC2 && (unsigned char)t[i] == 0xAD) continue;
        if (i >= 2 && (unsigned char)t[i - 2] == 0xC2 && (unsigned char)t[i - 1] == 0xAD) { out[k] = i; needs[k++] = 1; continue; }
        if (t[i - 1] == '-' && i >= 2 && t[i] != '-') { out[k] = i; needs[k++] = 0; continue; }
        if (i >= 3 && (unsigned char)t[i - 3] == 0xE2 && (unsigned char)t[i - 2] == 0x80 &&
            ((unsigned char)t[i - 1] == 0x94 || (unsigned char)t[i - 1] == 0x93)) { out[k] = i; needs[k++] = 0; }   // — –
    }
    if (L->o->hyphen && k == 0) {   // a word with its own break points keeps them
        int pts[48];
        const int m = hyph_word(L->o->lang, t, n, pts, 48);
        for (int i = 0; i < m && k < max; i++) { out[k] = pts[i]; needs[k++] = 1; }
    }
    return k;
}

// The word does not fit in what is left of the line: the longest first piece that fits (with its
// hyphen) is placed, the line ends, the rest stays the word. Returns 1 when it broke it.
static int hyphenate(lay_t *L, int space) {
    if (L->nword != 1 || L->word[0].image >= 0 || L->wrap_any) return 0;
    frag_t *f = &L->word[0];
    // a "word" longer than this (a URL, a line of code, a run of dashes) is broken anywhere instead:
    // that is linear, while trying break points over a huge one would cost its length squared
    if (f->len < 5 || f->len > 120) return 0;
    int br[64];
    uint8_t needs[64];
    const int nb = word_breaks(L, f, br, needs, 64);
    const int col = (L->x1 - L->x0) << 4;
    const int hw = text_width16(f->font, f->px, "-", 1);
    for (int i = nb - 1; i >= 0; i--) {
        const int w = text_width16(f->font, f->px, L->d->text + f->off, br[i]);
        if (L->lx + space + w + (needs[i] ? hw : 0) > col) continue;
        place(L, f, br[i], w, space);
        if (needs[i]) place_hyphen(L, f);
        line_flush(L);
        f->off += br[i];
        f->len -= br[i];
        f->w -= w;   // widths add up (no kerning): what is left is the rest
        L->word_w = f->w;
        return 1;
    }
    return 0;
}

static void word_commit(lay_t *L) {
    if (!L->nword) return;
    const int col = (L->x1 - L->x0) << 4;
    int space = L->space_w;
    if (L->lx == L->indent) space = 0;
    while (L->lx > L->indent && L->lx + space + L->word_w > col) {   // does not fit: break it, or next line
        if (hyphenate(L, space)) { space = 0; continue; }
        line_flush(L);
        space = 0;
    }
    if (L->lx + space + L->word_w <= col) {
        for (int i = 0; i < L->nword; i++) {
            if (L->word[i].image >= 0) place_image(L, &L->word[i], i == 0 ? space : 0);
            else place(L, &L->word[i], L->word[i].len, L->word[i].w, i == 0 ? space : 0);
        }
    } else {   // longer than the column: hyphenate it, or break it anywhere
        while (hyphenate(L, space)) space = 0;
        if (L->lx + space + L->word_w <= col) {
            for (int i = 0; i < L->nword; i++) {
                if (L->word[i].image >= 0) place_image(L, &L->word[i], i == 0 ? space : 0);
                else place(L, &L->word[i], L->word[i].len, L->word[i].w, i == 0 ? space : 0);
            }
        } else
            for (int i = 0; i < L->nword; i++) {
                frag_t f = L->word[i];
                int sp = i == 0 ? space : 0;
                if (f.image >= 0) {
                    if (L->lx > L->indent && L->lx + sp + f.w > col) { line_flush(L); sp = 0; }
                    place_image(L, &f, sp);
                    continue;
                }
                while (f.len > 0) {
                    const int rm = col - L->lx - sp;
                    int k = text_fit(f.font, f.px, L->d->text + f.off, f.len, rm > 0 ? rm >> 4 : 0);
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

static void frag_style(const lay_t *L, const ist_t *s, frag_t *f) {
    int px = s->px;
    const int base = L->o->serif ? F_SERIF : F_SANS;
    const int bold = s->bold || s->head;
    if (s->code) { f->font = F_MONO; px = px * 88 / 100; }
    else f->font = (uint8_t)(base + (bold && s->italic ? 3 : bold ? 1 : s->italic ? 2 : 0));
    f->raise = 0;
    if (s->sup || s->sub) {
        f->raise = (int8_t)(s->sup ? -(s->px * 36 / 100) : s->px * 18 / 100);
        px = px * 70 / 100;
    }
    f->px = (uint8_t)(px < 6 ? 6 : px > 200 ? 200 : px);
    f->link = (int16_t)(s->head ? -1 : s->link);
    f->color = (uint8_t)(f->link >= 0 ? C_LINK : s->color);
    f->code = (uint8_t)s->code;
}

// "20.66ex" / "50px" / "50" / "" -> pixels on this screen (0 when unknown).
static int length_px(const char *s, int n, int body) {
    char b[24];
    if (n <= 0 || n >= (int)sizeof b) return 0;
    memcpy(b, s, (size_t)n);
    b[n] = 0;
    char *end = NULL;
    const double v = strtod(b, &end);
    if (!end || end == b || v <= 0 || v > 20000) return 0;
    if (!strcmp(end, "ex")) return (int)(v * body * 0.536 + 0.5);
    if (!strcmp(end, "em")) return (int)(v * body + 0.5);
    if (!strcmp(end, "px") || !*end) return (int)(v * body / 16.0 + 0.5);   // CSS pixels for a 16 px body
    return 0;
}

// A picture: the next fragment of the word (inline), a line of its own (block) or a page of its
// own (fixed). Returns its width (1/16 px) for the table measure, which places nothing.
static int image_frag(lay_t *L, const ist_t *st, const int32_t *fs, const int *fl, int measure, int fixed) {
    doc_t *d = L->d;
    const char *t = d->text;
    const int body = L->body, col = L->x1 - L->x0, page = L->o->page_h;
    int w = length_px(t + fs[1], fl[1], body), h = length_px(t + fs[2], fl[2], body);
    const int va = length_px(t + fs[3], fl[3], body);
    int mask = 0, block = 0, figure = 0;
    for (int k = 0; k < fl[4]; k++) {
        mask |= t[fs[4] + k] == 'm';
        block |= t[fs[4] + k] == 'd';
        figure |= t[fs[4] + k] == 'f';
    }
    if (!mask && (fixed || w <= 0 || h <= 0)) {   // the file's own size: real proportions
        int iw = 0, ih = 0;
        if (lay_image_size(t + fs[0], fl[0], &iw, &ih) == 0 && iw > 0 && ih > 0) {
            if (w > 0 && h <= 0) h = (int)((long)w * ih / iw);
            else if (h > 0 && w <= 0) w = (int)((long)h * iw / ih);
            else if (w <= 0) { w = iw; h = ih; }
            if (fixed) { w = iw; h = ih; }
        }
    }
    if (w <= 0 && h <= 0) { w = body * 8; h = body * 6; }   // nothing known: a 4:3 box
    else if (w <= 0) w = h;
    else if (h <= 0) h = w;
    if (fixed) {   // fitted to the whole page, either way
        const long sw = (long)col * 1000 / w, sh = (long)page * 1000 / h;
        const long s = sw < sh ? sw : sh;
        w = (int)(w * s / 1000);
        h = (int)(h * s / 1000);
    }
    if (w > col && col > 0) { h = (int)((long)h * col / w); w = col; }
    const int hmax = page > 0 ? page - (fixed ? 0 : body) : body * 30;   // never taller than a page
    if (h > hmax && hmax > 0) { w = (int)((long)w * hmax / h); h = hmax; }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (!mask && (figure || fixed || w > col * 6 / 10)) block = 1;
    if (measure) return w << 4;
    dimg_t im = {fs[0], fs[5], (uint16_t)fl[0], (uint16_t)fl[5], (int16_t)w, (int16_t)h, (uint8_t)mask, (uint8_t)block,
                 (uint8_t)fixed};
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
    word_commit(L);
    if (L->nline || L->lx > L->indent) line_flush_end(L, 1);
    const int keep_center = L->center, keep_just = L->justify;
    L->center = L->justify = 0;
    L->lx = ((col << 4) - f.w) / 2;
    if (L->lx < 0) L->lx = 0;
    if (fixed) {   // the whole page: no air around it
        L->line_flags |= LF_PAGE;
        L->min_lh = 0;
        L->lx = ((col << 4) - f.w) / 2;
        const int i = line_push(L, 0);
        op_t *o = &L->line[i];
        o->kind = OP_IMAGE;
        o->w = (int16_t)w;
        o->off = f.image;
        o->len = (uint16_t)h;
        L->lx += f.w;
        L->img_above = h - 4;   // line_flush adds 4 above and below
        L->img_below = -4;
        L->asc = 1;
    } else {
        place_image(L, &f, 0);
    }
    line_flush_end(L, 1);
    L->center = keep_center;
    L->justify = keep_just;
    L->space_w = 0;
    return f.w;
}

typedef struct { int minw, maxw, cur; } meas_t;

// Feeds the text of [off, off + len) into the line builder. measure: only collect word widths.
static void flow(lay_t *L, int32_t off, int len, ist_t *st, meas_t *m, int fixed) {
    doc_t *d = L->d;
    const char *t = d->text;
    int32_t i = off;
    const int32_t end = off + len;
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
            case NVHTML_ANCHOR:   // the rest of a heading line: its id
                if (!m) add_anchor(d, i, (int)(end - i), L->y, off);
                i = end;
                break;
            case RT_ID: {
                const int32_t a0 = i;
                while (i < end && t[i] != RT_ID_END) i++;
                if (!m) add_anchor(d, a0, (int)(i - a0), L->y, a0);
                if (i < end) i++;
                break;
            }
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
                    if (m) word_w += image_frag(L, st, fs, fl, 1, 0);
                    else image_frag(L, st, fs, fl, 0, fixed);
                }
                break;
            }
            default: break;
            }
            continue;
        }
        if (c == ' ' && !L->wrap_any) {
            frag_t f;
            frag_style(L, st, &f);
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
            while (i < end && t[i] == ' ') i++;
            continue;
        }
        int32_t j = i;
        while (j < end && (t[j] != ' ' || L->wrap_any) && (unsigned char)t[j] >= 0x20) j++;
        frag_t f;
        frag_style(L, st, &f);
        f.image = -1;
        f.off = i;
        f.len = (int)(j - i);
        f.w = text_width16(f.font, f.px, t + i, f.len);
        if (m) word_w += f.w;
        else word_add(L, &f);
        i = j;
    }
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
    char kind;               // 'p' text, 'h' heading, 'i' item, 'd' definition, 'r' row, 'x' pre, 'f' fixed, 0 blank
    int level;
    int32_t body_off;
    int body_len;
    int32_t mark_off;
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
    for (int i = 0; i < n && !vis; i++) vis = (unsigned char)t[i] >= 0x21 || t[i] == NVHTML_IMAGE || t[i] == RT_ID;
    if (!vis) { l->kind = 0; return; }
    if (t[0] == RT_FIXED) { l->kind = 'f'; l->body_off++; l->body_len--; return; }
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
    // RT_ID markers may come first: a heading after them is still a heading
    int s = 0;
    while (s < n && t[s] == RT_ID) {
        const char *e = memchr(t + s, RT_ID_END, (size_t)(n - s));
        if (!e) break;
        s = (int)(e - t) + 1;
    }
    int h = 0;
    while (s + h < n && h < 7 && t[s + h] == '#') h++;
    if (h >= 1 && h <= 6 && s + h < n && t[s + h] == ' ') {
        l->kind = 'h';
        l->level = h;
        l->body_off = l->off + s + h + 1;
        l->body_len = n - s - h - 1;
        l->mark_off = l->off;   // the ids before the heading
        l->mark_len = s;
    }
}

typedef struct { int32_t off; int len; } cell_t;

#define TABLE_COLS 12
#define TABLE_PAD 8

static void lay_paragraph(lay_t *L, int32_t off, int len, int px, int color, int head, int first_indent, int fixed) {
    ist_t st;
    memset(&st, 0, sizeof st);
    st.link = -1;
    st.px = px;
    st.color = color;
    st.head = head;
    L->lx = first_indent << 4;
    L->asc = L->desc = 0;
    L->space_w = 0;
    L->first_line = 1;
    flow(L, off, len, &st, NULL, fixed);
    line_flush_end(L, 1);
    L->para++;
}

static int split_cells(const doc_t *d, const line_t *l, cell_t *c, int max) {
    int n = 0;
    int32_t s = l->body_off;
    const int32_t e = l->body_off + l->body_len;
    for (int32_t i = s; i <= e; i++) {
        if (i == e || d->text[i] == NVHTML_CELL) {
            if (n < max) { c[n].off = s; c[n].len = (int)(i - s); n++; }
            else c[max - 1].len = (int)(i - c[max - 1].off);
            s = i + 1;
        }
    }
    return n;
}

static int cell_empty(const doc_t *d, const cell_t *c) {
    for (int i = 0; i < c->len; i++) {
        const unsigned char ch = (unsigned char)d->text[c->off + i];
        if (ch == NVHTML_LINK) {
            while (i < c->len && d->text[c->off + i] != NVHTML_LINK_TEXT) i++;
            continue;
        }
        if (ch > 0x20) return 0;
    }
    return 1;
}

static int cell_bold(const doc_t *d, const cell_t *c) {
    const char *t = d->text + c->off;
    int n = c->len;
    while (n && t[n - 1] == ' ') n--;
    return n >= 2 && t[0] == NVHTML_BOLD_ON && t[n - 1] == NVHTML_BOLD_OFF && !memchr(t + 1, NVHTML_BOLD_OFF, n - 2);
}

// A table: one unbreakable line for pagination when it fits a page (rows are recorded as lines).
static void lay_table(lay_t *L, const line_t *rows, int nrows) {
    doc_t *d = L->d;
    const int px = L->body * 88 / 100;
    const int col = L->x1 - L->x0;
    cell_t (*cells)[TABLE_COLS] = (cell_t(*)[TABLE_COLS])malloc(sizeof(cell_t) * TABLE_COLS * nrows);
    int *ncell = (int *)malloc(sizeof(int) * nrows);
    if (!cells || !ncell) { free(cells); free(ncell); return; }
    int ncols = 0;
    for (int r = 0; r < nrows; r++) {
        ncell[r] = split_cells(d, &rows[r], cells[r], TABLE_COLS);
        while (ncell[r] > 1 && cell_empty(d, &cells[r][ncell[r] - 1])) ncell[r]--;
        int any = 0;
        for (int k = 0; k < ncell[r]; k++) any |= !cell_empty(d, &cells[r][k]);
        if (!any) ncell[r] = 0;
        if (ncell[r] > ncols) ncols = ncell[r];
    }
    if (!ncols) { free(cells); free(ncell); return; }
    int minw[TABLE_COLS] = {0}, maxw[TABLE_COLS] = {0};
    for (int r = 0; r < nrows; r++) {
        if (ncols > 1 && ncell[r] == 1) continue;
        for (int k = 0; k < ncell[r]; k++) {
            ist_t st;
            memset(&st, 0, sizeof st);
            st.link = -1;
            st.px = px;
            st.color = C_TEXT;
            meas_t m = {0, 0, 0};
            flow(L, cells[r][k].off, cells[r][k].len, &st, &m, 0);
            if ((m.minw + 15) >> 4 > minw[k]) minw[k] = (m.minw + 15) >> 4;
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
    const int x0 = L->x0, x1 = L->x1, indent = L->indent, just = L->justify;
    L->justify = 0;
    int32_t y = L->y;
    int drawn = 0;
    for (int r = 0; r < nrows; r++) {
        if (!ncell[r]) continue;
        const int span = ncols > 1 && ncell[r] == 1;
        const int n = span ? 1 : ncell[r];
        const int first_op = d->nops, first_line = d->nlines;
        add_rect(d, x0, y, tw, 1, C_RULE);
        int32_t row_bottom = y + 1 + 2 * 4;
        for (int k = 0; k < n; k++) {
            const int a = cx[k], b = span ? cx[ncols] : cx[k + 1];
            if (cell_bold(d, &cells[r][k]) || span) add_rect_h(d, x0 + a + 1, y + 1, b - a - 1, 0, C_CELL_BG);
            L->x0 = x0 + a + 1 + TABLE_PAD;
            L->x1 = x0 + b - TABLE_PAD;
            L->indent = 0;
            L->y = y + 1 + 4;
            L->min_lh = 0;
            lay_paragraph(L, cells[r][k].off, cells[r][k].len, px, C_TEXT, 0, 0, 0);
            if (L->y + 4 > row_bottom) row_bottom = L->y + 4;
        }
        for (int i = first_op; i < d->nops; i++)
            if (d->ops[i].kind == OP_RECT && d->ops[i].color == C_CELL_BG && d->ops[i].y1 == d->ops[i].y0) d->ops[i].y1 = row_bottom;
        for (int k = 0; k <= ncols; k++)
            if (k == 0 || k == ncols || !span) add_rect(d, x0 + cx[k], y, 1, row_bottom - y, C_RULE);
        // the row is one line for pagination: the cells' lines are replaced by it
        d->nlines = first_line;
        add_line(d, y, row_bottom + (r == nrows - 1 ? 1 : 0), cells[r][0].off, L->para, LF_FIRST | LF_LAST);
        y = row_bottom;
        drawn = 1;
    }
    if (drawn) add_rect(d, x0, y, tw, 1, C_RULE);
    L->x0 = x0;
    L->x1 = x1;
    L->indent = indent;
    L->justify = just;
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

static int add_head(doc_t *d, int32_t y, const line_t *l) {
    GROW(d->heads, d->nheads, d->hcap, 32);
    head_t *h = &d->heads[d->nheads++];
    h->y = y;
    h->level = (uint8_t)l->level;
    h->off = l->body_off;
    h->len = l->body_len;
    const char *a = (const char *)memchr(d->text + l->body_off, NVHTML_ANCHOR, l->body_len);
    if (a) h->len = (int32_t)(a - (d->text + l->body_off));
    return 0;
}

// Pages: the largest break that fits, between lines that no operation crosses, moved up to keep a
// heading with what follows and to avoid widows and orphans (when that leaves the page at least
// half full). A line marked LF_PAGE is a page of its own.
static void paginate(doc_t *d) {
    const int32_t ph = d->opt.page_h > 40 ? d->opt.page_h : 40;
    const int nl = d->nlines;
    free(d->page_top);
    free(d->page_off);
    d->page_top = d->page_off = NULL;
    d->npages = 0;
    // can[i]: a page may start at line i (no operation reaches across lines[i].y0)
    uint8_t *can = (uint8_t *)malloc((size_t)(nl ? nl : 1));
    int32_t *tops = (int32_t *)malloc(sizeof(int32_t) * (size_t)(nl + 2));
    int32_t *offs = (int32_t *)malloc(sizeof(int32_t) * (size_t)(nl + 2));
    int *start = (int *)malloc(sizeof(int) * (size_t)(nl + 2));
    if (!can || !tops || !offs || !start) { free(can); free(tops); free(offs); free(start); return; }
    // lines are in layout order, which is top to bottom except inside table rows (one line each)
    int32_t reach = 0;   // the lowest bottom of every operation above the line being looked at
    int oi = 0;
    for (int i = 0; i < nl; i++) {
        const int32_t y = d->lines[i].y0;
        while (oi < d->nops && d->ops[oi].y0 < y) {
            if (d->ops[oi].y1 > reach) reach = d->ops[oi].y1;
            oi++;
        }
        can[i] = reach <= y && (i == 0 || d->lines[i - 1].y0 < y);
    }
    int np = 0, i = 0;
    while (i < nl) {
        tops[np] = np == 0 ? 0 : d->lines[i].y0;
        start[np] = i;
        offs[np] = d->lines[i].off >= 0 ? d->lines[i].off : 0;
        np++;
        const int32_t top = tops[np - 1], limit = top + ph;
        if (d->lines[i].flags & LF_PAGE) { i++; continue; }
        // the last line that fits
        int j = i + 1, best = -1;
        while (j < nl && d->lines[j].y1 <= limit + 1 && !(d->lines[j].flags & LF_PAGE)) {
            if (can[j]) best = j;
            j++;
        }
        if (j >= nl && d->lines[nl - 1].y1 <= limit + 1) break;   // the rest fits on this page
        if (j < nl && (d->lines[j].flags & LF_PAGE) && can[j]) best = j;
        if (best < 0) {   // nothing breaks cleanly: cut at the first line below the page
            best = j > i + 1 ? j : i + 1;
            if (best >= nl) break;
        } else if (!(d->lines[best].flags & LF_PAGE)) {
            const int32_t half = top + ph / 2;
            int b = best;
            // a heading (and the ids before it) at the bottom goes with what follows
            while (b - 1 > i && (d->lines[b - 1].flags & LF_HEAD) && can[b - 1]) b--;
            if (b != best && d->lines[b].y0 > half) best = b;
            const dline_t *last = &d->lines[best - 1], *next = &d->lines[best];
            // orphan: a paragraph's first line alone at the bottom
            if (best - 1 > i && (last->flags & LF_FIRST) && !(last->flags & LF_LAST) && next->para == last->para &&
                can[best - 1] && last->y0 > half)
                best--;
            // widow: a paragraph's last line alone at the top of the next page
            else if ((next->flags & LF_LAST) && !(next->flags & LF_FIRST) && last->para == next->para && best - 1 > i &&
                     can[best - 1] && !(d->lines[best - 1].flags & LF_FIRST) && d->lines[best - 1].y0 > half)
                best--;
        }
        i = best;
    }
    if (!np) { tops[0] = 0; offs[0] = 0; np = 1; }
    tops[np] = d->height > tops[np - 1] ? d->height : tops[np - 1] + 1;
    d->page_top = tops;
    d->page_off = offs;
    d->npages = np;
    free(can);
    free(start);
}

int doc_relayout(doc_t *d, const lopt_t *opt) {
    d->nops = d->nlinks = d->nheads = d->nimgs = d->nanchors = d->nlines = 0;
    d->opt = *opt;
    const int width = opt->width, body = opt->body;
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
    L->o = &d->opt;
    L->body = body;
    L->x0 = 0;
    L->x1 = width;
    L->code_bg = 1;
    const int gap = body * 55 / 100, indent_px = body * 3 / 2;
    int32_t y = 0;
    int pending_gap = 0, prev_item_indent = -1, after_head = 1, block_start = 1;
    for (int i = 0; i < nl; i++) {
        line_t *l = &ls[i];
        if (!l->kind) { pending_gap = 1; prev_item_indent = -1; block_start = 1; continue; }
        L->justify = 0;
        L->center = 0;
        L->head = 0;
        if (l->kind == 'h') {
            int j = i + 1;
            while (j < nl && !ls[j].kind) j++;
            if (l->level >= 2 && (j == nl || (ls[j].kind == 'h' && ls[j].level <= l->level))) continue;
            static const int kScale[7] = {100, 160, 135, 118, 108, 104, 100};
            const int px = body * kScale[l->level] / 100;
            if (y > 0) y += l->level <= 2 ? body * 160 / 100 : body * 90 / 100;
            else if (l->level <= 2) y += body * 60 / 100;   // a chapter's title: a little lower than the top
            pending_gap = 0;
            prev_item_indent = -1;
            // the ids in front of the heading point at it
            for (int32_t k = l->mark_off; k < l->mark_off + l->mark_len; k++)
                if (d->text[k] == RT_ID) {
                    const char *e = memchr(d->text + k, RT_ID_END, (size_t)(l->mark_off + l->mark_len - k));
                    if (!e) break;
                    add_anchor(d, k + 1, (int)(e - (d->text + k + 1)), y, l->off);
                    k = (int32_t)(e - d->text);
                }
            add_head(d, y, l);
            L->x0 = 0;
            L->x1 = width;
            L->indent = 0;
            L->y = y;
            L->min_lh = 0;
            L->center = opt->center_heads;
            L->head = 1;
            lay_paragraph(L, l->body_off, l->body_len, px, C_HEAD, 1, 0, 0);
            y = L->y + body * 45 / 100;
            after_head = 1;
            block_start = 1;
            continue;
        }
        if (l->kind == 'f') {   // a fixed-layout page
            L->x0 = 0;
            L->x1 = width;
            L->indent = 0;
            L->y = y;
            L->min_lh = 0;
            lay_paragraph(L, l->body_off, l->body_len, body, C_TEXT, 0, 0, 1);
            y = L->y;
            continue;
        }
        if (pending_gap && y > 0 && !opt->indent) y += gap;
        if (pending_gap && y > 0 && opt->indent && (l->kind != 'p' || after_head)) y += gap / 2;
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
            L->para++;
            y = L->y;
            i = j - 1;
            pending_gap = 1;
            prev_item_indent = -1;
            after_head = 0;
            block_start = 1;
            continue;
        }
        if (l->kind == 'x') {   // preformatted: one tinted band per line, wrapped anywhere
            L->x0 = 12;
            L->x1 = width - 12;
            L->indent = 0;
            const int px = body * 85 / 100;
            const int band = add_rect_h(d, 0, L->y, width, 0, C_CODE_BG);
            ist_t st;
            memset(&st, 0, sizeof st);
            st.link = -1;
            st.px = px;
            st.color = C_TEXT;
            st.code = 1;
            L->code_bg = 0;
            L->wrap_any = 1;
            L->asc = L->desc = 0;
            int32_t o = l->body_off, e = l->body_off + l->body_len;
            int lead = 0;
            while (o < e && d->text[o] == ' ') { o++; lead++; }
            L->lx = lead * text_width16(F_MONO, px * 88 / 100, " ", 1);
            if (L->lx > ((L->x1 - L->x0) << 4) / 2) L->lx = 0;
            L->indent = L->lx;
            L->first_line = 1;
            flow(L, o, (int)(e - o), &st, NULL, 0);
            line_flush_end(L, 1);
            L->para++;
            L->code_bg = 1;
            L->wrap_any = 0;
            if (band >= 0) d->ops[band].y1 = L->y;
            y = L->y;
            after_head = 0;
            continue;
        }
        int indent = 0, first = 0;
        if (l->kind == 'i') {
            indent = body * 160 / 100 * (l->level + 1);
            if (indent > width / 2) indent = width / 2;
            const int fam = opt->serif ? F_SERIF : F_SANS;
            const int asc = font_ascent(fam, body), desc = font_descent(fam, body);
            const int lh = line_height(L, asc, desc);
            op_t *m = push_op(d);
            if (m) {
                m->y0 = y;
                m->y1 = y + lh;
                m->base = y + (lh - asc - desc) / 2 + asc;
                m->font = (uint8_t)fam;
                m->px = (uint8_t)body;
                m->color = C_DIM;
                if (l->numbered) {
                    const int mw = text_width(fam, body, d->text + l->mark_off, l->mark_len);
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
                    m->w = (int16_t)text_width(fam, body, kStatic[b], (int)strlen(kStatic[b]));
                }
            }
            prev_item_indent = indent;
        } else if (l->kind == 'd') {
            indent = body * 2;   // quotes, epigraphs, definitions
            prev_item_indent = -1;
        } else if (prev_item_indent >= 0) {
            indent = prev_item_indent;
        } else if (opt->indent && block_start && !after_head) {
            first = indent_px;   // the book's paragraph: the first line indented
        }
        L->x0 = indent;
        L->x1 = l->kind == 'd' ? width - body : width;
        L->indent = 0;
        L->justify = opt->justify && l->kind != 'i';
        lay_paragraph(L, l->body_off, l->body_len, body, C_TEXT, 0, first, 0);
        y = L->y;
        after_head = 0;
        block_start = 0;
    }
    d->height = y + body;
    free(L);
    free(ls);
    sort_ops(d);
    d->max_op_h = 0;
    for (int i = 0; i < d->nops; i++)
        if (d->ops[i].y1 - d->ops[i].y0 > d->max_op_h) d->max_op_h = d->ops[i].y1 - d->ops[i].y0;
    paginate(d);
    return 0;
}

int doc_layout(doc_t *d, char *text, size_t n, const lopt_t *opt) {
    doc_free(d);
    d->text = text;
    d->n = n;
    return doc_relayout(d, opt);
}

void doc_free(doc_t *d) {
    free(d->text);
    free(d->ops);
    free(d->links);
    free(d->heads);
    free(d->anchors);
    free(d->imgs);
    free(d->lines);
    free(d->page_top);
    free(d->page_off);
    memset(d, 0, sizeof *d);
}

// ---------------------------------------------------------------------------------- the drawing

static int first_op(const doc_t *d, int32_t top) {
    int32_t from = top - d->max_op_h;
    int lo = 0, hi = d->nops;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (d->ops[mid].y0 < from) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static void page_span(const doc_t *d, int k, int32_t *top, int32_t *bot) {
    if (k < 0) k = 0;
    if (k >= d->npages) k = d->npages - 1;
    *top = d->npages ? d->page_top[k] : 0;
    *bot = d->npages ? d->page_top[k + 1] : d->height;
}

void doc_draw_page(const doc_t *d, int k, int x, int y, int focus_link) {
    int32_t top, bot;
    page_span(d, k, &top, &bot);
    const int h = (int)(bot - top);
    gfx_clip(0, y, g_w, y + h);
    for (int i = first_op(d, top); i < d->nops; i++) {
        const op_t *o = &d->ops[i];
        if (o->y0 >= bot) break;
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
            if (o->link >= 0 && o->link == focus_link) gfx_ring(x + o->x - 4, sy - 4, o->w + 8, ih + 8, 6, 2, g_col[C_FOCUS]);
            doc_draw_image(d, (int)o->off, x + o->x, sy, o->w, ih);
        } else if (o->kind == OP_STATIC) {
            text_draw(o->font, o->px, x + o->x, y + (int)(o->base - top), kStatic[o->off], (int)strlen(kStatic[o->off]),
                      g_col[o->color]);
        }
    }
    gfx_noclip();
}

int doc_page_of_y(const doc_t *d, int32_t y) {
    int lo = 0, hi = d->npages - 1;
    while (lo < hi) {   // the last page whose top is <= y
        const int mid = (lo + hi + 1) / 2;
        if (d->page_top[mid] <= y) lo = mid;
        else hi = mid - 1;
    }
    return lo < 0 ? 0 : lo;
}

int doc_page_of_off(const doc_t *d, int32_t off) {
    if (d->npages <= 1) return 0;
    int lo = 0, hi = d->npages - 1;
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (d->page_off[mid] <= off) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

int doc_link_at(const doc_t *d, int k, int px, int py) {
    int32_t top, bot;
    page_span(d, k, &top, &bot);
    const int32_t dy = top + py;
    int best = -1, bestd = 1 << 30;
    for (int i = first_op(d, dy - 24); i < d->nops; i++) {
        const op_t *o = &d->ops[i];
        if (o->y0 > dy + 24 || o->y0 >= bot) break;
        if ((o->kind != OP_TEXT && o->kind != OP_IMAGE) || o->link < 0 || o->y1 <= top) continue;
        const int ex = px < o->x ? o->x - px : px > o->x + o->w ? px - o->x - o->w : 0;
        const int ey = dy < o->y0 ? (int)(o->y0 - dy) : dy >= o->y1 ? (int)(dy - o->y1 + 1) : 0;
        if (ex > 14 || ey > 12) continue;   // fingers are wider than letters (a note's number above all)
        const int dist = ex * ex + ey * ey * 4;
        if (dist < bestd) { bestd = dist; best = o->link; }
    }
    return best;
}

int doc_image_at(const doc_t *d, int k, int px, int py) {
    int32_t top, bot;
    page_span(d, k, &top, &bot);
    const int32_t dy = top + py;
    for (int i = first_op(d, dy); i < d->nops; i++) {
        const op_t *o = &d->ops[i];
        if (o->y0 > dy) break;
        if (o->kind == OP_IMAGE && px >= o->x && px < o->x + o->w && dy >= o->y0 && dy < o->y1) return (int)o->off;
    }
    return -1;
}

int doc_links_on(const doc_t *d, int k, int *out, int max) {
    int32_t top, bot;
    page_span(d, k, &top, &bot);
    int n = 0;
    for (int i = first_op(d, top); i < d->nops && n < max; i++) {
        const op_t *o = &d->ops[i];
        if (o->y0 >= bot) break;
        if ((o->kind != OP_TEXT && o->kind != OP_IMAGE) || o->link < 0 || o->y0 < top) continue;
        int dup = 0;
        for (int j = 0; j < n && !dup; j++) dup = out[j] == o->link;
        if (!dup) out[n++] = o->link;
    }
    return n;
}

int doc_anchor(const doc_t *d, const char *id, int n) {
    for (int i = 0; i < d->nanchors; i++)
        if (d->anchors[i].len == n && !memcmp(d->text + d->anchors[i].off, id, (size_t)n)) return i;
    return -1;
}

char *doc_para_text(const doc_t *d, int32_t off, size_t max) {
    if (off < 0 || (size_t)off >= d->n) return NULL;
    char *o = (char *)malloc(max + 4);
    if (!o) return NULL;
    size_t k = 0;
    int lines = 0, blank = 0, seen = 0;
    for (size_t i = (size_t)off; i < d->n && k + 4 < max; i++) {
        const unsigned char c = (unsigned char)d->text[i];
        if (c == '\n') {
            if (seen) lines++;
            if (i + 1 < d->n && d->text[i + 1] == '\n') blank = 1;
            if (k && o[k - 1] != ' ') o[k++] = ' ';
            // a note is its paragraph(s) up to the next id or heading
            if (i + 1 < d->n && (d->text[i + 1] == RT_ID || (d->text[i + 1] == '#' && seen && lines > 1))) break;
            if (blank && lines >= 3) break;
            continue;
        }
        if (c == RT_ID) {
            if (seen && i != (size_t)off) break;
            while (i < d->n && d->text[i] != RT_ID_END) i++;
            continue;
        }
        if (c == NVHTML_LINK) { while (i < d->n && d->text[i] != NVHTML_LINK_TEXT) i++; continue; }
        if (c == NVHTML_ANCHOR) { while (i < d->n && d->text[i] != '\n') i++; i--; continue; }
        if (c == NVHTML_IMAGE) { while (i < d->n && d->text[i] != NVHTML_IMAGE_END) i++; continue; }
        if (c < 0x20) continue;
        if (c == '#' && (i == 0 || d->text[i - 1] == '\n')) { while (i < d->n && d->text[i] == '#') i++; i--; continue; }
        o[k++] = (char)c;
        seen = 1;
    }
    while (k && o[k - 1] == ' ') k--;
    o[k] = 0;
    return o;
}
