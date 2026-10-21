// conv.c — the whole-text formats as rich text (bk.h), the same lines nvhtml makes of XHTML: a
// paragraph per line group, a blank line between paragraphs, a single line break inside one (verse),
// "# " headings, the inline markers of nvhtml.h.
//   FB2: the FictionBook 2 XML (sections, titles, epigraphs, poems, notes, pictures as <binary>),
//        in any of the encodings FB2 files declare (UTF-8, windows-1251, KOI8-R, windows-1252...).
//   TXT: plain text in UTF-8, UTF-16 or a Windows code page; hard-wrapped lines are joined into
//        paragraphs, chapter headings are recognised in the usual languages.
//   Markdown: headings, paragraphs, lists, quotes, code blocks, emphasis, links and pictures.
#include "bk.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "nvhtml.h"

typedef struct bin_s { char id[64]; size_t off, len; } bin_t;   // as in book.c

typedef struct { char *p; size_t n, cap; int oom; } sb_t;

static void sb_put(sb_t *b, const char *s, size_t n) {
    if (b->oom || !n) return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 65536;
        while (cap < b->n + n + 1) cap *= 2;
        char *q = (char *)realloc(b->p, cap);
        if (!q) { b->oom = 1; return; }
        b->p = q;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void sb_c(sb_t *b, char c) { sb_put(b, &c, 1); }
static void sb_s(sb_t *b, const char *s) { sb_put(b, s, strlen(s)); }
static char *sb_done(sb_t *b, size_t *n) {
    if (b->oom) { free(b->p); return NULL; }
    if (!b->p) { b->p = (char *)calloc(1, 1); if (!b->p) return NULL; }
    while (b->n && (b->p[b->n - 1] == '\n' || b->p[b->n - 1] == ' ')) b->p[--b->n] = 0;
    *n = b->n;
    return b->p;
}
// Ends the current line; blank: and leaves a blank line (a paragraph break). Never two blank lines.
static void sb_brk(sb_t *b, int blank) {
    while (b->n && b->p[b->n - 1] == ' ') b->p[--b->n] = 0;
    if (!b->n) return;
    int k = 0;
    while (k < (int)b->n && b->p[b->n - 1 - k] == '\n') k++;
    for (; k < (blank ? 2 : 1); k++) sb_c(b, '\n');
}

// ------------------------------------------------------------------------------- encodings

static const uint16_t kCp1252[32] = {0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030,
                                     0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD, 0xFFFD, 0x2018, 0x2019, 0x201C,
                                     0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD,
                                     0x017E, 0x0178};
static const uint16_t kCp1251[64] = {
    0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021, 0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F,
    0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0xFFFD, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F,
    0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7, 0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407,
    0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7, 0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457};
static const uint16_t kKoi8r[128] = {
    0x2500, 0x2502, 0x250C, 0x2510, 0x2514, 0x2518, 0x251C, 0x2524, 0x252C, 0x2534, 0x253C, 0x2580, 0x2584, 0x2588, 0x258C, 0x2590,
    0x2591, 0x2592, 0x2593, 0x2320, 0x25A0, 0x2219, 0x221A, 0x2248, 0x2264, 0x2265, 0x00A0, 0x2321, 0x00B0, 0x00B2, 0x00B7, 0x00F7,
    0x2550, 0x2551, 0x2552, 0x0451, 0x2553, 0x2554, 0x2555, 0x2556, 0x2557, 0x2558, 0x2559, 0x255A, 0x255B, 0x255C, 0x255D, 0x255E,
    0x255F, 0x2560, 0x2561, 0x0401, 0x2562, 0x2563, 0x2564, 0x2565, 0x2566, 0x2567, 0x2568, 0x2569, 0x256A, 0x256B, 0x256C, 0x00A9,
    0x044E, 0x0430, 0x0431, 0x0446, 0x0434, 0x0435, 0x0444, 0x0433, 0x0445, 0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E,
    0x043F, 0x044F, 0x0440, 0x0441, 0x0442, 0x0443, 0x0436, 0x0432, 0x044C, 0x044B, 0x0437, 0x0448, 0x044D, 0x0449, 0x0447, 0x044A,
    0x042E, 0x0410, 0x0411, 0x0426, 0x0414, 0x0415, 0x0424, 0x0413, 0x0425, 0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E,
    0x041F, 0x042F, 0x0420, 0x0421, 0x0422, 0x0423, 0x0416, 0x0412, 0x042C, 0x042B, 0x0417, 0x0428, 0x042D, 0x0429, 0x0427, 0x042A};

enum { ENC_UTF8, ENC_CP1252, ENC_CP1251, ENC_KOI8R, ENC_LATIN1, ENC_UTF16LE, ENC_UTF16BE };

static int valid_utf8(const unsigned char *s, size_t n) {
    for (size_t i = 0; i < n;) {
        const unsigned c = s[i];
        if (c < 0x80) { i++; continue; }
        const int k = c >= 0xC2 && c < 0xE0 ? 1 : c >= 0xE0 && c < 0xF0 ? 2 : c >= 0xF0 && c < 0xF5 ? 3 : -1;
        if (k < 0) return 0;
        if (i + (size_t)k >= n) return 1;   // a character cut short at the very end: still UTF-8
        for (int j = 1; j <= k; j++)
            if ((s[i + j] & 0xC0) != 0x80) return 0;
        i += (size_t)k + 1;
    }
    return 1;
}

static char *convert(const char *s, size_t n, int enc, size_t *out_len) {
    const unsigned char *u = (const unsigned char *)s;
    sb_t b = {0};
    if (enc == ENC_UTF16LE || enc == ENC_UTF16BE) {
        for (size_t i = 0; i + 1 < n; i += 2) {
            uint32_t c = enc == ENC_UTF16LE ? (uint32_t)(u[i] | u[i + 1] << 8) : (uint32_t)(u[i] << 8 | u[i + 1]);
            if (c >= 0xD800 && c < 0xDC00 && i + 3 < n) {
                const uint32_t d = enc == ENC_UTF16LE ? (uint32_t)(u[i + 2] | u[i + 3] << 8) : (uint32_t)(u[i + 2] << 8 | u[i + 3]);
                if (d >= 0xDC00 && d < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); i += 2; }
            }
            if (c == 0xFEFF) continue;
            char o[4];
            sb_put(&b, o, (size_t)utf8_put(c, o));
        }
    } else {
        for (size_t i = 0; i < n; i++) {
            uint32_t c = u[i];
            if (c >= 0x80) {
                if (enc == ENC_CP1251) c = c >= 0xC0 ? 0x0410 + (c - 0xC0) : kCp1251[c - 0x80];
                else if (enc == ENC_KOI8R) c = kKoi8r[c - 0x80];
                else if (enc == ENC_CP1252 && c < 0xA0) c = kCp1252[c - 0x80];
            }
            char o[4];
            sb_put(&b, o, (size_t)utf8_put(c, o));
        }
    }
    if (b.oom) { free(b.p); return NULL; }
    if (!b.p) b.p = (char *)calloc(1, 1);
    *out_len = b.n;
    return b.p;
}

