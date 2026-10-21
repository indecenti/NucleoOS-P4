// book.c — a book file as chapters of rich text (bk.h, "rich text"):
//   EPUB 2 and 3: container.xml -> the OPF package (metadata, manifest, spine, cover), the table of
//     contents from the EPUB 3 nav document or the NCX; each spine item is a chapter, its XHTML
//     turned into rich text by nvhtml. Fixed-layout books (rendition:layout pre-paginated) get one
//     page per spine item, the item's picture fitted to the screen. DRM-encrypted books are refused.
//   CBZ: every picture of the archive, in natural order, a page of its own (ComicInfo.xml metadata).
//   FB2 (and .fb2.zip), TXT, HTML, Markdown: converted whole (conv.c), cut into chapters at the
//     headings of the first levels (and at paragraph ends when a chapter grows too long).
// Every file is untrusted: lengths are checked, a damaged member makes a chapter empty, not a crash.
#define _GNU_SOURCE   // strcasestr
#include "bk.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "nvhtml.h"

#define MEMBER_MAX (24u << 20)   // a chapter or picture larger than this is not read
#define WHOLE_MAX (48u << 20)    // a whole-text file larger than this is refused
#define CHAP_SPLIT 96000u        // whole-text formats: a chapter longer than this is cut
#define EPUB_SPLIT (256u << 10)  // an EPUB member larger than this is read as parts of about this size
#define TOC_MAX 4000

typedef struct bin_s { char id[64]; size_t off, len; } bin_t;   // FB2 pictures: base64 in b->src

// ------------------------------------------------------------------------------------ helpers

