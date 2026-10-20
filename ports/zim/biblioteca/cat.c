// cat.c — the Kiwix catalog for the "Download" screen (docs/ZIM_READER_PLAN.md §4f): the OPDS v2 Atom feed
// of opds.library.kiwix.org parsed into cat_t rows, the flavours of one collection grouped, and the
// Recommended list chosen by name. Pure C: the feed comes from the network, so every scan is bounded
// (tested on the PC with test/cat_test.c, fuzzed by test/fuzz.sh).
#include "bib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int is_sp(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static const char *find(const char *p, const char *e, const char *s) {
    const size_t k = strlen(s);
    for (; p && (size_t)(e - p) >= k; p++)
        if (*p == *s && !memcmp(p, s, k)) return p;
    return NULL;
}

// One UTF-8 code point into out[o..]; returns the new o (or o when it doesn't fit).
static size_t put_utf8(char *out, size_t o, size_t cap, unsigned cp) {
    char b[4];
    size_t k;
    if (cp < 0x80) { b[0] = (char)cp; k = 1; }
    else if (cp < 0x800) { b[0] = (char)(0xC0 | cp >> 6); b[1] = (char)(0x80 | (cp & 63)); k = 2; }
    else if (cp < 0x10000) { b[0] = (char)(0xE0 | cp >> 12); b[1] = (char)(0x80 | (cp >> 6 & 63)); b[2] = (char)(0x80 | (cp & 63)); k = 3; }
    else if (cp < 0x110000) { b[0] = (char)(0xF0 | cp >> 18); b[1] = (char)(0x80 | (cp >> 12 & 63)); b[2] = (char)(0x80 | (cp >> 6 & 63)); b[3] = (char)(0x80 | (cp & 63)); k = 4; }
    else return o;
    if (o + k >= cap) return o;
    memcpy(out + o, b, k);
    return o + k;
}

// Character data [s, e) into out: entities expanded, runs of white space folded to one space, trimmed,
// cut at a whole character when it doesn't fit. Control characters dropped.
static void text_to(const char *s, const char *e, char *out, size_t cap) {
    size_t o = 0;
    int sp = 0;
    if (!cap) return;
    while (s < e && is_sp(*s)) s++;
    while (s < e) {
        unsigned c = (unsigned char)*s;
        if (is_sp((char)c)) { sp = 1; s++; continue; }
        if (sp && o) o = put_utf8(out, o, cap, ' ');
        sp = 0;
        if (c == '&') {
            const char *semi = memchr(s, ';', (size_t)(e - s) < 12 ? (size_t)(e - s) : 12);
            unsigned cp = 0;
            if (semi) {
                const char *n = s + 1;
                const size_t k = (size_t)(semi - n);
                if (k == 3 && !memcmp(n, "amp", 3)) cp = '&';
                else if (k == 2 && !memcmp(n, "lt", 2)) cp = '<';
                else if (k == 2 && !memcmp(n, "gt", 2)) cp = '>';
                else if (k == 4 && !memcmp(n, "quot", 4)) cp = '"';
                else if (k == 4 && !memcmp(n, "apos", 4)) cp = '\'';
                else if (k >= 2 && n[0] == '#') cp = (unsigned)strtoul(n + 1 + (n[1] == 'x' || n[1] == 'X'), NULL,
                                                                       (n[1] == 'x' || n[1] == 'X') ? 16 : 10);
            }
            if (cp >= 0x20 && cp < 0x110000 && !(cp >= 0xD800 && cp < 0xE000)) {
                const size_t o2 = put_utf8(out, o, cap, cp);
                if (o2 == o) break;
                o = o2;
                s = semi + 1;
                continue;
            }
        }
        if (c < 0x20 || c == 0x7f) { s++; continue; }
        // a whole UTF-8 sequence or nothing
        size_t k = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if ((size_t)(e - s) < k) k = (size_t)(e - s);
        if (o + k >= cap) break;
        memcpy(out + o, s, k);
        o += k;
        s += k;
    }
    out[o] = 0;
}