char *to_utf8(const char *s, size_t n, size_t *out_len) {
    const unsigned char *u = (const unsigned char *)s;
    if (n >= 2 && u[0] == 0xFF && u[1] == 0xFE) return convert(s + 2, n - 2, ENC_UTF16LE, out_len);
    if (n >= 2 && u[0] == 0xFE && u[1] == 0xFF) return convert(s + 2, n - 2, ENC_UTF16BE, out_len);
    if (n >= 3 && u[0] == 0xEF && u[1] == 0xBB && u[2] == 0xBF) { s += 3; n -= 3; }
    if (valid_utf8((const unsigned char *)s, n)) {
        char *o = (char *)malloc(n + 1);
        if (!o) return NULL;
        memcpy(o, s, n);
        o[n] = 0;
        *out_len = n;
        return o;
    }
    // not UTF-8: Cyrillic letters in the upper half (windows-1251) or the Western code page
    size_t hi = 0, cyr = 0;
    for (size_t i = 0; i < n; i++)
        if (u[i] >= 0x80) { hi++; cyr += u[i] >= 0xC0; }
    const int enc = hi > 50 && cyr * 10 > hi * 9 && hi * 20 > n ? ENC_CP1251 : ENC_CP1252;
    return convert(s, n, enc, out_len);
}

static int enc_by_name(const char *e, int n) {
    char b[24];
    int k = 0;
    for (int i = 0; i < n && k + 1 < (int)sizeof b; i++)
        if (isalnum((unsigned char)e[i])) b[k++] = (char)tolower((unsigned char)e[i]);
    b[k] = 0;
    if (!strcmp(b, "windows1251") || !strcmp(b, "cp1251")) return ENC_CP1251;
    if (!strcmp(b, "koi8r") || !strcmp(b, "koi8u")) return ENC_KOI8R;
    if (!strcmp(b, "windows1252") || !strcmp(b, "cp1252")) return ENC_CP1252;
    if (!strcmp(b, "iso88591") || !strcmp(b, "latin1")) return ENC_LATIN1;
    if (!strcmp(b, "utf16") || !strcmp(b, "utf16le")) return ENC_UTF16LE;
    if (!strcmp(b, "utf16be")) return ENC_UTF16BE;
    return ENC_UTF8;
}

// ------------------------------------------------------------------------------------ FB2

typedef struct {
    sb_t *o;
    int sect;                // section depth in the current body
    int in_title, in_p;
    int notes;               // inside the body of notes
    int link_open;
} fb_t;