static char *dupn(const char *s, size_t n) {
    char *d = (char *)malloc(n + 1);
    if (!d) return NULL;
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

static int ends_with(const char *s, const char *suf) {
    const size_t a = strlen(s), b = strlen(suf);
    return a >= b && !strcasecmp(s + a - b, suf);
}

int book_kind_of(const char *path) {
    if (ends_with(path, ".epub") || ends_with(path, ".kepub") || ends_with(path, ".epub3")) return BK_EPUB;
    if (ends_with(path, ".fb2") || ends_with(path, ".fb2.zip") || ends_with(path, ".fbz")) return BK_FB2;
    if (ends_with(path, ".cbz")) return BK_CBZ;
    if (ends_with(path, ".txt")) return BK_TXT;
    if (ends_with(path, ".html") || ends_with(path, ".htm") || ends_with(path, ".xhtml")) return BK_HTML;
    if (ends_with(path, ".md") || ends_with(path, ".markdown")) return BK_MD;
    return -1;
}

const char *book_kind_name(int kind) {
    static const char *const k[BK_KINDS] = {"EPUB", "FB2", "CBZ", "TXT", "HTML", "Markdown"};
    return kind >= 0 && kind < BK_KINDS ? k[kind] : "?";
}

static int hexv(int c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

// base (a member's path) + href -> a member's path, normalised ("a/b/../c" -> "a/c"), %XX decoded,
// the fragment cut off into frag (may be NULL). Returns the length, -1 when it leaves the archive.
static int resolve(const char *base, const char *href, int hn, char *out, size_t cap, char *frag, size_t fcap) {
    if (frag && fcap) frag[0] = 0;
    char tmp[600];
    size_t k = 0;
    if (hn > 0 && href[0] != '/') {   // relative: from the base's folder
        const char *slash = base ? strrchr(base, '/') : NULL;
        if (slash) {
            const size_t bl = (size_t)(slash + 1 - base);
            if (bl >= sizeof tmp) return -1;
            memcpy(tmp, base, bl);
            k = bl;
        }
    }
    int i = hn > 0 && href[0] == '/' ? 1 : 0;
    for (; i < hn && href[i] != '#' && href[i] != '?' && k + 1 < sizeof tmp; i++) {
        int c = (unsigned char)href[i];
        if (c == '%' && i + 2 < hn && hexv(href[i + 1]) >= 0 && hexv(href[i + 2]) >= 0) {
            c = hexv(href[i + 1]) * 16 + hexv(href[i + 2]);
            i += 2;
        }
        if (!c) return -1;
        tmp[k++] = (char)c;
    }
    tmp[k] = 0;
    if (frag && fcap) {
        const char *h = memchr(href, '#', (size_t)(hn > 0 ? hn : 0));
        if (h) {
            const int fl = (int)(href + hn - (h + 1));
            snprintf(frag, fcap, "%.*s", fl, h + 1);
        }
    }
    // normalise the segments
    char *seg[64];
    int ns = 0;
    char *p = tmp;
    while (p && *p) {
        char *s = strchr(p, '/');
        if (s) *s = 0;
        if (!strcmp(p, "..")) { if (ns) ns--; else return -1; }
        else if (*p && strcmp(p, ".")) { if (ns == 64) return -1; seg[ns++] = p; }
        p = s ? s + 1 : NULL;
    }
    size_t o = 0;
    for (int j = 0; j < ns; j++) {
        const size_t l = strlen(seg[j]);
        if (o + l + 2 > cap) return -1;
        if (j) out[o++] = '/';
        memcpy(out + o, seg[j], l);
        o += l;
    }
    if (cap) out[o] = 0;
    return (int)o;
}

static int toc_add(book_t *b, const char *title, int chap, const char *anchor, int level) {
    if (b->ntoc >= TOC_MAX) return -1;
    if (!(b->ntoc & 63)) {
        tocent_t *t = (tocent_t *)realloc(b->toc, sizeof(tocent_t) * (size_t)(b->ntoc + 64));
        if (!t) return -1;
        b->toc = t;
    }
    tocent_t *e = &b->toc[b->ntoc];
    e->title = strdup(title && *title ? title : "?");
    e->anchor = strdup(anchor ? anchor : "");
    if (!e->title || !e->anchor) { free(e->title); free(e->anchor); return -1; }
    e->chap = chap;
    e->level = (uint8_t)(level < 0 ? 0 : level > 6 ? 6 : level);
    b->ntoc++;
    return 0;
}

// A chapter's label ("CHAPTER II", "Capitolo 3", "XIV."): how a document that the contents skip
// names itself in its first line.
static int chapter_label(const char *s) {
    static const char *const kWords[] = {"chapter", "capitolo", "kapitel", "chapitre", "cap\xC3\xADtulo", "capitulo",
                                         "hoofdstuk", "kapitola", "book", "part", "parte", "libro", "livre", "buch",
                                         "teil", "canto", "act", "atto", "acte", "akt", "letter", "lettera", "lettre",
                                         "brief", "stave", NULL};
    for (const char *const *w = kWords; *w; w++) {
        const size_t n = strlen(*w);
        if (!strncasecmp(s, *w, n) && (s[n] == ' ' || s[n] == '.')) return 1;
    }
    const char *p = s;   // a numeral alone, a dot after it perhaps
    while (*p && strchr("IVXLC", *p)) p++;
    if (p == s) while (*p >= '0' && *p <= '9') p++;
    return p > s && p - s < 8 && (!*p || (*p == '.' && !p[1]));
}

// The first two blocks of text of an XHTML document, entities decoded and spaces folded.
static void first_blocks(const char *s, size_t n, char *a, size_t ac, char *b2, size_t bc) {
    static const char *const kBlock[] = {"div", "p", "h1", "h2", "h3", "h4", "h5", "h6", "br", "li", "td", "tr",
                                         "section", "header", "table", "blockquote", "hr", "img", NULL};
    a[0] = b2[0] = 0;
    size_t i = 0;
    for (; i + 5 < n; i++)
        if (s[i] == '<' && !strncasecmp(s + i + 1, "body", 4)) break;
    int got = 0;
    char buf[400];   // the current block's text, its inline tags left out
    size_t bn = 0;
    while (i < n && got < 2) {
        if (s[i] == '<') {
            size_t j = i + 1;
            if (j < n && s[j] == '/') j++;
            size_t k = j;
            while (k < n && ((s[k] >= 'a' && s[k] <= 'z') || (s[k] >= 'A' && s[k] <= 'Z') || (s[k] >= '0' && s[k] <= '9'))) k++;
            int block = 0;
            for (const char *const *t = kBlock; *t && !block; t++)
                block = strlen(*t) == k - j && !strncasecmp(s + j, *t, k - j);
            while (k < n && s[k] != '>') k++;
            i = k + 1;
            if (block && bn) {
                char out[200];
                xml_text(buf, (int)bn, out, sizeof out);
                if (out[0]) {
                    if (got == 0) snprintf(a, ac, "%s", out);
                    else snprintf(b2, bc, "%s", out);
                    got++;
                }
                bn = 0;
            }
            continue;
        }
        if (bn + 1 < sizeof buf) buf[bn++] = s[i];
        i++;
    }
}

// Documents of the spine that the contents leave out but that open with a chapter's label get their
// own entry (old Gutenberg books list only a few landmarks): the contents and the running head name
// every chapter.
static void toc_fill(book_t *b) {
    if (b->fixed || b->nch < 2 || b->nch > 600 || !b->zip) return;
    uint8_t *has = (uint8_t *)calloc((size_t)b->nch, 1);
    tocent_t *add = (tocent_t *)calloc((size_t)b->nch, sizeof(tocent_t));
    if (!has || !add) { free(has); free(add); return; }
    for (int i = 0; i < b->ntoc; i++)
        if (b->toc[i].chap >= 0 && b->toc[i].chap < b->nch) has[b->toc[i].chap] = 1;
    int nadd = 0, looked = 0;
    for (int c = 0; c < b->nch && looked < 200; c++) {
        const chap_t *ch = &b->ch[c];
        if (has[c] || !ch->path || ch->part || !ch->linear) continue;
        const int zi = zip_find(b->zip, ch->path);
        uint8_t *d;
        size_t dn;
        looked++;
        if (zi < 0 || zip_read_head(b->zip, zi, &d, &dn, 6144)) continue;
        char l1[120], l2[120];
        first_blocks((const char *)d, dn, l1, sizeof l1, l2, sizeof l2);
        free(d);
        if (!l1[0] || strlen(l1) > 60 || !chapter_label(l1)) continue;
        char title[200];
        const size_t n2 = strlen(l2);
        if (n2 && n2 <= 60 && !chapter_label(l2) && !strchr(".!?;:", l2[n2 - 1]))
            snprintf(title, sizeof title, "%s \xE2\x80\x94 %s", l1, l2);   // "CHAPTER II — Pool of Tears"
        else
            snprintf(title, sizeof title, "%s", l1);
        add[nadd].title = strdup(title);
        add[nadd].anchor = strdup("");
        if (!add[nadd].title || !add[nadd].anchor) { free(add[nadd].title); free(add[nadd].anchor); continue; }
        add[nadd].chap = c;
        nadd++;
    }
    free(has);
    if (!nadd || b->ntoc + nadd > TOC_MAX) {
        for (int i = 0; i < nadd; i++) { free(add[i].title); free(add[i].anchor); }
        free(add);
        return;
    }
    // merged in reading order: each new entry before the first old one of a later chapter
    tocent_t *m = (tocent_t *)malloc(sizeof(tocent_t) * (size_t)((b->ntoc + nadd + 63) & ~63));
    if (!m) {
        for (int i = 0; i < nadd; i++) { free(add[i].title); free(add[i].anchor); }
        free(add);
        return;
    }
    int k = 0, o = 0;
    for (int i = 0; i < b->ntoc; i++) {
        while (k < nadd && add[k].chap < b->toc[i].chap) m[o++] = add[k++];
        m[o++] = b->toc[i];
    }
    while (k < nadd) m[o++] = add[k++];
    free(b->toc);
    free(add);
    b->toc = m;
    b->ntoc = o;
}

static int chap_add(book_t *b, const char *path, uint32_t size, uint32_t start, int linear, int fixed) {
    if (!(b->nch & 63)) {
        chap_t *c = (chap_t *)realloc(b->ch, sizeof(chap_t) * (size_t)(b->nch + 64));
        if (!c) return -1;
        b->ch = c;
    }
    chap_t *c = &b->ch[b->nch];
    c->path = path ? strdup(path) : NULL;
    if (path && !c->path) return -1;
    c->size = size ? size : 1;
    c->start = start;
    c->linear = (uint8_t)linear;
    c->fixed = (uint8_t)fixed;
    c->part = 0;
    c->nparts = 1;
    b->total += c->size;
    b->nch++;
    return 0;
}

// A huge XHTML member (a whole library reference in one file): parts of about EPUB_SPLIT, so no page
// waits for megabytes of layout. The parts share the member; book_chapter cuts its text.
static void chap_add_parts(book_t *b, const char *path, uint32_t size, int linear) {
    int n = size > EPUB_SPLIT ? (int)((size + EPUB_SPLIT - 1) / EPUB_SPLIT) : 1;
    if (n > 64) n = 64;
    for (int k = 0; k < n; k++) {
        if (chap_add(b, path, size / (uint32_t)n, 0, linear, 0) < 0) return;
        b->ch[b->nch - 1].part = (uint8_t)k;
        b->ch[b->nch - 1].nparts = (uint8_t)n;
    }
}

static int chap_of_path(const book_t *b, const char *path) {
    for (int i = 0; i < b->nch; i++)
        if (b->ch[i].path && !strcmp(b->ch[i].path, path)) return i;
    for (int i = 0; i < b->nch; i++)
        if (b->ch[i].path && !strcasecmp(b->ch[i].path, path)) return i;
    return -1;
}

// Plain text of an HTML fragment (descriptions are often escaped HTML): tags dropped.
static char *strip_tags(const char *s) {
    char *o = (char *)malloc(strlen(s) + 1);
    if (!o) return NULL;
    size_t k = 0;
    int in = 0, sp = 0;
    for (const char *p = s; *p; p++) {
        if (*p == '<') { in = 1; continue; }
        if (in) { if (*p == '>') { in = 0; sp = k > 0; } continue; }
        if (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') { sp = k > 0; continue; }
        if (sp) { o[k++] = ' '; sp = 0; }
        o[k++] = *p;
    }
    o[k] = 0;
    return o;
}

// ------------------------------------------------------------------------------------ EPUB

typedef struct { char *id, *href, *type, *props; } item_t;

static void items_free(item_t *it, int n) {
    for (int i = 0; i < n; i++) { free(it[i].id); free(it[i].href); free(it[i].type); free(it[i].props); }
    free(it);
}

static int attr_dup(const xtok_t *t, const char *name, char **out) {
    const char *v;
    int vn;
    *out = NULL;
    if (!xml_attr(t, name, &v, &vn)) return 0;
    char tmp[700];
    xml_text(v, vn, tmp, sizeof tmp);
    *out = strdup(tmp);
    return *out != NULL;
}

static int attr_eq(const xtok_t *t, const char *name, const char *want) {
    const char *v;
    int vn;
    return xml_attr(t, name, &v, &vn) && (size_t)vn == strlen(want) && !strncasecmp(v, want, (size_t)vn);
}

static int attr_has(const xtok_t *t, const char *name, const char *word) {   // a space-separated list holds word
    const char *v;
    int vn;
    if (!xml_attr(t, name, &v, &vn)) return 0;
    const size_t wl = strlen(word);
    for (int i = 0; i + (int)wl <= vn; i++)
        if ((i == 0 || v[i - 1] == ' ') && !strncasecmp(v + i, word, wl) && (i + (int)wl == vn || v[i + wl] == ' ')) return 1;
    return 0;
}

static int word_in(const char *list, const char *word) {   // a space-separated list holds word
    if (!list) return 0;
    const size_t wl = strlen(word);
    for (const char *p = list; (p = strstr(p, word)) != NULL; p += wl)
        if ((p == list || p[-1] == ' ') && (p[wl] == 0 || p[wl] == ' ')) return 1;
    return 0;
}

static void set_field(char *dst, size_t cap, const char *s) {
    if (!dst[0] && s && *s) snprintf(dst, cap, "%s", s);
}

// The EPUB 3 navigation document: <nav epub:type="toc"> with nested <ol><li><a href>.
static void parse_nav(book_t *b, const char *nav_path, const char *s, size_t n) {
    xml_t x;
    xml_init(&x, s, n);
    xtok_t t;
    int in_toc = 0, nav_depth = 0, ol_depth = 0;
    while (xml_next(&x, &t) != X_END) {
        if (!in_toc) {
            if (t.type == X_OPEN && xml_is(&t, "nav") && attr_has(&t, "type", "toc")) { in_toc = 1; nav_depth = 1; }
            continue;
        }
        if (t.type == X_OPEN && xml_is(&t, "nav")) nav_depth++;
        else if (t.type == X_CLOSE && xml_is(&t, "nav") && --nav_depth == 0) break;
        else if (t.type == X_OPEN && xml_is(&t, "ol")) ol_depth++;
        else if (t.type == X_CLOSE && xml_is(&t, "ol")) ol_depth--;
        else if (t.type == X_OPEN && (xml_is(&t, "a") || xml_is(&t, "span"))) {
            const char *h = NULL;
            int hn = 0;
            const int is_a = xml_is(&t, "a") && xml_attr(&t, "href", &h, &hn);
            char title[300] = "";
            xml_inner_text(&x, &t, title, sizeof title);
            if (!title[0]) continue;
            int chap = -1;
            char frag[160] = "";
            if (is_a) {
                char path[600], hb[600];
                xml_text(h, hn, hb, sizeof hb);
                if (resolve(nav_path, hb, (int)strlen(hb), path, sizeof path, frag, sizeof frag) >= 0) chap = chap_of_path(b, path);
            }
            if (chap >= 0) toc_add(b, title, chap, frag, ol_depth > 0 ? ol_depth - 1 : 0);
        }
    }
}

// The NCX of EPUB 2: navPoint (nested) > navLabel > text, content src.
static void parse_ncx(book_t *b, const char *ncx_path, const char *s, size_t n) {
    xml_t x;
    xml_init(&x, s, n);
    xtok_t t;
    int depth = 0, in_map = 0;
    char label[300] = "";
    while (xml_next(&x, &t) != X_END) {
        if (t.type == X_OPEN && xml_is(&t, "navMap")) in_map = 1;
        else if (t.type == X_CLOSE && xml_is(&t, "navMap")) break;
        if (!in_map) continue;
        if (t.type == X_OPEN && xml_is(&t, "navPoint")) { depth++; label[0] = 0; }
        else if (t.type == X_CLOSE && xml_is(&t, "navPoint")) depth--;
        else if (t.type == X_OPEN && xml_is(&t, "text")) { label[0] = 0; xml_inner_text(&x, &t, label, sizeof label); }
        else if ((t.type == X_EMPTY || t.type == X_OPEN) && xml_is(&t, "content")) {
            const char *h;
            int hn;
            if (!label[0] || !xml_attr(&t, "src", &h, &hn)) continue;
            char path[600], frag[160], hb[600];
            xml_text(h, hn, hb, sizeof hb);
            if (resolve(ncx_path, hb, (int)strlen(hb), path, sizeof path, frag, sizeof frag) < 0) continue;
            const int chap = chap_of_path(b, path);
            if (chap >= 0) toc_add(b, label, chap, frag, depth > 0 ? depth - 1 : 0);
            label[0] = 0;
        }
    }
}

// DRM: META-INF/encryption.xml that encrypts anything but fonts (font obfuscation is harmless: the
// reader does not use the book's fonts), or Adobe's rights.xml.
static int epub_drm(book_t *b) {
    if (zip_find(b->zip, "META-INF/rights.xml") >= 0) return 1;
    const int i = zip_find(b->zip, "META-INF/encryption.xml");
    if (i < 0) return 0;
    uint8_t *d;
    size_t n;
    if (zip_read(b->zip, i, &d, &n, 1u << 20)) return 0;
    int drm = 0;
    xml_t x;
    xml_init(&x, (const char *)d, n);
    xtok_t t;
    while (!drm && xml_next(&x, &t) != X_END) {
        if ((t.type != X_OPEN && t.type != X_EMPTY) || !xml_is(&t, "CipherReference")) continue;
        const char *u;
        int un;
        if (!xml_attr(&t, "URI", &u, &un)) continue;
        char uri[400];
        xml_text(u, un, uri, sizeof uri);
        if (!ends_with(uri, ".ttf") && !ends_with(uri, ".otf") && !ends_with(uri, ".woff") && !ends_with(uri, ".woff2")) drm = 1;
    }
    free(d);
    return drm;
}

static int open_epub(book_t *b) {
    int ci = zip_find(b->zip, "META-INF/container.xml");
    if (ci < 0) return BKE_FORMAT;
    if (epub_drm(b)) return BKE_DRM;
    uint8_t *d;
    size_t n;
    if (zip_read(b->zip, ci, &d, &n, 1u << 20)) return BKE_FORMAT;
    char opf_path[400] = "";
    xml_t x;
    xml_init(&x, (const char *)d, n);
    xtok_t t;
    while (!opf_path[0] && xml_next(&x, &t) != X_END)
        if ((t.type == X_EMPTY || t.type == X_OPEN) && xml_is(&t, "rootfile")) {
            const char *v, *mt;
            int vn, mn;
            if (!xml_attr(&t, "full-path", &v, &vn)) continue;
            if (xml_attr(&t, "media-type", &mt, &mn) && !attr_eq(&t, "media-type", "application/oebps-package+xml")) continue;
            char raw[400];
            xml_text(v, vn, raw, sizeof raw);
            if (resolve(NULL, raw, (int)strlen(raw), opf_path, sizeof opf_path, NULL, 0) < 0) opf_path[0] = 0;
        }
    free(d);
    const int oi = opf_path[0] ? zip_find(b->zip, opf_path) : -1;
    if (oi < 0 || zip_read(b->zip, oi, &d, &n, 4u << 20)) return BKE_FORMAT;
    // the package: metadata, manifest, spine
    item_t *items = NULL;
    int nitems = 0;
    char cover_id[200] = "", toc_id[200] = "";
    int fixed_book = 0;
    char **spine = NULL;
    uint8_t *spine_lin = NULL, *spine_fix = NULL;
    int nspine = 0;
    xml_init(&x, (const char *)d, n);
    int in_meta = 0;
    while (xml_next(&x, &t) != X_END) {
        if (t.type == X_OPEN && xml_is(&t, "metadata")) { in_meta = 1; continue; }
        if (t.type == X_CLOSE && xml_is(&t, "metadata")) { in_meta = 0; continue; }
        if (in_meta && (t.type == X_OPEN || t.type == X_EMPTY)) {
            if (xml_is(&t, "meta")) {
                const char *v;
                int vn;
                if (attr_eq(&t, "name", "cover") && xml_attr(&t, "content", &v, &vn)) xml_text(v, vn, cover_id, sizeof cover_id);
                if (attr_eq(&t, "property", "rendition:layout") && t.type == X_OPEN) {
                    char val[40] = "";
                    xml_inner_text(&x, &t, val, sizeof val);
                    fixed_book = !strcmp(val, "pre-paginated");
                }
                continue;
            }
            if (t.type != X_OPEN) continue;
            char val[700] = "";
            if (xml_is(&t, "title")) { xml_inner_text(&x, &t, val, sizeof val); set_field(b->title, sizeof b->title, val); }
            else if (xml_is(&t, "creator")) {
                xml_inner_text(&x, &t, val, sizeof val);
                if (!b->author[0]) set_field(b->author, sizeof b->author, val);
                else if (val[0] && strlen(b->author) + strlen(val) + 3 < sizeof b->author && !strstr(b->author, val)) {
                    strcat(b->author, ", ");
                    strcat(b->author, val);
                }
            } else if (xml_is(&t, "language")) { xml_inner_text(&x, &t, val, sizeof val); set_field(b->lang, sizeof b->lang, val); }
            else if (xml_is(&t, "publisher")) { xml_inner_text(&x, &t, val, sizeof val); set_field(b->publisher, sizeof b->publisher, val); }
            else if (xml_is(&t, "date")) { xml_inner_text(&x, &t, val, sizeof val); set_field(b->date, sizeof b->date, val); }
            else if (xml_is(&t, "rights")) { xml_inner_text(&x, &t, val, sizeof val); set_field(b->rights, sizeof b->rights, val); }
            else if (xml_is(&t, "identifier")) { xml_inner_text(&x, &t, val, sizeof val); set_field(b->ident, sizeof b->ident, val); }
            else if (xml_is(&t, "subject")) {
                xml_inner_text(&x, &t, val, sizeof val);
                if (!b->subject[0]) set_field(b->subject, sizeof b->subject, val);
                else if (val[0] && strlen(b->subject) + strlen(val) + 3 < sizeof b->subject) { strcat(b->subject, "; "); strcat(b->subject, val); }
            } else if (xml_is(&t, "description") && !b->desc) {
                char *big = (char *)malloc(8192);
                if (big) {
                    big[0] = 0;
                    xml_inner_text(&x, &t, big, 8192);
                    b->desc = strip_tags(big);
                    free(big);
                }
            }
            continue;
        }
        if ((t.type == X_EMPTY || t.type == X_OPEN) && xml_is(&t, "item")) {
            if (!(nitems & 63)) {
                item_t *ni = (item_t *)realloc(items, sizeof(item_t) * (size_t)(nitems + 64));
                if (!ni) break;
                items = ni;
            }
            item_t *it = &items[nitems];
            memset(it, 0, sizeof *it);
            attr_dup(&t, "id", &it->id);
            attr_dup(&t, "href", &it->href);
            attr_dup(&t, "media-type", &it->type);
            attr_dup(&t, "properties", &it->props);
            if (!it->id || !it->href) { free(it->id); free(it->href); free(it->type); free(it->props); continue; }
            nitems++;
        } else if (t.type == X_OPEN && xml_is(&t, "spine")) {
            const char *v;
            int vn;
            if (xml_attr(&t, "toc", &v, &vn)) xml_text(v, vn, toc_id, sizeof toc_id);
            if (attr_eq(&t, "page-progression-direction", "rtl")) b->rtl = 1;
        } else if ((t.type == X_EMPTY || t.type == X_OPEN) && xml_is(&t, "itemref")) {
            if (!(nspine & 63)) {
                char **ns = (char **)realloc(spine, sizeof(char *) * (size_t)(nspine + 64));
                uint8_t *nl = (uint8_t *)realloc(spine_lin, (size_t)(nspine + 64));
                if (ns) spine = ns;
                if (nl) spine_lin = nl;
                uint8_t *nf = (uint8_t *)realloc(spine_fix, (size_t)(nspine + 64));
                if (nf) spine_fix = nf;
                if (!ns || !nl || !nf) break;
            }
            char *idref = NULL;
            if (!attr_dup(&t, "idref", &idref)) continue;
            spine[nspine] = idref;
            spine_lin[nspine] = !attr_eq(&t, "linear", "no");
            spine_fix[nspine] = attr_has(&t, "properties", "rendition:layout-pre-paginated") ? 1 :
                                attr_has(&t, "properties", "rendition:layout-reflowable") ? 2 : 0;
            nspine++;
        }
    }
    free(d);
    // the spine, in reading order: linear items first in place, the others after (notes, cover)
    int nfixed = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int s = 0; s < nspine; s++) {
            if (spine_lin[s] != (pass == 0)) continue;
            for (int i = 0; i < nitems; i++) {
                if (strcmp(items[i].id, spine[s])) continue;
                char path[600];
                if (resolve(opf_path, items[i].href, (int)strlen(items[i].href), path, sizeof path, NULL, 0) < 0) break;
                const int zi = zip_find(b->zip, path);
                if (zi < 0) break;
                const int fx = spine_fix[s] == 1 || (fixed_book && spine_fix[s] != 2);
                nfixed += fx;
                if (fx) chap_add(b, zip_name(b->zip, zi), zip_size(b->zip, zi), 0, pass == 0, fx);
                else chap_add_parts(b, zip_name(b->zip, zi), zip_size(b->zip, zi), pass == 0);
                break;
            }
        }
    b->fixed = b->nch > 0 && nfixed == b->nch;
    // the cover: EPUB 3 properties="cover-image", EPUB 2 <meta name="cover">, else an image named cover
    for (int pass = 0; pass < 3 && !b->cover[0]; pass++)
        for (int i = 0; i < nitems && !b->cover[0]; i++) {
            const item_t *it = &items[i];
            const int img = it->type && !strncasecmp(it->type, "image/", 6);
            const int hit = pass == 0 ? it->props && strstr(it->props, "cover-image") :
                            pass == 1 ? cover_id[0] && !strcmp(it->id, cover_id) :
                                        img && (strcasestr(it->id, "cover") || strcasestr(it->href, "cover"));
            if (!hit || !img) continue;
            char path[600];
            if (resolve(opf_path, it->href, (int)strlen(it->href), path, sizeof path, NULL, 0) >= 0 && zip_find(b->zip, path) >= 0)
                snprintf(b->cover, sizeof b->cover, "%s", path);
        }
    // the table of contents: the nav document, else the NCX
    for (int pass = 0; pass < 2 && !b->ntoc; pass++)
        for (int i = 0; i < nitems; i++) {
            const item_t *it = &items[i];
            const int hit = pass == 0 ? word_in(it->props, "nav")
                                      : (toc_id[0] && !strcmp(it->id, toc_id)) ||
                                            (it->type && !strcasecmp(it->type, "application/x-dtbncx+xml"));
            if (!hit) continue;
            char path[600];
            if (resolve(opf_path, it->href, (int)strlen(it->href), path, sizeof path, NULL, 0) < 0) continue;
            const int zi = zip_find(b->zip, path);
            uint8_t *nd;
            size_t nn;
            if (zi < 0 || zip_read(b->zip, zi, &nd, &nn, 4u << 20)) continue;
            if (pass == 0) parse_nav(b, path, (const char *)nd, nn);
            else parse_ncx(b, path, (const char *)nd, nn);
            free(nd);
            if (b->ntoc) break;
        }
    toc_fill(b);
    for (int s = 0; s < nspine; s++) free(spine[s]);
    free(spine);
    free(spine_lin);
    free(spine_fix);
    items_free(items, nitems);
    return b->nch ? BKE_OK : BKE_EMPTY;
}

// A fixed-layout page: the first picture of its XHTML (<img src>, SVG <image href>), as a page. The
// picture's path is made absolute ("/" + the member), so pages can be put together into one text.
static char *fixed_page(const char *xhtml, size_t n, const char *base, size_t *out_len) {
    xml_t x;
    xml_init(&x, xhtml, n);
    xtok_t t;
    char src[600] = "";
    while (!src[0] && xml_next(&x, &t) != X_END) {
        if (t.type != X_OPEN && t.type != X_EMPTY) continue;
        const char *v;
        int vn;
        if ((xml_is(&t, "img") && xml_attr(&t, "src", &v, &vn)) || (xml_is(&t, "image") && xml_attr(&t, "href", &v, &vn)))
            xml_text(v, vn, src, sizeof src);
    }
    if (!src[0]) return NULL;
    char abs[600];
    if (resolve(base, src, (int)strlen(src), abs, sizeof abs, NULL, 0) < 0) return NULL;
    const size_t cap = strlen(abs) + 18;
    char *o = (char *)malloc(cap);
    if (!o) return NULL;
    *out_len = (size_t)snprintf(o, cap, "%c%c/%s%c%c%c%c%c", RT_FIXED, NVHTML_IMAGE, abs, NVHTML_FIELD, NVHTML_FIELD,
                                NVHTML_FIELD, NVHTML_FIELD, NVHTML_FIELD);
    if (*out_len + 2 > cap) { free(o); return NULL; }
    o[(*out_len)++] = NVHTML_IMAGE_END;
    o[*out_len] = 0;
    return o;
}

// ------------------------------------------------------------------------------------ CBZ

static int is_picture_name(const char *s) {
    return ends_with(s, ".jpg") || ends_with(s, ".jpeg") || ends_with(s, ".png") || ends_with(s, ".gif") ||
           ends_with(s, ".webp") || ends_with(s, ".bmp");
}

static int natcmp(const char *a, const char *b) {   // "page2" < "page10"
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            while (*a == '0') a++;
            while (*b == '0') b++;
            const char *a0 = a, *b0 = b;
            while (isdigit((unsigned char)*a)) a++;
            while (isdigit((unsigned char)*b)) b++;
            if (a - a0 != b - b0) return (int)((a - a0) - (b - b0));
            const int c = strncmp(a0, b0, (size_t)(a - a0));
            if (c) return c;
            continue;
        }
        const int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static const zip_t *s_sort_zip;
static int cmp_members(const void *x, const void *y) {
    return natcmp(zip_name(s_sort_zip, *(const int *)x), zip_name(s_sort_zip, *(const int *)y));
}

static int open_cbz(book_t *b) {
    const int n = zip_count(b->zip);
    int *idx = (int *)malloc(sizeof(int) * (size_t)(n ? n : 1));
    if (!idx) return BKE_MEM;
    int k = 0;
    for (int i = 0; i < n; i++) {
        const char *nm = zip_name(b->zip, i);
        const char *base = strrchr(nm, '/');
        base = base ? base + 1 : nm;
        if (base[0] != '.' && !strstr(nm, "__MACOSX") && is_picture_name(nm)) idx[k++] = i;
    }
    s_sort_zip = b->zip;
    qsort(idx, (size_t)k, sizeof(int), cmp_members);
    for (int i = 0; i < k; i++) chap_add(b, zip_name(b->zip, idx[i]), zip_size(b->zip, idx[i]), 0, 1, 1);
    if (k) snprintf(b->cover, sizeof b->cover, "%s", zip_name(b->zip, idx[0]));
    free(idx);
    b->fixed = 1;
    // ComicInfo.xml (the de-facto metadata of comic archives)
    const int ci = zip_find(b->zip, "ComicInfo.xml");
    uint8_t *d;
    size_t dn;
    if (ci >= 0 && !zip_read(b->zip, ci, &d, &dn, 1u << 20)) {
        xml_t x;
        xml_init(&x, (const char *)d, dn);
        xtok_t t;
        while (xml_next(&x, &t) != X_END) {
            if (t.type != X_OPEN) continue;
            char v[300] = "";
            if (xml_is(&t, "Title")) { xml_inner_text(&x, &t, v, sizeof v); set_field(b->title, sizeof b->title, v); }
            else if (xml_is(&t, "Writer")) { xml_inner_text(&x, &t, v, sizeof v); set_field(b->author, sizeof b->author, v); }
            else if (xml_is(&t, "LanguageISO")) { xml_inner_text(&x, &t, v, sizeof v); set_field(b->lang, sizeof b->lang, v); }
            else if (xml_is(&t, "Publisher")) { xml_inner_text(&x, &t, v, sizeof v); set_field(b->publisher, sizeof b->publisher, v); }
            else if (xml_is(&t, "Year")) { xml_inner_text(&x, &t, v, sizeof v); set_field(b->date, sizeof b->date, v); }
            else if (xml_is(&t, "Manga")) { xml_inner_text(&x, &t, v, sizeof v); if (!strcasecmp(v, "YesAndRightToLeft")) b->rtl = 1; }
            else if (xml_is(&t, "Summary") && !b->desc) { char s[2000] = ""; xml_inner_text(&x, &t, s, sizeof s); b->desc = strdup(s); }
        }
        free(d);
    }
    return b->nch ? BKE_OK : BKE_EMPTY;
}

// --------------------------------------------------------------------- whole-text formats

// Cuts the rich text into chapters: at headings of level 1 (or 2 when there is no more than one
// of level 1), merging very short ones, and at a blank line once a chapter is longer than CHAP_SPLIT.
// The headings of the chosen levels become the table of contents.
static void cut_chapters(book_t *b) {
    const char *t = b->rt;
    const size_t n = b->rtn;
    int count[3] = {0};
    for (size_t i = 0; i < n;) {
        const char *e = memchr(t + i, '\n', n - i);
        const size_t l = e ? (size_t)(e - (t + i)) : n - i;
        if (l > 2 && t[i] == '#' && t[i + 1] == ' ') count[1]++;
        else if (l > 3 && t[i] == '#' && t[i + 1] == '#' && t[i + 2] == ' ') count[2]++;
        i += l + 1;
    }
    const int level = count[1] >= 2 ? 1 : count[2] >= 2 ? 2 : 0;
    size_t start = 0;
    int last_head_chap = -1;
    for (size_t i = 0; i <= n;) {
        const char *e = i < n ? memchr(t + i, '\n', n - i) : NULL;
        const size_t l = i < n ? (e ? (size_t)(e - (t + i)) : n - i) : 0;
        int hl = 0;
        while (hl < (int)l && hl < 7 && t[i + hl] == '#') hl++;
        const int head = i < n && hl >= 1 && hl < (int)l && t[i + hl] == ' ' && hl <= level && level;
        const int blank = i < n && !l;
        if (i == n || (head && i - start > 1500) || (blank && i - start > CHAP_SPLIT)) {
            if (i > start) chap_add(b, NULL, (uint32_t)(i - start), (uint32_t)start, 1, 0);
            start = i;
        }
        if (head && hl <= level) {
            char title[300];
            const char *anc = memchr(t + i + hl + 1, NVHTML_ANCHOR, l - (size_t)hl - 1);
            const size_t tl = anc ? (size_t)(anc - (t + i + hl + 1)) : l - (size_t)hl - 1;
            size_t k = 0;
            for (size_t j = 0; j < tl && k + 1 < sizeof title; j++)
                if ((unsigned char)t[i + hl + 1 + j] >= 0x20) title[k++] = t[i + hl + 1 + j];
            title[k] = 0;
            const int chap = b->nch;   // the chapter this heading starts (cut above, or the first)
            if (chap != last_head_chap || hl > 1) toc_add(b, title, chap, "", hl - 1);
            last_head_chap = chap;
        }
        if (i == n) break;
        i += l + 1;
    }
    if (!b->nch) chap_add(b, NULL, 1, 0, 1, 0);
}

static int s_meta_only;   // book_open_meta: the whole-text formats read only their first bytes
#define META_HEAD (64 * 1024)

static int open_whole(book_t *b, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return BKE_OPEN;
    char *s = NULL;
    size_t n = 0;
    if (fseek(f, 0, SEEK_END) == 0) {
        const long sz = ftell(f);
        if (sz < 0 || (unsigned long)sz > WHOLE_MAX) { fclose(f); return BKE_BIG; }
        fseek(f, 0, SEEK_SET);
        size_t want = (size_t)sz;
        if (s_meta_only && b->kind != BK_FB2 && want > META_HEAD) want = META_HEAD;   // a title is near the top
        s = (char *)malloc(want + 1);
        if (!s) { fclose(f); return BKE_MEM; }
        n = fread(s, 1, want, f);
        s[n] = 0;
    }
    fclose(f);
    if (!s) return BKE_OPEN;
    if (b->kind == BK_FB2 && n >= 4 && !memcmp(s, "PK\x03\x04", 4)) {   // .fb2.zip: the first .fb2 inside
        free(s);
        s = NULL;
        zip_t *z;
        if (zip_open(path, &z)) return BKE_FORMAT;
        for (int i = 0; i < zip_count(z) && !s; i++)
            if (ends_with(zip_name(z, i), ".fb2")) {
                uint8_t *d;
                if (!zip_read(z, i, &d, &n, WHOLE_MAX)) s = (char *)d;
            }
        zip_close(z);
        if (!s) return BKE_FORMAT;
    }
    size_t rn = 0;
    char *rt = NULL;
    if (b->kind == BK_FB2) {
        rt = fb2_to_rich(s, n, b, &rn);
        if (!rt) { free(s); return BKE_FORMAT; }
        if (b->nbins) { b->src = s; b->srcn = n; s = NULL; }   // the pictures are read from it later
    } else if (b->kind == BK_TXT) {
        rt = txt_to_rich(s, n, &rn);
        // Project Gutenberg's plain text says what it is in its first lines
        const char *hdr[2] = {"Title:", "Author:"};
        char *dst[2] = {b->title, b->author};
        const size_t cap[2] = {sizeof b->title, sizeof b->author};
        for (int k = 0; k < 2; k++) {
            const char *p = s;
            for (int line = 0; p && line < 60; line++) {
                if (!strncmp(p, hdr[k], strlen(hdr[k]))) {
                    const char *v = p + strlen(hdr[k]);
                    while (*v == ' ') v++;
                    size_t l = strcspn(v, "\r\n");
                    if (l >= cap[k]) l = cap[k] - 1;
                    size_t un = 0;
                    char *u = to_utf8(v, l, &un);
                    if (u) { snprintf(dst[k], cap[k], "%s", u); free(u); }
                    break;
                }
                p = strchr(p, '\n');
                if (p) p++;
            }
        }
    } else if (b->kind == BK_MD) {
        size_t un = 0;
        char *u = to_utf8(s, n, &un);
        rt = u ? md_to_rich(u, un, &rn) : NULL;
        free(u);
        if (rt && rn > 2 && rt[0] == '#' && rt[1] == ' ') {   // the first heading is the title
            const size_t l = strcspn(rt + 2, "\n");
            size_t k = 0;
            for (size_t i = 0; i < l && k + 1 < sizeof b->title; i++)
                if ((unsigned char)rt[2 + i] >= 0x20) b->title[k++] = rt[2 + i];
            b->title[k] = 0;
        }
    } else {   // HTML
        size_t un = 0;
        char *u = to_utf8(s, n, &un);
        if (u) {
            // the page's <title> as the book's title
            const char *tt = strcasestr(u, "<title");
            const char *gt = tt ? strchr(tt, '>') : NULL, *te = gt ? strcasestr(gt, "</title") : NULL;
            if (te && te - gt < 400) xml_text(gt + 1, (int)(te - gt - 1), b->title, sizeof b->title);
            nvhtml_opts_t o;
            memset(&o, 0, sizeof o);
            o.rich = 1;
            o.back_matter = 1;
            o.book = 1;
            rt = nvhtml_text(u, un, &o, &rn);
        }
        free(u);
    }
    free(s);
    if (!rt) return BKE_MEM;
    b->rt = rt;
    b->rtn = rn;
    cut_chapters(b);
    return BKE_OK;
}

// ------------------------------------------------------------------------------------ the API

int book_open_meta(const char *path, book_t **out) {
    s_meta_only = 1;
    const int r = book_open(path, out);
    s_meta_only = 0;
    return r;
}

int book_open(const char *path, book_t **out) {
    *out = NULL;
    const int kind = book_kind_of(path);
    if (kind < 0) return BKE_FORMAT;
    book_t *b = (book_t *)calloc(1, sizeof *b);
    if (!b) return BKE_MEM;
    b->kind = kind;
    snprintf(b->file, sizeof b->file, "%s", path);
    struct stat st;
    if (stat(path, &st) == 0) b->file_size = (int64_t)st.st_size;
    int r;
    if (kind == BK_EPUB || kind == BK_CBZ) {
        if (zip_open(path, &b->zip)) r = BKE_FORMAT;
        else r = kind == BK_EPUB ? open_epub(b) : open_cbz(b);
    } else {
        r = open_whole(b, path);
    }
    if (r != BKE_OK) { book_close(b); return r; }
    if (!b->title[0]) {   // the file's name, without its extension
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        snprintf(b->title, sizeof b->title, "%s", base);
        char *dot = strrchr(b->title, '.');
        if (dot && dot != b->title) *dot = 0;
        if (ends_with(b->title, ".fb2")) b->title[strlen(b->title) - 4] = 0;
        for (char *p = b->title; *p; p++) if (*p == '_') *p = ' ';
    }
    if (!b->ntoc && b->fixed && b->nch > 1) {   // a comic: a few landmarks are enough
        char t[32];
        for (int i = 0; i < b->nch; i += b->nch > 40 ? b->nch / 20 : 1) {
            snprintf(t, sizeof t, "%d", i + 1);
            toc_add(b, t, i, "", 0);
        }
    }
    *out = b;
    return BKE_OK;
}

void book_close(book_t *b) {
    if (!b) return;
    zip_close(b->zip);
    for (int i = 0; i < b->nch; i++) free(b->ch[i].path);
    free(b->ch);
    for (int i = 0; i < b->ntoc; i++) { free(b->toc[i].title); free(b->toc[i].anchor); }
    free(b->toc);
    free(b->desc);
    free(b->rt);
    free(b->bins);
    free(b->src);
    free(b->part_rt);
    free(b);
}

int book_chapter(book_t *b, int i, char **text, size_t *n) {
    *text = NULL;
    *n = 0;
    if (i < 0 || i >= b->nch) return BKE_FORMAT;
    const chap_t *c = &b->ch[i];
    if (b->rt) {   // a slice of the whole text
        const size_t end = i + 1 < b->nch ? b->ch[i + 1].start : b->rtn;
        if (c->start > end || end > b->rtn) return BKE_FORMAT;
        *text = dupn(b->rt + c->start, end - c->start);
        *n = end - c->start;
        return *text ? BKE_OK : BKE_MEM;
    }
    if (b->kind == BK_CBZ) {   // a picture: a page of its own
        const size_t cap = strlen(c->path) + 16;
        char *o = (char *)malloc(cap);
        if (!o) return BKE_MEM;
        // "/" + the member: book_resource takes it from the root of the archive
        int k = snprintf(o, cap, "%c%c/%s%c%c%c%c%c", RT_FIXED, NVHTML_IMAGE, c->path, NVHTML_FIELD, NVHTML_FIELD,
                         NVHTML_FIELD, NVHTML_FIELD, NVHTML_FIELD);
        if (k < 0 || (size_t)k + 2 > cap) { free(o); return BKE_MEM; }
        o[k++] = NVHTML_IMAGE_END;
        o[k] = 0;
        *text = o;
        *n = (size_t)k;
        return BKE_OK;
    }
    if (c->nparts > 1 && b->part_rt && !strcmp(b->part_path, c->path)) goto cut;   // the member read just before
    const int zi = zip_find(b->zip, c->path);
    uint8_t *d;
    size_t dn;
    int r = zi < 0 ? BKE_FORMAT : zip_read(b->zip, zi, &d, &dn, MEMBER_MAX);
    if (r) return r;
    char *html = (char *)d;
    if (dn >= 2 && ((d[0] == 0xFF && d[1] == 0xFE) || (d[0] == 0xFE && d[1] == 0xFF))) {   // UTF-16
        size_t un = 0;
        char *u = to_utf8((const char *)d, dn, &un);
        free(d);
        if (!u) return BKE_MEM;
        html = u;
        dn = un;
    }
    char *o = NULL;
    size_t on = 0;
    if (c->fixed) o = fixed_page(html, dn, c->path, &on);
    if (!o) {
        nvhtml_opts_t opt;
        memset(&opt, 0, sizeof opt);
        opt.rich = 1;
        opt.back_matter = 1;
        opt.book = 1;
        o = nvhtml_text(html, dn, &opt, &on);
    }
    free(html);
    if (!o) return BKE_MEM;
    if (c->nparts <= 1) {
        *text = o;
        *n = on;
        return BKE_OK;
    }
    free(b->part_rt);   // kept: the next part is read from it
    b->part_rt = o;
    b->part_rtn = on;
    snprintf(b->part_path, sizeof b->part_path, "%s", c->path);
cut:;
    // part k: from the first paragraph break after k/n of the text to the one after (k+1)/n
    const size_t tn = b->part_rtn;
    size_t lo = tn * c->part / c->nparts, hi = c->part + 1 == c->nparts ? tn : tn * (c->part + 1) / c->nparts;
    const char *t = b->part_rt;
    if (c->part) { while (lo < tn && !(t[lo] == '\n' && lo + 1 < tn && t[lo + 1] == '\n')) lo++; }
    if (hi < tn) { while (hi < tn && !(t[hi] == '\n' && hi + 1 < tn && t[hi + 1] == '\n')) hi++; }
    if (lo > hi) lo = hi;
    *text = dupn(t + lo, hi - lo);
    *n = hi - lo;
    return *text ? BKE_OK : BKE_MEM;
}

static size_t b64_decode(const char *s, size_t n, uint8_t *out) {
    static int8_t map[256];
    static int init;
    if (!init) {
        memset(map, -1, sizeof map);
        const char *a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; i++) map[(unsigned char)a[i]] = (int8_t)i;
        init = 1;
    }
    uint32_t acc = 0;
    int bits = 0;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        const int v = map[(unsigned char)s[i]];
        if (v < 0) continue;   // white space, '='
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out[k++] = (uint8_t)(acc >> bits); }
    }
    return k;
}

