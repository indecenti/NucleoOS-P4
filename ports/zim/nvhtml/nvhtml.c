// nvhtml.c — HTML to plain text; see nvhtml.h. One pass over the bytes with a tag tokenizer (no
// tree): block elements become line or paragraph breaks, lists and tables keep a little state, and
// elements to drop are skipped up to their matching end tag. Sections, the intro, the back matter
// and the length cap are applied afterwards on the text lines.
#include "nvhtml.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ------------------------------------------------------------------------------- output buffer

typedef struct {
    char *p;
    size_t n, cap;
    int oom;
} sb_t;

static void sb_put(sb_t *b, const char *s, size_t n) {
    if (b->oom || !n) return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 1024;
        while (cap < b->n + n + 1) {
            if (cap > SIZE_MAX / 2) { b->oom = 1; return; }
            cap *= 2;
        }
        char *p = (char *)realloc(b->p, cap);
        if (!p) { b->oom = 1; return; }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void sb_str(sb_t *b, const char *s) { sb_put(b, s, strlen(s)); }

// ------------------------------------------------------------------------------------ state

#define LIST_MAX 8

typedef struct {
    sb_t text, box;            // the page; the infoboxes, moved after the lead
    sb_t *out;                 // where text goes now (text, or box inside an infobox)
    int box_tables;            // > 0: nesting of tables inside an infobox
    int cell;                  // inside a table cell (a <br> there is a space)
    int space;                 // a space is due before the next visible character
    int pre;                   // inside <pre>: whitespace kept
    int lists;                 // nesting of ul/ol
    char kind[LIST_MAX];       // 'u' or 'o'
    int num[LIST_MAX];
    int cells;                 // cells written in the current table row
    int skip;                  // > 0: inside an element being dropped (its nesting)
    char skip_tag[16];
} st_t;

static void trim_spaces(st_t *s) {
    while (s->out->n && s->out->p[s->out->n - 1] == ' ') s->out->n--;
    if (s->out->p) s->out->p[s->out->n] = 0;
}

static void brk(st_t *s, int lines) {
    trim_spaces(s);
    s->space = 0;
    if (!s->out->n) return;   // no blank lines at the top
    int k = 0;
    while (k < (int)s->out->n && s->out->p[s->out->n - 1 - k] == '\n') k++;
    for (; k < lines; k++) sb_put(s->out, "\n", 1);
}

static int at_line_start(const st_t *s) { return !s->out->n || s->out->p[s->out->n - 1] == '\n'; }

static void put_text(st_t *s, const char *t, size_t n) {
    for (size_t i = 0; i < n; i++) {
        const char c = t[i];
        // control characters (a NUL would cut the text short) are not text
        if ((unsigned char)c < 0x20 && c != '\t' && c != '\n' && c != '\r' && c != '\f') continue;
        if (c == 0x7F) continue;
        if (s->pre) {
            if (c == '\r') continue;
            if (c == '\n') { trim_spaces(s); sb_put(s->out, "\n", 1); continue; }
            sb_put(s->out, &c, 1);
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') { s->space = 1; continue; }
        if (s->space && !at_line_start(s) && s->out->p[s->out->n - 1] != ' ') sb_put(s->out, " ", 1);
        s->space = 0;
        sb_put(s->out, &c, 1);
    }
}

// --------------------------------------------------------------------------- character references

static size_t utf8(uint32_t cp, char *o) {
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | cp >> 6); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | cp >> 12); o[1] = (char)(0x80 | (cp >> 6 & 0x3F)); o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | cp >> 18); o[1] = (char)(0x80 | (cp >> 12 & 0x3F));
    o[2] = (char)(0x80 | (cp >> 6 & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static const struct { const char *name; uint32_t cp; } kEntities[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", ' '},
    {"ensp", ' '}, {"emsp", ' '}, {"thinsp", ' '}, {"shy", 0}, {"zwj", 0}, {"zwnj", 0}, {"lrm", 0}, {"rlm", 0},
    {"ndash", 0x2013}, {"mdash", 0x2014}, {"minus", 0x2212}, {"hellip", 0x2026}, {"laquo", 0xAB},
    {"raquo", 0xBB}, {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C}, {"rdquo", 0x201D},
    {"sbquo", 0x201A}, {"bdquo", 0x201E}, {"bull", 0x2022}, {"middot", 0xB7}, {"deg", 0xB0},
    {"copy", 0xA9}, {"reg", 0xAE}, {"trade", 0x2122}, {"euro", 0x20AC}, {"pound", 0xA3}, {"yen", 0xA5},
    {"cent", 0xA2}, {"sect", 0xA7}, {"para", 0xB6}, {"times", 0xD7}, {"divide", 0xF7}, {"plusmn", 0xB1},
    {"le", 0x2264}, {"ge", 0x2265}, {"ne", 0x2260}, {"asymp", 0x2248}, {"infin", 0x221E}, {"micro", 0xB5},
    {"frac12", 0xBD}, {"frac14", 0xBC}, {"frac34", 0xBE}, {"sup1", 0xB9}, {"sup2", 0xB2}, {"sup3", 0xB3},
    {"iexcl", 0xA1}, {"iquest", 0xBF}, {"larr", 0x2190}, {"rarr", 0x2192}, {"uarr", 0x2191}, {"darr", 0x2193},
    {"harr", 0x2194}, {"rArr", 0x21D2}, {"hArr", 0x21D4}, {"dagger", 0x2020}, {"Dagger", 0x2021},
    {"prime", 0x2032}, {"Prime", 0x2033}, {"alpha", 0x3B1}, {"beta", 0x3B2}, {"gamma", 0x3B3},
    {"delta", 0x3B4}, {"mu", 0x3BC}, {"pi", 0x3C0}, {"sigma", 0x3C3}, {"omega", 0x3C9}, {"Omega", 0x3A9},
    {"agrave", 0xE0}, {"egrave", 0xE8}, {"eacute", 0xE9}, {"igrave", 0xEC}, {"ograve", 0xF2}, {"ugrave", 0xF9},
    {"Agrave", 0xC0}, {"Egrave", 0xC8}, {"Eacute", 0xC9}, {"auml", 0xE4}, {"ouml", 0xF6}, {"uuml", 0xFC},
    {"Auml", 0xC4}, {"Ouml", 0xD6}, {"Uuml", 0xDC}, {"szlig", 0xDF}, {"ccedil", 0xE7}, {"ntilde", 0xF1},
};

// Decodes the reference at t[0] == '&'. Writes its UTF-8 to o (up to 4 bytes) and returns the bytes
// it spans in t, or 0 when it is not a reference (the '&' is then literal).
static size_t entity(const char *t, size_t n, char *o, size_t *olen) {
    *olen = 0;
    if (n < 3) return 0;
    size_t i = 1;
    if (t[1] == '#') {
        uint32_t cp = 0;
        int hex = i + 1 < n && (t[2] == 'x' || t[2] == 'X');
        i = hex ? 3 : 2;
        const size_t start = i;
        for (; i < n && i < start + 8; i++) {
            const char c = t[i];
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (hex && c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (hex && c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            cp = cp * (hex ? 16 : 10) + (uint32_t)d;
        }
        if (i == start || i >= n || t[i] != ';') return 0;
        if (cp == 0xA0 || cp == 0x2002 || cp == 0x2003 || cp == 0x2009) cp = ' ';
        if (cp == 0xAD || cp == 0x200B || cp == 0x200C || cp == 0x200D) return i + 1;   // invisible
        *olen = utf8(cp, o);
        return i + 1;
    }
    for (; i < n && i < 10 && ((t[i] >= 'a' && t[i] <= 'z') || (t[i] >= 'A' && t[i] <= 'Z') || (t[i] >= '0' && t[i] <= '9')); i++) {}
    if (i >= n || t[i] != ';' || i == 1) return 0;
    for (size_t k = 0; k < sizeof kEntities / sizeof kEntities[0]; k++)
        if (strlen(kEntities[k].name) == i - 1 && !memcmp(kEntities[k].name, t + 1, i - 1)) {
            if (kEntities[k].cp) *olen = utf8(kEntities[k].cp, o);
            return i + 1;
        }
    return 0;
}

// Text with references decoded.
static void put_decoded(st_t *s, const char *t, size_t n) {
    if (!n) return;   // t may be NULL then (an attribute without a value)
    size_t from = 0;
    for (size_t i = 0; i < n; i++) {
        if (t[i] != '&') continue;
        char o[4];
        size_t ol;
        const size_t used = entity(t + i, n - i, o, &ol);
        if (!used) continue;
        put_text(s, t + from, i - from);
        if (ol == 1 && o[0] == ' ') {   // nbsp and friends: a real space, not collapsed away
            if (!at_line_start(s)) sb_put(s->out, " ", 1);
            s->space = 0;
        } else {
            put_text(s, o, ol);
        }
        i += used - 1;
        from = i + 1;
    }
    put_text(s, t + from, n - from);
}

// ----------------------------------------------------------------------------------- tags

typedef struct {
    const char *name, *val;
    size_t nlen, vlen;
} attr_t;

#define ATTR_MAX 24

typedef struct {
    char name[16];
    int end, self_close;
    attr_t a[ATTR_MAX];
    int na;
} tag_t;

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static const attr_t *attr(const tag_t *t, const char *name) {
    const size_t l = strlen(name);
    for (int i = 0; i < t->na; i++) {
        if (t->a[i].nlen != l) continue;
        size_t k = 0;
        while (k < l && lower((unsigned char)t->a[i].name[k]) == name[k]) k++;
        if (k == l) return &t->a[i];
    }
    return NULL;
}

static int has_word(const attr_t *a, const char *word) {
    if (!a) return 0;
    const size_t l = strlen(word);
    for (size_t i = 0; i + l <= a->vlen; i++)
        if (!memcmp(a->val + i, word, l) && (i == 0 || a->val[i - 1] == ' ') &&
            (i + l == a->vlen || a->val[i + l] == ' '))
            return 1;
    return 0;
}

// style="display: none" (spaces and case as they come)
static int hidden_style(const attr_t *a) {
    if (!a) return 0;
    static const char kWant[] = "display:none";
    size_t k = 0;
    for (size_t i = 0; i < a->vlen; i++) {
        const char c = (char)lower((unsigned char)a->val[i]);
        if (c == ' ' || c == '\t') continue;
        if (c == kWant[k]) {
            if (!kWant[++k]) return 1;
        } else {
            k = c == kWant[0] ? 1 : 0;
        }
    }
    return 0;
}

static int contains(const attr_t *a, const char *needle) {
    if (!a) return 0;
    const size_t l = strlen(needle);
    for (size_t i = 0; i + l <= a->vlen; i++) {
        size_t k = 0;
        while (k < l && lower((unsigned char)a->val[i + k]) == needle[k]) k++;
        if (k == l) return 1;
    }
    return 0;
}

// Parses the tag at h[0] == '<'. Returns its length (up to and including '>'), or 0 if this '<'
// starts no tag. Comments, doctypes and processing instructions come back with name "!".
static size_t parse_tag(const char *h, size_t n, tag_t *t) {
    memset(t, 0, sizeof *t);
    size_t i = 1;
    if (i < n && (h[i] == '!' || h[i] == '?')) {
        t->name[0] = '!';
        if (n >= 4 && !memcmp(h, "<!--", 4)) {
            for (i = 4; i + 2 < n; i++)
                if (h[i] == '-' && h[i + 1] == '-' && h[i + 2] == '>') return i + 3;
            return n;
        }
        const char *e = (const char *)memchr(h, '>', n);
        return e ? (size_t)(e - h) + 1 : n;
    }
    if (i < n && h[i] == '/') { t->end = 1; i++; }
    if (i >= n || !((h[i] >= 'a' && h[i] <= 'z') || (h[i] >= 'A' && h[i] <= 'Z'))) return 0;
    size_t k = 0;
    for (; i < n && ((h[i] >= 'a' && h[i] <= 'z') || (h[i] >= 'A' && h[i] <= 'Z') || (h[i] >= '0' && h[i] <= '9') ||
                     h[i] == '-' || h[i] == ':'); i++)
        if (k + 1 < sizeof t->name) t->name[k++] = (char)lower((unsigned char)h[i]);
    t->name[k] = 0;
    while (i < n) {
        while (i < n && (h[i] == ' ' || h[i] == '\t' || h[i] == '\n' || h[i] == '\r' || h[i] == '\f')) i++;
        if (i >= n) break;
        if (h[i] == '>') return i + 1;
        if (h[i] == '/') {
            if (i + 1 < n && h[i + 1] == '>') { t->self_close = 1; return i + 2; }
            i++;
            continue;
        }
        const size_t ns = i;
        while (i < n && h[i] != '=' && h[i] != '>' && h[i] != ' ' && h[i] != '\t' && h[i] != '\n' &&
               h[i] != '\r' && h[i] != '/')
            i++;
        attr_t a = {h + ns, NULL, i - ns, 0};
        if (i == ns) { i++; continue; }   // a stray character
        while (i < n && (h[i] == ' ' || h[i] == '\t' || h[i] == '\n' || h[i] == '\r')) i++;
        if (i < n && h[i] == '=') {
            i++;
            while (i < n && (h[i] == ' ' || h[i] == '\t' || h[i] == '\n' || h[i] == '\r')) i++;
            if (i < n && (h[i] == '"' || h[i] == '\'')) {
                const char q = h[i++];
                const size_t vs = i;
                while (i < n && h[i] != q) i++;
                a.val = h + vs;
                a.vlen = i - vs;
                if (i < n) i++;
            } else {
                const size_t vs = i;
                while (i < n && h[i] != '>' && h[i] != ' ' && h[i] != '\t' && h[i] != '\n' && h[i] != '\r') i++;
                a.val = h + vs;
                a.vlen = i - vs;
            }
        }
        if (t->na < ATTR_MAX) t->a[t->na++] = a;
    }
    return n;   // an unterminated tag runs to the end of the input
}

static int is(const char *name, const char *const *list) {
    for (; *list; list++)
        if (!strcmp(name, *list)) return 1;
    return 0;
}

static const char *const kVoid[] = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta",
                                    "param", "source", "track", "wbr", NULL};
static const char *const kRaw[] = {"script", "style", "textarea", "title", NULL};   // content is not HTML
static const char *const kDrop[] = {"head", "nav", "footer", "button", "select", "svg", "object", "iframe",
                                    "audio", "video", "map", "noscript", "template", "form", NULL};
// classes of MediaWiki (and a few other generators) that carry no article text
static const char *const kDropClass[] = {"sidebar", "vertical-navbox", "reference", "mw-ref", "mw-editsection", "noprint", "navbox", "navbox-styles",
                                         "metadata", "catlinks", "mw-references-wrap", "references", "reflist",
                                         "refbegin", "mw-cite-backlink", "toc", "zim-footer", "mw-jump-link",
                                         "printfooter", "sistersitebox", "ambox", "mw-empty-elt", "mwe-math-mathml-a11y",
                                         "mw-hidden-catlinks", "noviewer", NULL};
static const char *const kPara[] = {"p", "blockquote", "figure", "figcaption", "dl", "address", "center", "table",
                                    NULL};
static const char *const kLine[] = {"div", "section", "article", "main", "header", "aside", "details", "summary",
                                    "dt", "tr", "caption", "fieldset", "legend", NULL};

// The infoboxes of the lead (MediaWiki's "infobox", "sinottico", ...) are kept aside and come
// after the lead's prose: someone asking what a thing is wants the sentence before the table.
static void flush_box(st_t *s) {
    if (!s->box.n || s->out != &s->text) return;
    brk(s, 2);
    sb_put(&s->text, s->box.p, s->box.n);
    s->box.n = 0;
    s->box.p[0] = 0;
    brk(s, 2);
}

static int is_infobox(const attr_t *cls) {
    static const char *const kBox[] = {"infobox", "sinottico", "vcard", "taxobox", "biota", "infobox_v2", NULL};
    for (const char *const *b = kBox; *b; b++)
        if (has_word(cls, *b)) return 1;
    return 0;
}

static void tex(st_t *s, const char *v, size_t n) {   // MediaWiki: alttext="{\displaystyle x^2}"
    static const char kDs[] = "{\\displaystyle ";
    if (n > sizeof kDs - 1 && !memcmp(v, kDs, sizeof kDs - 1) && v[n - 1] == '}') {
        v += sizeof kDs - 1;
        n -= sizeof kDs;
    }
    put_text(s, " ", 1);
    put_decoded(s, v, n);
}

static void start_skip(st_t *s, const char *name) {
    s->skip = 1;
    snprintf(s->skip_tag, sizeof s->skip_tag, "%s", name);
}

static void on_start(st_t *s, const tag_t *t) {
    const char *nm = t->name;
    const attr_t *cls = attr(t, "class");
    const int is_void = is(nm, kVoid);
    if (!is_void && !t->self_close) {
        int drop = is(nm, kDrop) || attr(t, "hidden") || hidden_style(attr(t, "style"));
        for (const char *const *c = kDropClass; !drop && *c; c++) drop = has_word(cls, *c);
        if (!strcmp(nm, "math")) {
            const attr_t *alt = attr(t, "alttext");
            if (alt) tex(s, alt->val, alt->vlen);
            drop = 1;
        }
        if (drop) { start_skip(s, nm); return; }
    }
    if (!strcmp(nm, "img")) {
        const attr_t *alt = attr(t, "alt");
        if (alt && (has_word(cls, "mwe-math-fallback-image-inline") || has_word(cls, "mwe-math-fallback-image-display") ||
                    contains(cls, "tex")))
            tex(s, alt->val, alt->vlen);
        return;
    }
    if (!strcmp(nm, "br")) {
        if (s->pre) sb_put(s->out, "\n", 1);
        else if (s->cell) s->space = 1;   // a line break inside a table cell
        else brk(s, 1);
        return;
    }
    if (!strcmp(nm, "hr")) { brk(s, 2); return; }
    if (nm[0] == 'h' && nm[1] >= '1' && nm[1] <= '6' && !nm[2]) {
        if (nm[1] >= '2') flush_box(s);   // the lead ends at the first section
        brk(s, 2);
        static const char kHashes[] = "######";
        sb_put(s->out, kHashes, (size_t)(nm[1] - '0'));
        sb_put(s->out, " ", 1);
        return;
    }
    if (!strcmp(nm, "ul") || !strcmp(nm, "ol")) {
        brk(s, s->lists ? 1 : 2);
        if (s->lists < LIST_MAX) {
            s->kind[s->lists] = nm[0];
            s->num[s->lists] = 1;
        }
        s->lists++;
        return;
    }
    if (!strcmp(nm, "li")) {
        brk(s, 1);
        const int d = s->lists > 0 ? s->lists - 1 : 0;
        for (int i = 0; i < d && i < 6; i++) sb_str(s->out, "  ");
        if (s->lists > 0 && s->lists <= LIST_MAX && s->kind[s->lists - 1] == 'o') {
            char b[16];
            snprintf(b, sizeof b, "%d. ", s->num[s->lists - 1]++);
            sb_str(s->out, b);
        } else {
            sb_str(s->out, "- ");
        }
        return;
    }
    if (!strcmp(nm, "dd")) { brk(s, 1); sb_str(s->out, "  "); return; }
    if (!strcmp(nm, "td") || !strcmp(nm, "th")) {
        if (s->cells++) {
            trim_spaces(s);
            if (!at_line_start(s)) sb_str(s->out, " | ");
        }
        s->space = 0;
        s->cell = 1;
        return;
    }
    if (!strcmp(nm, "tr")) { brk(s, 1); s->cells = s->cell = 0; return; }
    if (!strcmp(nm, "table")) {
        if (s->box_tables) s->box_tables++;
        else if (is_infobox(cls)) { s->box_tables = 1; s->out = &s->box; }
        brk(s, 2);
        return;
    }
    if (!strcmp(nm, "pre")) { brk(s, 2); s->pre = 1; return; }
    if (is(nm, kPara)) { brk(s, 2); return; }
    if (is(nm, kLine)) { brk(s, 1); return; }
}

static void on_end(st_t *s, const tag_t *t) {
    const char *nm = t->name;
    if (nm[0] == 'h' && nm[1] >= '1' && nm[1] <= '6' && !nm[2]) { brk(s, 2); return; }
    if (!strcmp(nm, "ul") || !strcmp(nm, "ol")) {
        if (s->lists > 0) s->lists--;
        brk(s, s->lists ? 1 : 2);
        return;
    }
    if (!strcmp(nm, "pre")) { s->pre = 0; brk(s, 2); return; }
    if (!strcmp(nm, "table")) {
        s->cells = s->cell = 0;
        brk(s, 2);
        if (s->box_tables && --s->box_tables == 0) s->out = &s->text;
        return;
    }
    if (!strcmp(nm, "td") || !strcmp(nm, "th")) { s->cell = 0; return; }
    if (is(nm, kPara)) { brk(s, 2); return; }
    if (is(nm, kLine) || !strcmp(nm, "li") || !strcmp(nm, "dd")) { brk(s, 1); return; }
}

// Index just past the end tag </name> (case-insensitive) from i, or n.
static size_t past_end_tag(const char *h, size_t n, size_t i, const char *name) {
    const size_t l = strlen(name);
    for (; i + 2 + l <= n; i++) {
        if (h[i] != '<' || h[i + 1] != '/') continue;
        size_t k = 0;
        while (k < l && lower((unsigned char)h[i + 2 + k]) == name[k]) k++;
        if (k < l) continue;
        const char *e = (const char *)memchr(h + i, '>', n - i);
        return e ? (size_t)(e - h) + 1 : n;
    }
    return n;
}

static void convert(st_t *s, const char *h, size_t n) {
    size_t i = 0, text_from = 0;
    while (i < n && !s->text.oom && !s->box.oom) {
        if (h[i] != '<') { i++; continue; }
        tag_t t;
        const size_t len = parse_tag(h + i, n - i, &t);
        if (!len) { i++; continue; }   // a literal '<'
        if (!s->skip) put_decoded(s, h + text_from, i - text_from);
        i += len;
        text_from = i;
        if (t.name[0] == '!') continue;
        if (s->skip) {   // dropping an element: only its own start and end tags count
            if (!strcmp(t.name, s->skip_tag) && !t.self_close && !is(t.name, kVoid)) s->skip += t.end ? -1 : 1;
            continue;
        }
        if (!t.end && is(t.name, kRaw)) {   // script, style...: skip the content as text
            i = past_end_tag(h, n, i, t.name);
            text_from = i;
            continue;
        }
        if (t.end) on_end(s, &t);
        else {
            on_start(s, &t);
            if (t.self_close && !s->skip) on_end(s, &t);   // XHTML <p/>
        }
    }
    if (!s->skip) put_decoded(s, h + text_from, n - text_from);
}

// -------------------------------------------------------------------------- lines and sections

static int heading_level(const char *l, size_t n) {
    size_t k = 0;
    while (k < n && k < 7 && l[k] == '#') k++;
    return k >= 1 && k <= 6 && k < n && l[k] == ' ' ? (int)k : 0;
}

// Case-insensitive (ASCII and Latin-1 letters in UTF-8) comparison of a heading with a name:
// mode 0 = equal, 1 = the heading starts with the name.
static int fold(const unsigned char **p, const unsigned char *end) {
    const unsigned char *q = *p;
    if (q >= end) return -1;
    if (q[0] == 0xC3 && q + 1 < end && q[1] >= 0x80 && q[1] <= 0x9E && q[1] != 0x97) { *p += 2; return 0xC300 | (q[1] + 0x20); }
    if (q[0] == 0xC3 && q + 1 < end) { *p += 2; return 0xC300 | q[1]; }
    *p += 1;
    return lower(q[0]);
}

static int name_match(const char *h, size_t hn, const char *name, int prefix) {
    const unsigned char *a = (const unsigned char *)h, *ae = a + hn;
    const unsigned char *b = (const unsigned char *)name, *be = b + strlen(name);
    while (b < be) {
        if (a >= ae) return 0;
        if (fold(&a, ae) != fold(&b, be)) return 0;
    }
    return prefix || a >= ae;
}

static const char *const kBackMatter[] = {
    "note", "bibliografia", "collegamenti esterni", "voci correlate", "altri progetti", "fonti",
    "notes", "references", "external links", "see also", "further reading", "bibliography", "sources",
    "notas", "referencias", "enlaces externos", "véase también", "bibliografía",
    "notes et références", "références", "liens externes", "voir aussi", "bibliographie",
    "einzelnachweise", "weblinks", "literatur", "siehe auch", "anmerkungen", NULL};

typedef struct { const char *p; size_t n; int level; } line_t;

static char *finish(sb_t *text, const nvhtml_opts_t *o, size_t *out_len) {
    static const char kEmpty[] = "";
    const char *tp = text->p ? text->p : kEmpty;   // nothing was written: no buffer at all
    // split into lines
    size_t nl = 1;
    for (size_t i = 0; i < text->n; i++) nl += tp[i] == '\n';
    line_t *L = (line_t *)malloc(sizeof(line_t) * nl);
    if (!L) return NULL;
    size_t k = 0, from = 0;
    for (size_t i = 0; i <= text->n; i++)
        if (i == text->n || tp[i] == '\n') {
            L[k].p = tp + from;
            L[k].n = i - from;
            L[k].level = heading_level(L[k].p, L[k].n);
            k++;
            from = i + 1;
        }
    nl = k;
    char *keep = (char *)calloc(nl ? nl : 1, 1);
    if (!keep) { free(L); return NULL; }
    memset(keep, 1, nl);
    if (!o->back_matter && !o->section) {   // drop Notes, References... (level-2 sections)
        for (size_t i = 0; i < nl; i++) {
            if (L[i].level != 2) continue;
            int bm = 0;
            for (const char *const *b = kBackMatter; !bm && *b; b++) bm = name_match(L[i].p + 3, L[i].n - 3, *b, 0);
            if (!bm) continue;
            size_t j = i;
            do keep[j++] = 0;
            while (j < nl && !(L[j].level && L[j].level <= 2));
            i = j - 1;
        }
    }
    if (o->no_title) {
        for (size_t i = 0; i < nl; i++) {
            if (L[i].level == 1) { keep[i] = 0; break; }
            if (L[i].n) break;
        }
    }
    if (o->intro) {
        for (size_t i = 0; i < nl; i++)
            if (L[i].level >= 2) { memset(keep + i, 0, nl - i); break; }
    } else if (o->section) {
        size_t a = nl, b = nl;
        for (size_t i = 0; i < nl && a == nl; i++)
            if (L[i].level >= 2 && name_match(L[i].p + L[i].level + 1, L[i].n - (size_t)L[i].level - 1, o->section, 1)) a = i;
        if (a < nl) {
            for (b = a + 1; b < nl && !(L[b].level && L[b].level <= L[a].level); b++) {}
        }
        for (size_t i = 0; i < nl; i++) if (i < a || i >= b) keep[i] = 0;
    }
    sb_t out = {0};
    int blank = 1;   // no blank lines at the top, never two in a row
    for (size_t i = 0; i < nl; i++) {
        if (!keep[i]) continue;
        if (!L[i].n) {
            if (!blank) sb_put(&out, "\n", 1);
            blank = 1;
            continue;
        }
        sb_put(&out, L[i].p, L[i].n);
        sb_put(&out, "\n", 1);
        blank = 0;
    }
    free(L);
    free(keep);
    while (out.n && (out.p[out.n - 1] == '\n' || out.p[out.n - 1] == ' ')) out.p[--out.n] = 0;
    if (o->max && out.n > o->max) {
        size_t cut = o->max;
        while (cut > 0 && ((unsigned char)out.p[cut] & 0xC0) == 0x80) cut--;   // a UTF-8 boundary
        size_t best = 0;
        for (size_t i = cut; i > cut * 2 / 3 && !best; i--)
            if (out.p[i - 1] == '\n' && i >= 2 && out.p[i - 2] == '\n') best = i - 2;
        for (size_t i = cut; i > cut * 2 / 3 && !best; i--)
            if (out.p[i - 1] == '.' && (out.p[i] == ' ' || out.p[i] == '\n')) best = i;
        out.n = best ? best : cut;
        out.p[out.n] = 0;
        sb_str(&out, " [\xE2\x80\xA6]");
    }
    if (out.oom) { free(out.p); return NULL; }
    if (!out.p) {
        out.p = (char *)malloc(1);
        if (!out.p) return NULL;
        out.p[0] = 0;
    }
    if (out_len) *out_len = out.n;
    return out.p;
}

char *nvhtml_text(const char *html, size_t n, const nvhtml_opts_t *opts, size_t *out_len) {
    static const nvhtml_opts_t kDefault = {0};
    const nvhtml_opts_t *o = opts ? opts : &kDefault;
    st_t s;
    memset(&s, 0, sizeof s);
    s.out = &s.text;
    convert(&s, html, n);
    s.out = &s.text;   // an infobox left open by broken HTML
    flush_box(&s);
    char *r = s.text.oom || s.box.oom ? NULL : finish(&s.text, o, out_len);
    free(s.text.p);
    free(s.box.p);
    return r;
}

char *nvhtml_sections(const char *html, size_t n, size_t *out_len) {
    nvhtml_opts_t o = {0};
    o.back_matter = 1;
    size_t len;
    char *t = nvhtml_text(html, n, &o, &len);
    if (!t) return NULL;
    sb_t out = {0};
    for (size_t i = 0; i < len;) {
        const char *e = (const char *)memchr(t + i, '\n', len - i);
        const size_t l = e ? (size_t)(e - (t + i)) : len - i;
        if (heading_level(t + i, l) >= 2) {
            sb_put(&out, t + i, l);
            sb_put(&out, "\n", 1);
        }
        i += l + 1;
    }
    free(t);
    if (out.oom) { free(out.p); return NULL; }
    if (!out.p && !(out.p = (char *)calloc(1, 1))) return NULL;
    if (out_len) *out_len = out.n;
    return out.p;
}

char *nvhtml_latin1_to_utf8(const char *s, size_t n, size_t *out_len) {
    char *o = (char *)malloc(n * 2 + 1);
    if (!o) return NULL;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x80) o[k++] = (char)c;
        else { o[k++] = (char)(0xC0 | c >> 6); o[k++] = (char)(0x80 | (c & 0x3F)); }
    }
    o[k] = 0;
    if (out_len) *out_len = k;
    return o;
}