// Inline content of a paragraph-like element until its end tag: text, emphasis, strong, links
// (notes become superscript links), pictures.
static void fb_inline(xml_t *x, fb_t *f, const xtok_t *open) {
    char tmp[4096];
    int depth = 1;
    xtok_t t;
    while (depth > 0 && xml_next(x, &t) != X_END) {
        if (t.type == X_TEXT) {
            const size_t k = xml_text(t.text, t.tlen, tmp + 1, sizeof tmp - 2);
            // xml_text trims: keep one space where the source had space at either end
            const int lead = t.tlen && strchr(" \t\r\n", t.text[0]), trail = t.tlen && strchr(" \t\r\n", t.text[t.tlen - 1]);
            if (!k) { if (lead || trail) sb_c(f->o, ' '); continue; }
            if (lead) sb_c(f->o, ' ');
            sb_put(f->o, tmp + 1, k);
            if (trail) sb_c(f->o, ' ');
            continue;
        }
        if (t.type == X_OPEN) depth++;
        else if (t.type == X_CLOSE) depth--;
        if (t.type == X_OPEN || t.type == X_EMPTY) {
            if (xml_is(&t, "emphasis")) sb_c(f->o, NVHTML_ITALIC_ON);
            else if (xml_is(&t, "strong")) sb_c(f->o, NVHTML_BOLD_ON);
            else if (xml_is(&t, "sup")) sb_c(f->o, NVHTML_SUP_ON);
            else if (xml_is(&t, "sub")) sb_c(f->o, NVHTML_SUB_ON);
            else if (xml_is(&t, "code")) sb_c(f->o, NVHTML_CODE_ON);
            else if (xml_is(&t, "a") && t.type == X_OPEN) {
                const char *h;
                int hn;
                if (xml_attr(&t, "href", &h, &hn) && hn > 0) {
                    char hb[NVHTML_HREF_MAX];
                    xml_text(h, hn, hb, sizeof hb);
                    const char *ty;
                    int tn;
                    const int note = xml_attr(&t, "type", &ty, &tn) && tn == 4 && !strncmp(ty, "note", 4);
                    if (note) sb_c(f->o, NVHTML_SUP_ON);
                    sb_c(f->o, NVHTML_LINK);
                    sb_s(f->o, hb);
                    sb_c(f->o, NVHTML_LINK_TEXT);
                    f->link_open = note ? 2 : 1;
                }
            } else if (xml_is(&t, "image")) {
                const char *h;
                int hn;
                if (xml_attr(&t, "href", &h, &hn) && hn > 0) {
                    char hb[300];
                    xml_text(h, hn, hb, sizeof hb);
                    sb_c(f->o, NVHTML_IMAGE);
                    sb_s(f->o, hb);
                    sb_s(f->o, "\x1E\x1E\x1E\x1E\x1E");
                    sb_c(f->o, NVHTML_IMAGE_END);
                }
            }
        }
        if (t.type == X_CLOSE) {
            if (xml_is(&t, "emphasis")) sb_c(f->o, NVHTML_ITALIC_OFF);
            else if (xml_is(&t, "strong")) sb_c(f->o, NVHTML_BOLD_OFF);
            else if (xml_is(&t, "sup")) sb_c(f->o, NVHTML_SUP_OFF);
            else if (xml_is(&t, "sub")) sb_c(f->o, NVHTML_SUB_OFF);
            else if (xml_is(&t, "code")) sb_c(f->o, NVHTML_CODE_OFF);
            else if (xml_is(&t, "a") && f->link_open) {
                sb_c(f->o, NVHTML_LINK_END);
                if (f->link_open == 2) sb_c(f->o, NVHTML_SUP_OFF);
                f->link_open = 0;
            }
        }
    }
    (void)open;
}

static void fb_id(fb_t *f, const xtok_t *t) {
    const char *v;
    int vn;
    if (!xml_attr(t, "id", &v, &vn) || vn <= 0 || vn > 120) return;
    char id[130];
    xml_text(v, vn, id, sizeof id);
    sb_c(f->o, RT_ID);
    sb_s(f->o, id);
    sb_c(f->o, RT_ID_END);
}

// <title>: its paragraphs joined into one heading of the section's depth.
static void fb_title(xml_t *x, fb_t *f, int level) {
    sb_brk(f->o, 1);
    static const char kHashes[] = "######";
    sb_put(f->o, kHashes, (size_t)(level < 1 ? 1 : level > 6 ? 6 : level));
    sb_c(f->o, ' ');
    const size_t start = f->o->n;
    int depth = 1;
    xtok_t t;
    while (depth > 0 && xml_next(x, &t) != X_END) {
        if (t.type == X_OPEN && xml_is(&t, "p")) {
            if (f->o->n > start) {
                const char c = f->o->p[f->o->n - 1];
                sb_s(f->o, strchr(".:!?;", c) ? " " : ". ");
            }
            fb_inline(x, f, &t);
            while (f->o->n > start && f->o->p[f->o->n - 1] == ' ') f->o->n--;
            continue;
        }
        if (t.type == X_OPEN) depth++;
        else if (t.type == X_CLOSE) depth--;
    }
    // a heading is one line: line breaks inside it become spaces
    for (size_t i = start; i < f->o->n; i++) if (f->o->p[i] == '\n') f->o->p[i] = ' ';
    sb_brk(f->o, 1);
}