int book_resource(book_t *b, int chap, const char *href, int hn, uint8_t **data, size_t *n) {
    *data = NULL;
    *n = 0;
    if (hn <= 0) return BKE_FORMAT;
    if (b->kind == BK_FB2) {   // "#id" of a <binary>
        const char *id = href[0] == '#' ? href + 1 : href;
        const int il = href[0] == '#' ? hn - 1 : hn;
        for (int i = 0; i < b->nbins; i++) {
            const bin_t *bn = &b->bins[i];
            if ((int)strlen(bn->id) != il || memcmp(bn->id, id, (size_t)il)) continue;
            if (!b->src || bn->off + bn->len > b->srcn) return BKE_FORMAT;
            uint8_t *o = (uint8_t *)malloc(bn->len * 3 / 4 + 4);
            if (!o) return BKE_MEM;
            *n = b64_decode(b->src + bn->off, bn->len, o);
            *data = o;
            return BKE_OK;
        }
        return BKE_FORMAT;
    }
    if (!b->zip) {   // TXT/HTML/MD: pictures next to the file are not read (the app is sandboxed)
        return BKE_FORMAT;
    }
    char path[600];
    const char *base = chap >= 0 && chap < b->nch ? b->ch[chap].path : NULL;
    if (resolve(base, href, hn, path, sizeof path, NULL, 0) < 0) return BKE_FORMAT;
    const int zi = zip_find(b->zip, path);
    if (zi < 0) return BKE_FORMAT;
    return zip_read(b->zip, zi, data, n, MEMBER_MAX);
}

