// xml.c — a tolerant XML scanner for the metadata files of EPUB (container.xml, the OPF package,
// the NCX and nav tables of contents) and for FB2: a stream of tags and text, no tree, no
// validation, nothing allocated. Malformed input ends the stream early, it never reads past it.
#include "bk.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

void xml_init(xml_t *x, const char *s, size_t n) {
    x->p = s;
    x->end = s + n;
}

static const char *find(const char *p, const char *end, const char *s) {   // s inside [p, end)
    const size_t n = strlen(s);
    for (; p + n <= end; p++)
        if (*p == s[0] && !memcmp(p, s, n)) return p;
    return NULL;
}

static int is_name(char c) { return c && !strchr(" \t\r\n/>=\"'<", c); }

int xml_next(xml_t *x, xtok_t *t) {
    memset(t, 0, sizeof *t);
    for (;;) {
        const char *p = x->p, *end = x->end;
        if (p >= end) return t->type = X_END;
        if (*p != '<') {   // text up to the next tag
            const char *q = memchr(p, '<', (size_t)(end - p));
            if (!q) q = end;
            t->type = X_TEXT;
            t->text = p;
            t->tlen = (int)(q - p);
            x->p = q;
            return X_TEXT;
        }
        if (p + 4 <= end && !memcmp(p, "<!--", 4)) {
            const char *q = find(p + 4, end, "-->");
            x->p = q ? q + 3 : end;
            continue;
        }
        if (p + 9 <= end && !memcmp(p, "<![CDATA[", 9)) {
            const char *q = find(p + 9, end, "]]>");
            t->type = X_TEXT;
            t->text = p + 9;
            t->tlen = (int)((q ? q : end) - (p + 9));
            x->p = q ? q + 3 : end;
            return X_TEXT;
        }
        if (p + 1 < end && (p[1] == '?' || p[1] == '!')) {   // processing instruction, doctype
            const char *q = memchr(p, '>', (size_t)(end - p));
            x->p = q ? q + 1 : end;
            continue;
        }
        // a tag: up to its '>' outside quotes
        const char *q = p + 1;
        char quote = 0;
        while (q < end && (quote || *q != '>')) {
            if (quote) { if (*q == quote) quote = 0; }
            else if (*q == '"' || *q == '\'') quote = *q;
            q++;
        }
        if (q >= end) { x->p = end; return t->type = X_END; }
        x->p = q + 1;
        const char *s = p + 1;
        int closing = 0;
        if (s < q && *s == '/') { closing = 1; s++; }
        const char *n0 = s;
        while (s < q && is_name(*s)) s++;
        if (s == n0) continue;   // "< " and the like: not a tag
        t->name = n0;
        t->nlen = (int)(s - n0);
        const char *ae = q;
        int empty = 0;
        if (ae > s && ae[-1] == '/') { empty = 1; ae--; }
        t->attrs = s;
        t->alen = (int)(ae - s);
        t->type = closing ? X_CLOSE : empty ? X_EMPTY : X_OPEN;
        return t->type;
    }
}

static int local_eq(const char *s, int n, const char *local) {   // "dc:title" matches "title"
    const char *c = memchr(s, ':', (size_t)n);
    if (c) { n -= (int)(c + 1 - s); s = c + 1; }
    return (size_t)n == strlen(local) && !strncasecmp(s, local, (size_t)n);
}

int xml_is(const xtok_t *t, const char *local) { return t->name && local_eq(t->name, t->nlen, local); }

int xml_attr(const xtok_t *t, const char *local, const char **v, int *vn) {
    const char *p = t->attrs, *end = t->attrs + t->alen;
    while (p && p < end) {
        while (p < end && strchr(" \t\r\n", *p)) p++;
        const char *n0 = p;
        while (p < end && is_name(*p)) p++;
        const int nl = (int)(p - n0);
        while (p < end && strchr(" \t\r\n", *p)) p++;
        if (p >= end || *p != '=') {   // an attribute without a value
            if (p == n0) p++;
            continue;
        }
        p++;
        while (p < end && strchr(" \t\r\n", *p)) p++;
        const char *v0, *v1;
        if (p < end && (*p == '"' || *p == '\'')) {
            const char q = *p++;
            v0 = p;
            while (p < end && *p != q) p++;
            v1 = p;
            if (p < end) p++;
        } else {
            v0 = p;
            while (p < end && !strchr(" \t\r\n", *p)) p++;
            v1 = p;
        }
        if (nl && local_eq(n0, nl, local)) {
            *v = v0;
            *vn = (int)(v1 - v0);
            return 1;
        }
    }
    return 0;
}

// The common named entities of book metadata; numeric references in full.
static const struct { const char *name; uint32_t cp; } kEnt[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 0xA0}, {"shy", 0xAD},
    {"mdash", 0x2014}, {"ndash", 0x2013}, {"hellip", 0x2026}, {"laquo", 0xAB}, {"raquo", 0xBB},
    {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"copy", 0xA9},
    {"eacute", 0xE9}, {"egrave", 0xE8}, {"agrave", 0xE0}, {"ograve", 0xF2}, {"ugrave", 0xF9}, {"igrave", 0xEC},
};

size_t xml_text(const char *s, int n, char *out, size_t cap) {
    if (!cap) return 0;
    size_t k = 0;
    int space = 0;
    for (int i = 0; i < n && k + 5 < cap;) {
        uint32_t cp = (unsigned char)s[i];
        int adv = 1;
        if (cp == '&') {
            const char *semi = memchr(s + i, ';', (size_t)(n - i < 12 ? n - i : 12));
            if (semi) {
                const int el = (int)(semi - (s + i + 1));
                const char *e = s + i + 1;
                uint32_t v = 0;
                int ok = 0;
                if (el > 1 && e[0] == '#') {
                    const int hex = e[1] == 'x' || e[1] == 'X';
                    v = (uint32_t)strtoul(e + 1 + hex, NULL, hex ? 16 : 10);
                    ok = v > 0;
                } else {
                    for (size_t j = 0; j < sizeof kEnt / sizeof kEnt[0]; j++)
                        if ((size_t)el == strlen(kEnt[j].name) && !memcmp(e, kEnt[j].name, (size_t)el)) { v = kEnt[j].cp; ok = 1; break; }
                }
                if (ok) {
                    cp = v;
                    adv = el + 2;
                }
            }
        }
        i += adv;
        if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r') {
            space = k > 0;
            continue;
        }
        if (cp < 0x20 || cp == 0xAD) continue;
        if (space) { out[k++] = ' '; space = 0; }
        if (adv == 1 && cp >= 0x80) { out[k++] = (char)cp; continue; }   // UTF-8 bytes pass through
        k += (size_t)utf8_put(cp, out + k);
    }
    out[k] = 0;
    return k;
}

size_t xml_inner_text(xml_t *x, const xtok_t *open, char *out, size_t cap) {
    size_t k = cap ? strlen(out) : 0;
    if (open->type != X_OPEN) return k;
    int depth = 1;
    xtok_t t;
    while (depth > 0 && xml_next(x, &t) != X_END) {
        if (t.type == X_OPEN) depth++;
        else if (t.type == X_CLOSE) depth--;
        else if (t.type == X_TEXT && cap && k + 2 < cap) {
            if (k && out[k - 1] != ' ') out[k++] = ' ';
            k += xml_text(t.text, t.tlen, out + k, cap - k);
            if (k && out[k - 1] == ' ') k--;
            out[k] = 0;
        }
    }
    return k;
}