static void fb_meta_text(xml_t *x, const xtok_t *t, char *dst, size_t cap) {
    char v[600] = "";
    xml_inner_text(x, t, v, sizeof v);
    if (!dst[0] && v[0]) snprintf(dst, cap, "%s", v);
}

static void fb2_bins(const char *s, size_t n, book_t *b) {
    xml_t x;
    xml_init(&x, s, n);
    xtok_t t;
    while (xml_next(&x, &t) != X_END) {
        if (t.type != X_OPEN || !xml_is(&t, "binary")) continue;
        const char *v;
        int vn;
        if (!xml_attr(&t, "id", &v, &vn) || vn <= 0 || vn >= 63) continue;
        xtok_t c;
        if (xml_next(&x, &c) != X_TEXT) continue;
        bin_t *nb = (bin_t *)realloc(b->bins, sizeof(bin_t) * (size_t)(b->nbins + 1));
        if (!nb) return;
        b->bins = nb;
        bin_t *e = &b->bins[b->nbins++];
        memcpy(e->id, v, (size_t)vn);
        e->id[vn] = 0;
        e->off = (size_t)(c.text - s);
        e->len = (size_t)c.tlen;
    }
}

char *fb2_to_rich(const char *raw, size_t rn, book_t *meta, size_t *out_len) {
    // the pictures, by their offsets in the file as it is (base64 is ASCII in any encoding)
    fb2_bins(raw, rn, meta);
    // the declared encoding
    int enc = ENC_UTF8;
    if (rn > 5 && !memcmp(raw, "<?xml", 5)) {
        const char *e = memchr(raw, '>', rn < 200 ? rn : 200);
        const char *en = e ? strstr(raw, "encoding") : NULL;
        if (en && en < e) {
            const char *q = en + 8;
            while (q < e && (*q == ' ' || *q == '=')) q++;
            if (q < e && (*q == '"' || *q == '\'')) {
                const char *q2 = memchr(q + 1, *q, (size_t)(e - q - 1));
                if (q2) enc = enc_by_name(q + 1, (int)(q2 - q - 1));
            }
        }
    }
    size_t n = rn;
    char *own = NULL;
    const char *s = raw;
    if (enc != ENC_UTF8 || (rn >= 2 && ((unsigned char)raw[0] == 0xFF || (unsigned char)raw[0] == 0xFE))) {
        own = enc == ENC_UTF8 ? to_utf8(raw, rn, &n) : convert(raw, rn, enc, &n);
        if (!own) return NULL;
        s = own;
    }
    sb_t o = {0};
    fb_t f;
    memset(&f, 0, sizeof f);
    f.o = &o;
    xml_t x;
    xml_init(&x, s, n);
    xtok_t t;
    int in_desc = 0, in_title_info = 0, in_publish = 0, in_body = 0, saw_fb = 0;
    char first[80] = "", middle[80] = "", last[80] = "", nick[80] = "";
    int in_author = 0;
    while (xml_next(&x, &t) != X_END) {
        if (t.type == X_OPEN && xml_is(&t, "FictionBook")) saw_fb = 1;
        if (t.type == X_OPEN && xml_is(&t, "description")) { in_desc = 1; continue; }
        if (t.type == X_CLOSE && xml_is(&t, "description")) { in_desc = 0; continue; }
        if (in_desc) {
            if (t.type == X_OPEN && xml_is(&t, "title-info")) in_title_info = 1;
            else if (t.type == X_CLOSE && xml_is(&t, "title-info")) in_title_info = 0;
            else if (t.type == X_OPEN && xml_is(&t, "publish-info")) in_publish = 1;
            else if (t.type == X_CLOSE && xml_is(&t, "publish-info")) in_publish = 0;
            if (!in_title_info && !in_publish) continue;
            if (t.type == X_OPEN && xml_is(&t, "author") && in_title_info) { in_author = 1; first[0] = middle[0] = last[0] = nick[0] = 0; continue; }
            if (t.type == X_CLOSE && xml_is(&t, "author") && in_author) {
                in_author = 0;
                char name[240];
                if (first[0] || last[0])
                    snprintf(name, sizeof name, "%s%s%s%s%s", first, middle[0] ? " " : "", middle, first[0] && last[0] ? " " : "", last);
                else
                    snprintf(name, sizeof name, "%s", nick);
                if (name[0] && !meta->author[0]) snprintf(meta->author, sizeof meta->author, "%s", name);
                else if (name[0] && strlen(meta->author) + strlen(name) + 3 < sizeof meta->author) {
                    strcat(meta->author, ", ");
                    strcat(meta->author, name);
                }
                continue;
            }
            if (t.type != X_OPEN && t.type != X_EMPTY) continue;
            if (in_author && t.type == X_OPEN) {
                if (xml_is(&t, "first-name")) fb_meta_text(&x, &t, first, sizeof first);
                else if (xml_is(&t, "middle-name")) fb_meta_text(&x, &t, middle, sizeof middle);
                else if (xml_is(&t, "last-name")) fb_meta_text(&x, &t, last, sizeof last);
                else if (xml_is(&t, "nickname")) fb_meta_text(&x, &t, nick, sizeof nick);
                continue;
            }
            if (t.type == X_OPEN && in_title_info) {
                if (xml_is(&t, "book-title")) fb_meta_text(&x, &t, meta->title, sizeof meta->title);
                else if (xml_is(&t, "lang")) fb_meta_text(&x, &t, meta->lang, sizeof meta->lang);
                else if (xml_is(&t, "genre")) fb_meta_text(&x, &t, meta->subject, sizeof meta->subject);
                else if (xml_is(&t, "date")) fb_meta_text(&x, &t, meta->date, sizeof meta->date);
                else if (xml_is(&t, "annotation") && !meta->desc) {
                    char *d = (char *)malloc(4096);
                    if (d) { d[0] = 0; xml_inner_text(&x, &t, d, 4096); meta->desc = d; }
                }
            } else if (t.type == X_OPEN && in_publish) {
                if (xml_is(&t, "publisher")) fb_meta_text(&x, &t, meta->publisher, sizeof meta->publisher);
                else if (xml_is(&t, "year") && !meta->date[0]) fb_meta_text(&x, &t, meta->date, sizeof meta->date);
            }
            if (xml_is(&t, "image") && in_title_info && !meta->cover[0]) {
                const char *h;
                int hn;
                if (xml_attr(&t, "href", &h, &hn) && hn > 0) xml_text(h, hn, meta->cover, sizeof meta->cover);
            }
            continue;
        }
        if (t.type == X_OPEN && xml_is(&t, "body")) {
            in_body = 1;
            const char *v;
            int vn;
            f.notes = xml_attr(&t, "name", &v, &vn) && vn > 0;
            f.sect = 0;
            if (f.notes) {   // the notes start a chapter of their own
                sb_brk(&o, 1);
                char title[100] = "";
                if (vn < 90) xml_text(v, vn, title, sizeof title);
                if (title[0]) title[0] = (char)toupper((unsigned char)title[0]);
                sb_s(&o, "# ");
                sb_s(&o, title[0] ? title : "Notes");
                sb_brk(&o, 1);
            }
            continue;
        }
        if (t.type == X_CLOSE && xml_is(&t, "body")) { in_body = 0; continue; }
        if (t.type == X_OPEN && xml_is(&t, "binary")) { xtok_t c; while (xml_next(&x, &c) != X_END && !(c.type == X_CLOSE && xml_is(&c, "binary"))) {} continue; }
        if (!in_body) continue;
        if (t.type == X_OPEN && xml_is(&t, "section")) {
            f.sect++;
            sb_brk(&o, 1);
            fb_id(&f, &t);
            continue;
        }
        if (t.type == X_CLOSE && xml_is(&t, "section")) { if (f.sect) f.sect--; sb_brk(&o, 1); continue; }
        if (t.type == X_OPEN && xml_is(&t, "title")) {
            // the body's own title is the book's: level 1 only when there are no sections yet
            fb_title(&x, &f, f.notes ? 2 : f.sect ? f.sect : 1);
            continue;
        }
        if (t.type == X_OPEN && (xml_is(&t, "p") || xml_is(&t, "v") || xml_is(&t, "subtitle") || xml_is(&t, "text-author"))) {
            const int verse = xml_is(&t, "v"), sub = xml_is(&t, "subtitle"), au = xml_is(&t, "text-author");
            sb_brk(&o, !verse);
            fb_id(&f, &t);
            if (sub) sb_c(&o, NVHTML_BOLD_ON);
            if (au) sb_c(&o, NVHTML_ITALIC_ON);
            fb_inline(&x, &f, &t);
            if (sub) sb_c(&o, NVHTML_BOLD_OFF);
            if (au) sb_c(&o, NVHTML_ITALIC_OFF);
            sb_brk(&o, !verse);
            continue;
        }
        if (t.type == X_OPEN && xml_is(&t, "stanza")) { sb_brk(&o, 1); continue; }
        if (t.type == X_CLOSE && (xml_is(&t, "stanza") || xml_is(&t, "poem") || xml_is(&t, "epigraph") || xml_is(&t, "cite"))) {
            sb_brk(&o, 1);
            continue;
        }
        if ((t.type == X_EMPTY || t.type == X_OPEN) && xml_is(&t, "empty-line")) {
            sb_brk(&o, 1);
            sb_s(&o, "\xC2\xA0");   // a line of its own: the empty line shows
            sb_brk(&o, 1);
            continue;
        }
        if ((t.type == X_EMPTY || t.type == X_OPEN) && xml_is(&t, "image")) {
            const char *h;
            int hn;
            if (xml_attr(&t, "href", &h, &hn) && hn > 0) {
                char hb[300];
                xml_text(h, hn, hb, sizeof hb);
                sb_brk(&o, 1);
                sb_c(&o, NVHTML_IMAGE);
                sb_s(&o, hb);
                sb_s(&o, "\x1E\x1E\x1E\x1E" "f\x1E");   // a figure: on its own line
                sb_c(&o, NVHTML_IMAGE_END);
                sb_brk(&o, 1);
            }
            continue;
        }
        if (t.type == X_OPEN && xml_is(&t, "tr")) {
            sb_brk(&o, 0);
            sb_c(&o, NVHTML_ROW);
            int first_cell = 1;
            xtok_t c;
            while (xml_next(&x, &c) != X_END && !(c.type == X_CLOSE && xml_is(&c, "tr")))
                if (c.type == X_OPEN && (xml_is(&c, "td") || xml_is(&c, "th"))) {
                    if (!first_cell) sb_c(&o, NVHTML_CELL);
                    first_cell = 0;
                    const int th = xml_is(&c, "th");
                    if (th) sb_c(&o, NVHTML_BOLD_ON);
                    fb_inline(&x, &f, &c);
                    if (th) sb_c(&o, NVHTML_BOLD_OFF);
                }
            sb_brk(&o, 0);
            continue;
        }
        if (t.type == X_CLOSE && xml_is(&t, "table")) { sb_brk(&o, 1); continue; }
    }
    free(own);
    if (!saw_fb) { free(o.p); return NULL; }
    return sb_done(&o, out_len);
}