int book_image_size(book_t *b, int chap, const char *href, int hn, int *w, int *h) {
    *w = *h = 0;
    uint8_t *d = NULL;
    size_t n = 0;
    int r = BKE_FORMAT;
    if (b->zip && hn > 0) {
        char path[600];
        const char *base = chap >= 0 && chap < b->nch ? b->ch[chap].path : NULL;
        const int zi = resolve(base, href, hn, path, sizeof path, NULL, 0) < 0 ? -1 : zip_find(b->zip, path);
        if (zi >= 0) {
            r = zip_read_head(b->zip, zi, &d, &n, 96 * 1024);   // JPEG: the frame header may follow EXIF/ICC data
            if (!r && img_size(d, n, w, h)) {   // not in the head (an SVG): the whole file
                free(d);
                d = NULL;
                r = zip_read(b->zip, zi, &d, &n, MEMBER_MAX);
                if (!r) r = img_size(d, n, w, h) ? BKE_FORMAT : BKE_OK;
            }
        }
    } else if (b->kind == BK_FB2) {
        r = book_resource(b, chap, href, hn, &d, &n);
        if (!r) r = img_size(d, n, w, h) ? BKE_FORMAT : BKE_OK;
    }
    free(d);
    return r;
}

int book_link(const book_t *b, int chap, const char *href, int hn, char *anchor, size_t cap) {
    if (cap) anchor[0] = 0;
    if (hn <= 0) return -2;
    // external: a scheme
    for (int i = 0; i < hn && i < 12; i++) {
        if (href[i] == ':') { snprintf(anchor, cap, "%.*s", hn, href); return -1; }
        if (!isalpha((unsigned char)href[i])) break;
    }
    if (href[0] == '#') {   // inside this chapter (FB2 notes: anywhere in the book)
        snprintf(anchor, cap, "%.*s", hn - 1, href + 1);
        return chap;
    }
    if (!b->zip) return -2;
    char path[600];
    const char *base = chap >= 0 && chap < b->nch ? b->ch[chap].path : NULL;
    if (resolve(base, href, hn, path, sizeof path, anchor, cap) < 0) return -2;
    const int c = chap_of_path(b, path);
    return c >= 0 ? c : -2;
}

int book_cover(book_t *b, uint8_t **data, size_t *n) {
    *data = NULL;
    *n = 0;
    if (!b->cover[0]) return BKE_FORMAT;
    if (b->kind == BK_FB2) return book_resource(b, -1, b->cover, (int)strlen(b->cover), data, n);
    const int zi = zip_find(b->zip, b->cover);
    return zi < 0 ? BKE_FORMAT : zip_read(b->zip, zi, data, n, MEMBER_MAX);
}