// Value of attribute `want` in the tag [s, e), into out. 0 when absent.
static int attr(const char *s, const char *e, const char *want, char *out, size_t cap) {
    const size_t wl = strlen(want);
    for (const char *p = s; p + wl + 2 < e; p++) {
        if (!is_sp(*p) || memcmp(p + 1, want, wl) || p[1 + wl] != '=') continue;
        const char q = p[2 + wl];
        if (q != '"' && q != '\'') continue;
        const char *v = p + 3 + wl, *ve = memchr(v, q, (size_t)(e - v));
        if (!ve) return 0;
        text_to(v, ve, out, cap);
        return 1;
    }
    return 0;
}

static int64_t num(const char *s) {
    int64_t v = 0;
    for (; *s >= '0' && *s <= '9' && v < (INT64_MAX - 9) / 10; s++) v = v * 10 + (*s - '0');
    return v;
}

// The text of the first <tag>...</tag> inside [s, e).
static int child(const char *s, const char *e, const char *tag, char *out, size_t cap) {
    char open[32], close[32];
    snprintf(open, sizeof open, "<%s", tag);
    snprintf(close, sizeof close, "</%s>", tag);
    for (const char *p = find(s, e, open); p; p = find(p + 1, e, open)) {
        const char *gt = memchr(p, '>', (size_t)(e - p));
        if (!gt) return 0;
        const char after = p[strlen(open)];
        if (after != '>' && !is_sp(after) && after != '/') continue;   // <name> vs <namespace>
        if (gt[-1] == '/') { if (cap) out[0] = 0; return 1; }
        const char *c = find(gt + 1, e, close);
        if (!c) return 0;
        text_to(gt + 1, c, out, cap);
        return 1;
    }
    if (cap) out[0] = 0;
    return 0;
}

int cat_parse(const char *xml, size_t n, cat_t *out, int max, int *total) {
    const char *p = xml, *e = xml + n;
    int k = 0;
    char v[64];
    if (total) *total = child(xml, e, "totalResults", v, sizeof v) ? (int)num(v) : -1;
    if (!xml) return 0;
    while (k < max && (p = find(p, e, "<entry"))) {
        const char *end = find(p, e, "</entry>");
        if (!end) break;
        cat_t *c = &out[k];
        memset(c, 0, sizeof *c);
        child(p, end, "name", c->name, sizeof c->name);
        child(p, end, "flavour", c->flavour, sizeof c->flavour);
        child(p, end, "title", c->title, sizeof c->title);
        child(p, end, "summary", c->summary, sizeof c->summary);
        child(p, end, "language", c->lang, sizeof c->lang);
        child(p, end, "category", c->category, sizeof c->category);
        child(p, end, "updated", v, sizeof v);
        memcpy(c->date, v, 10);
        c->date[10] = 0;
        if (child(p, end, "author", v, sizeof v)) {
            const char *a = find(p, end, "<author");
            const char *ae = a ? find(a, end, "</author>") : NULL;
            if (ae) child(a, ae, "name", c->author, sizeof c->author);
        }
        char tags[256];
        child(p, end, "tags", tags, sizeof tags);
        c->pictures = strstr(tags, "_pictures:no") == NULL;
        c->videos = strstr(tags, "_videos:yes") != NULL;
        child(p, end, "articleCount", v, sizeof v); c->articles = (int32_t)num(v);
        child(p, end, "mediaCount", v, sizeof v);   c->media = (int32_t)num(v);
        // links: the 48 px thumbnail and the .meta4 with the size
        for (const char *l = find(p, end, "<link"); l; l = find(l + 1, end, "<link")) {
            const char *gt = memchr(l, '>', (size_t)(end - l));
            if (!gt) break;
            char rel[96], type[48], href[sizeof c->meta4];
            if (!attr(l, gt, "href", href, sizeof href)) continue;
            attr(l, gt, "rel", rel, sizeof rel) || (rel[0] = 0);
            attr(l, gt, "type", type, sizeof type) || (type[0] = 0);
            const size_t hl = strlen(href);
            if (!strcmp(rel, "http://opds-spec.org/image/thumbnail") && href[0] == '/' && hl < sizeof c->icon)
                memcpy(c->icon, href, hl + 1);
            else if (!strcmp(type, "application/x-zim") && !strncmp(href, "https://", 8)) {
                snprintf(c->meta4, sizeof c->meta4, "%s", href);
                if (attr(l, gt, "length", v, sizeof v)) c->size = (uint64_t)num(v);
            }
        }
        p = end + 8;
        if (c->name[0] && c->meta4[0] && strlen(c->meta4) > 6 && !strcmp(c->meta4 + strlen(c->meta4) - 6, ".meta4")) k++;
    }
    return k;
}