// ------------------------------------------------------------------------------------ TXT

// A chapter heading of plain text: "CHAPTER IV", "Capitolo 3", "Глава 1", "PART TWO", a roman
// numeral or a number alone on its line.
static int txt_heading(const char *s, size_t n) {
    while (n && (s[n - 1] == ' ' || s[n - 1] == '.')) n--;
    while (n && *s == ' ') { s++; n--; }
    if (!n || n > 70) return 0;
    static const char *const kWords[] = {"chapter", "capitolo", "cap.", "kapitel", "chapitre", "cap\xC3\xADtulo", "capitulo",
                                         "part ", "parte ", "teil ", "partie ", "book ", "libro ", "livre ", "buch ",
                                         "canto ", "prologue", "prologo", "epilogue", "epilogo", "\xD0\x93\xD0\xBB\xD0\xB0\xD0\xB2\xD0\xB0",
                                         "\xD0\x93\xD0\x9B\xD0\x90\xD0\x92\xD0\x90", "act ", "atto ", "scene ", "scena ", NULL};
    for (int i = 0; kWords[i]; i++) {
        const size_t l = strlen(kWords[i]);
        if (n >= l && !strncasecmp(s, kWords[i], l) && (n == l || !isalpha((unsigned char)s[l]) || kWords[i][l - 1] == ' '))
            return 1;
    }
    int roman = 1, digits = 1;
    for (size_t i = 0; i < n; i++) {
        if (!strchr("IVXLCDM", s[i])) roman = 0;
        if (!isdigit((unsigned char)s[i])) digits = 0;
    }
    return (roman && n <= 8) || (digits && n <= 3);
}

static void put_plain(sb_t *o, const char *s, size_t n) {   // text without control bytes
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c == '\t') sb_c(o, ' ');
        else if (c >= 0x20 && c != 0x7F) sb_c(o, (char)c);
    }
}

char *txt_to_rich(const char *raw, size_t rn, size_t *out_len) {
    size_t n = 0;
    char *s = to_utf8(raw, rn, &n);
    if (!s) return NULL;
    // the lines, CR LF or CR or LF
    size_t nl = 1;
    for (size_t i = 0; i < n; i++) nl += s[i] == '\n' || (s[i] == '\r' && (i + 1 >= n || s[i + 1] != '\n'));
    size_t *ls = (size_t *)malloc(sizeof(size_t) * nl * 2);
    if (!ls) { free(s); return NULL; }
    size_t k = 0, from = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i < n && s[i] != '\n' && s[i] != '\r') continue;
        size_t e = i;
        ls[2 * k] = from;
        ls[2 * k + 1] = e - from;
        k++;
        if (i < n && s[i] == '\r' && i + 1 < n && s[i + 1] == '\n') i++;
        from = i + 1;
    }
    nl = k;
    // hard-wrapped (Gutenberg's 70 columns): paragraphs are runs of lines between blank lines
    size_t nonblank = 0, blank = 0, sum = 0, narrow = 0;
    for (size_t i = 0; i < nl; i++) {
        const char *p = s + ls[2 * i];
        size_t l = ls[2 * i + 1];
        while (l && (p[l - 1] == ' ' || p[l - 1] == '\t')) l--;
        if (!l) { blank++; continue; }
        nonblank++;
        sum += l;
        narrow += l <= 80 * 2;   // bytes: accented letters take two
    }
    const size_t avg = nonblank ? sum / nonblank : 0;
    // nearly every line fits a typewriter's width (a few long ones — a URL, a table — do not count)
    const int wrapped = blank > 0 && avg > 20 && avg < 85 && narrow * 10 >= nonblank * 9 && nonblank > blank * 2;
    sb_t o = {0};
    size_t i = 0;
    while (i < nl) {
        const char *p = s + ls[2 * i];
        size_t l = ls[2 * i + 1];
        while (l && (p[l - 1] == ' ' || p[l - 1] == '\t' || p[l - 1] == '\f')) l--;
        if (!l) { i++; continue; }
        // a heading: a short line on its own (a blank line before it, or the start)
        const int alone = (i == 0 || !ls[2 * (i - 1) + 1]) && (i + 1 >= nl || !ls[2 * (i + 1) + 1] || !wrapped);
        if (alone && txt_heading(p, l)) {
            sb_brk(&o, 1);
            sb_s(&o, "# ");
            size_t a = 0;
            while (a < l && p[a] == ' ') a++;
            put_plain(&o, p + a, l - a);
            // a title on the next line ("CHAPTER I" / "The Beginning") joins the heading
            if (wrapped && i + 2 < nl && !ls[2 * (i + 1) + 1] && ls[2 * (i + 2) + 1] && ls[2 * (i + 2) + 1] < 60 &&
                (i + 3 >= nl || !ls[2 * (i + 3) + 1])) {
                sb_s(&o, ". ");
                const char *q = s + ls[2 * (i + 2)];
                size_t ql = ls[2 * (i + 2) + 1], qa = 0;
                while (qa < ql && q[qa] == ' ') qa++;
                put_plain(&o, q + qa, ql - qa);
                i += 2;
            }
            sb_brk(&o, 1);
            i++;
            continue;
        }
        if (!wrapped) {   // a line is a paragraph
            put_plain(&o, p, l);
            sb_brk(&o, 1);
            i++;
            continue;
        }
        // a run of lines: verse (short or indented lines) keeps its breaks, prose is joined
        size_t j = i, shortl = 0, indented = 0, cnt = 0;
        while (j < nl && ls[2 * j + 1]) {
            const char *q = s + ls[2 * j];
            const size_t ql = ls[2 * j + 1];
            size_t a = 0;
            while (a < ql && q[a] == ' ') a++;
            if (a == ql) break;
            cnt++;
            if (ql - a < avg * 6 / 10) shortl++;
            if (a >= 2) indented++;
            j++;
        }
        const int verse = cnt >= 2 && (shortl * 10 >= (cnt - 1) * 8 || indented == cnt);
        for (size_t m = i; m < j; m++) {
            const char *q = s + ls[2 * m];
            const size_t ql = ls[2 * m + 1];
            size_t a = 0;
            while (a < ql && q[a] == ' ') a++;
            if (m > i) {
                if (verse) sb_c(&o, '\n');
                else if (o.n && o.p[o.n - 1] == '-' && o.n >= 2 && isalpha((unsigned char)o.p[o.n - 2])) o.n--;   // a hyphen at the wrap
                else sb_c(&o, ' ');
            }
            put_plain(&o, q + a, ql - a);
        }
        sb_brk(&o, 1);
        i = j;
    }
    free(ls);
    free(s);
    return sb_done(&o, out_len);
}