// "https://.../wikipedia_it_top_nopic_2026-07.zim.meta4" -> "wikipedia_it_top_nopic_2026-07.zim"
int cat_file_name(const cat_t *c, char *out, size_t cap) {
    const char *s = strrchr(c->meta4, '/');
    if (!s || cap < 2) return 0;
    s++;
    const size_t n = strlen(s);
    if (n <= 6 || n - 6 >= cap) return 0;
    memcpy(out, s, n - 6);
    out[n - 6] = 0;
    for (const char *q = out; *q; q++)
        if (*q == '/' || *q == '\\' || (unsigned char)*q < ' ' || strchr(":*?\"<>|", *q)) return 0;
    return n - 6 > 4 && !strcmp(out + n - 10, ".zim");
}

// The language part of a name ("wikipedia_it_top" -> "it"), "" if none.
static void name_lang(const char *name, char *out, size_t cap) {
    const char *u = strchr(name, '_');
    const char *v = u ? strchr(u + 1, '_') : NULL;
    size_t k = u && v ? (size_t)(v - u - 1) : 0;
    if (k >= cap) k = 0;
    memcpy(out, u ? u + 1 : "", k);
    out[k] = 0;
}

// Rank of a collection in the Recommended list for language `l2` ("it"): lower first, -1 = not in it.
// What a person with no Internet wants first: the best of Wikipedia, medicine, a dictionary, travel,
// for children, then the bigger and the rarer.
int cat_recommended(const char *name, const char *l2) {
    static const char *const kOrder[] = {
        "wikipedia_%s_top", "wikipedia_%s_medicine", "wiktionary_%s_all", "wikivoyage_%s_all", "vikidia_%s_all",
        "wikiquote_%s_all", "wikibooks_%s_all", "wikiversity_%s_all", "wikisource_%s_all", "gutenberg_%s_all",
        "wikipedia_%s_all", "mdwiki_%s_all", "ifixit_%s_all", "wikinews_%s_all",
    };
    char want[64], l[8];
    name_lang(name, l, sizeof l);
    if (strcmp(l, l2)) return -1;
    for (size_t i = 0; i < sizeof kOrder / sizeof kOrder[0]; i++) {
        snprintf(want, sizeof want, kOrder[i], l2);
        if (!strcmp(name, want)) return (int)i;
    }
    return -1;
}

// Which flavour of a collection a list offers first (lower first): with pictures up to 1 GB, else
// without pictures up to 2 GB, else the mini (introductions only) - a 16 GB card holds a few of those.
// The detail page offers every flavour.
int cat_flavour_rank(const cat_t *c) {
    const int maxi = !strcmp(c->flavour, "maxi") || !c->flavour[0], nopic = !strcmp(c->flavour, "nopic");
    if (maxi && c->size <= (1ull << 30)) return 0;
    if (nopic && c->size <= (2ull << 30)) return 1;
    if (!strcmp(c->flavour, "mini")) return 2;
    return nopic ? 3 : maxi ? 4 : 5;
}