// ------------------------------------------------------------------------------- Markdown

// Inline Markdown: `code`, **strong**, *em* / _em_, [text](url), ![alt](src), \escapes.
static void md_inline(sb_t *o, const char *s, size_t n) {
    int bold = 0, it = 0, code = 0;
    for (size_t i = 0; i < n; i++) {
        const char c = s[i];
        if (code) {
            if (c == '`') { sb_c(o, NVHTML_CODE_OFF); code = 0; }
            else if ((unsigned char)c >= 0x20) sb_c(o, c);
            continue;
        }
        if (c == '\\' && i + 1 < n && strchr("\\`*_{}[]()#+-.!>", s[i + 1])) { sb_c(o, s[++i]); continue; }
        if (c == '`') { sb_c(o, NVHTML_CODE_ON); code = 1; continue; }
        if ((c == '*' || c == '_') && i + 1 < n && s[i + 1] == c) {
            sb_c(o, bold ? NVHTML_BOLD_OFF : NVHTML_BOLD_ON);
            bold = !bold;
            i++;
            continue;
        }
        if ((c == '*' || c == '_') && (it || (i + 1 < n && s[i + 1] != ' ' && (c == '*' || i == 0 || !isalnum((unsigned char)s[i - 1]))))) {
            sb_c(o, it ? NVHTML_ITALIC_OFF : NVHTML_ITALIC_ON);
            it = !it;
            continue;
        }
        if ((c == '[' || (c == '!' && i + 1 < n && s[i + 1] == '['))) {
            const int img = c == '!';
            const size_t t0 = i + (img ? 2 : 1);
            const char *close = memchr(s + t0, ']', n - t0);
            if (close && (size_t)(close - s) + 1 < n && close[1] == '(') {
                const size_t u0 = (size_t)(close - s) + 2;
                const char *pe = memchr(s + u0, ')', n - u0);
                if (pe) {
                    size_t ul = (size_t)(pe - (s + u0));
                    const char *sp = memchr(s + u0, ' ', ul);   // [t](url "title")
                    if (sp) ul = (size_t)(sp - (s + u0));
                    if (img) {
                        sb_c(o, NVHTML_IMAGE);
                        put_plain(o, s + u0, ul);
                        sb_s(o, "\x1E\x1E\x1E\x1E\x1E");
                        put_plain(o, s + t0, (size_t)(close - (s + t0)));
                        sb_c(o, NVHTML_IMAGE_END);
                    } else {
                        sb_c(o, NVHTML_LINK);
                        put_plain(o, s + u0, ul);
                        sb_c(o, NVHTML_LINK_TEXT);
                        md_inline(o, s + t0, (size_t)(close - (s + t0)));
                        sb_c(o, NVHTML_LINK_END);
                    }
                    i = (size_t)(pe - s);
                    continue;
                }
            }
        }
        if ((unsigned char)c >= 0x20) sb_c(o, c);
    }
    if (code) sb_c(o, NVHTML_CODE_OFF);
    if (bold) sb_c(o, NVHTML_BOLD_OFF);
    if (it) sb_c(o, NVHTML_ITALIC_OFF);
}

char *md_to_rich(const char *s, size_t n, size_t *out_len) {
    sb_t o = {0};
    sb_t para = {0};   // the paragraph being collected (lines joined)
    int fence = 0;
    size_t i = 0;
    for (;;) {
        const int end = i >= n;
        const char *p = s + (end ? n : i);
        size_t l = 0;
        if (!end) {
            const char *e = memchr(p, '\n', n - i);
            l = e ? (size_t)(e - p) : n - i;
            i += l + 1;
            if (l && p[l - 1] == '\r') l--;
        }
        size_t a = 0;
        while (a < l && p[a] == ' ') a++;
        const int blank = a == l;
        if (fence) {
            if (l - a >= 3 && !strncmp(p + a, "```", 3)) { fence = 0; sb_brk(&o, 1); continue; }
            sb_c(&o, NVHTML_PRE);
            put_plain(&o, p, l);
            sb_c(&o, '\n');
            if (end) break;
            continue;
        }
        const int fence_start = l - a >= 3 && !strncmp(p + a, "```", 3);
        int hl = 0;
        while (a + (size_t)hl < l && hl < 7 && p[a + hl] == '#') hl++;
        const int head = hl >= 1 && hl <= 6 && a + (size_t)hl < l && p[a + hl] == ' ';
        const int item = l - a >= 2 && strchr("-*+", p[a]) && p[a + 1] == ' ';
        size_t dn = 0;
        while (a + dn < l && isdigit((unsigned char)p[a + dn])) dn++;
        const int num = dn && dn < 4 && a + dn + 1 < l && (p[a + dn] == '.' || p[a + dn] == ')') && p[a + dn + 1] == ' ';
        const int quote = l > a && p[a] == '>';
        const int rule = l - a >= 3 && (!strspn(p + a, "-") ? 0 : strspn(p + a, "-* ") == l - a) && !item;
        if (end || blank || head || item || num || quote || fence_start || rule) {   // the paragraph so far ends
            if (para.n) {
                md_inline(&o, para.p, para.n);
                sb_brk(&o, 1);
                para.n = 0;
            }
        }
        if (end) break;
        if (blank) continue;
        if (fence_start) { sb_brk(&o, 1); fence = 1; continue; }
        if (rule) { sb_brk(&o, 1); sb_s(&o, "\xE2\x80\x83\xE2\x81\x82"); sb_brk(&o, 1); continue; }   // ⁂
        if (head) {
            sb_brk(&o, 1);
            sb_put(&o, "######", (size_t)hl);
            sb_c(&o, ' ');
            size_t tl = l;
            while (tl > a + (size_t)hl + 1 && (p[tl - 1] == '#' || p[tl - 1] == ' ')) tl--;
            md_inline(&o, p + a + hl + 1, tl - a - (size_t)hl - 1);
            sb_brk(&o, 1);
            continue;
        }
        if (item || num) {
            sb_brk(&o, 0);
            sb_c(&o, NVHTML_ITEM);
            for (size_t k = 0; k < a / 2; k++) sb_s(&o, "  ");
            if (item) sb_s(&o, "- ");
            else { sb_put(&o, p + a, dn); sb_s(&o, ". "); }
            const size_t t0 = a + (item ? 2 : dn + 2);
            md_inline(&o, p + t0, l - t0);
            sb_brk(&o, 0);
            continue;
        }
        if (quote) {
            sb_brk(&o, 0);
            sb_c(&o, NVHTML_DEF);
            size_t t0 = a + 1;
            while (t0 < l && p[t0] == ' ') t0++;
            sb_c(&o, NVHTML_ITALIC_ON);
            md_inline(&o, p + t0, l - t0);
            sb_c(&o, NVHTML_ITALIC_OFF);
            sb_brk(&o, 0);
            continue;
        }
        if (para.n) sb_c(&para, ' ');
        sb_put(&para, p + a, l - a);
    }
    free(para.p);
    return sb_done(&o, out_len);
}
