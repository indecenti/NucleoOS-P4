// hyph.c — hyphenation by Liang's algorithm (the TeX patterns): every substring of ".word." found
// among a language's patterns adds its digits between letters; an odd value is a break point.
// The patterns are compiled into the app by gen_hyph.py (hyphdata.c, one zlib-compressed string per
// language, patterns separated by spaces). The first time a language is used its patterns are
// inflated and indexed: a sorted array of their offsets (4 bytes a pattern, searched by binary
// search on their letters), so even German's 30 000 patterns cost little memory. Breaks leave at
// least the language's left/right minimum of letters on each side.
#include "bk.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *lang; const unsigned char *z; unsigned zn, n; int left, right; } hyph_lang_t;
extern const hyph_lang_t bk_hyph[];   // hyphdata.c (gen_hyph.py); ends with {NULL}
int bk_inflate(const void *src, size_t n, void *out, size_t out_n, int zlib);

#define MAXW 64
#define PAT_LETTERS 16

typedef struct {
    const hyph_lang_t *l;
    char *text;              // the patterns, each NUL-terminated
    uint32_t *idx;           // offsets in text, sorted by the patterns' letters
    int n;
    int ready;               // 1 built, -1 failed
} table_t;

static table_t s_tab[16];
static const char *s_sort_text;

// A pattern's letters as code points ("2a1b" -> "ab"); returns how many.
static int letters_of(const char *p, uint32_t *cp, int max) {
    int k = 0;
    const char *e = p + strlen(p);
    while (p < e && k < max) {
        if (*p >= '0' && *p <= '9') { p++; continue; }
        cp[k++] = utf8_next(&p, e);
    }
    return k;
}

static int cmp_cp(const uint32_t *a, int an, const uint32_t *b, int bn) {
    for (int i = 0; i < an && i < bn; i++)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return an - bn;
}

static int cmp_pat(const void *x, const void *y) {
    uint32_t a[PAT_LETTERS], b[PAT_LETTERS];
    const int an = letters_of(s_sort_text + *(const uint32_t *)x, a, PAT_LETTERS);
    const int bn = letters_of(s_sort_text + *(const uint32_t *)y, b, PAT_LETTERS);
    return cmp_cp(a, an, b, bn);
}

static uint32_t lower(uint32_t c) {
    if (c < 0x80) return (uint32_t)tolower((int)c);
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 0x20;
    if (c >= 0x100 && c < 0x180 && !(c & 1)) return c + 1;           // Latin Extended-A pairs (most of them)
    if (c >= 0x391 && c <= 0x3AB && c != 0x3A2) return c + 0x20;     // Greek
    if (c >= 0x410 && c <= 0x42F) return c + 0x20;                    // Cyrillic
    if (c >= 0x400 && c <= 0x40F) return c + 0x50;
    return c;
}

static int is_letter(uint32_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7) ||
           (c >= 0x370 && c <= 0x3FF) || (c >= 0x400 && c <= 0x4FF) || c == 0x2019 || c == '\'';
}

static table_t *table_for(const char *lang) {
    if (!lang || !lang[0]) return NULL;
    const hyph_lang_t *hl = NULL;
    for (int i = 0; bk_hyph[i].lang; i++) {
        const size_t n = strlen(bk_hyph[i].lang);
        if (!strncmp(lang, bk_hyph[i].lang, n) && (!lang[n] || lang[n] == '-' || lang[n] == '_')) { hl = &bk_hyph[i]; break; }
    }
    if (!hl) return NULL;
    table_t *t = NULL;
    for (int i = 0; i < 16; i++) {
        if (s_tab[i].l == hl) return s_tab[i].ready > 0 ? &s_tab[i] : NULL;
        if (!s_tab[i].l && !t) t = &s_tab[i];
    }
    if (!t) return NULL;
    t->l = hl;
    t->ready = -1;
    t->text = (char *)malloc(hl->n + 1);
    if (!t->text) return NULL;
    if (bk_inflate(hl->z, hl->zn, t->text, hl->n, 1) != (int)hl->n) { free(t->text); t->text = NULL; return NULL; }
    t->text[hl->n] = 0;
    int count = 0;
    for (unsigned i = 0; i < hl->n; i++) count += t->text[i] == ' ';
    t->idx = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)(count + 1));
    if (!t->idx) { free(t->text); t->text = NULL; return NULL; }
    for (unsigned i = 0; i < hl->n;) {   // patterns NUL-terminated in place
        while (i < hl->n && t->text[i] == ' ') t->text[i++] = 0;
        if (i >= hl->n) break;
        t->idx[t->n++] = i;
        while (i < hl->n && t->text[i] != ' ') i++;
    }
    s_sort_text = t->text;
    qsort(t->idx, (size_t)t->n, sizeof(uint32_t), cmp_pat);
    t->ready = 1;
    return t;
}

int hyph_has(const char *lang) { return table_for(lang) != NULL; }

// The pattern whose letters are exactly cp[0..n), or NULL.
static const char *find(const table_t *t, const uint32_t *cp, int n) {
    int lo = 0, hi = t->n - 1;
    uint32_t b[PAT_LETTERS];
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const char *p = t->text + t->idx[mid];
        const int bn = letters_of(p, b, PAT_LETTERS);
        const int c = cmp_cp(cp, n, b, bn);
        if (!c) return p;
        if (c < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return NULL;
}

int hyph_word(const char *lang, const char *w, int n, int *out, int max) {
    table_t *t = table_for(lang);
    if (!t) return 0;
    // the word's letters (punctuation around it is not part of it) and their byte offsets
    uint32_t cp[MAXW + 2];
    int off[MAXW + 2];
    int k = 0;
    const char *p = w, *e = w + n;
    while (p < e) {   // leading punctuation
        const char *q = p;
        if (is_letter(utf8_next(&q, e))) break;
        p = q;
    }
    cp[k] = '.';
    off[k++] = (int)(p - w);
    while (p < e && k < MAXW) {
        const char *q = p;
        const uint32_t c = utf8_next(&q, e);
        if (!is_letter(c)) break;
        cp[k] = lower(c);
        off[k++] = (int)(p - w);
        p = q;
    }
    for (const char *q = p; q < e;) {   // something after the letters other than punctuation: no breaks
        const char *r = q;
        if (is_letter(utf8_next(&r, e))) return 0;
        q = r;
    }
    const int letters = k - 1;
    if (letters < t->l->left + t->l->right) return 0;
    cp[k] = '.';
    off[k] = (int)(p - w);
    k++;
    uint8_t val[MAXW + 3];
    memset(val, 0, sizeof val);
    for (int i = 0; i < k; i++)
        for (int len = 1; len <= PAT_LETTERS && i + len <= k; len++) {
            const char *pt = find(t, cp + i, len);
            if (!pt) continue;
            int pos = 0;   // digits sit before letter `pos` of the pattern
            for (const char *q = pt; *q && pos <= len;) {
                if (*q >= '0' && *q <= '9') {
                    if (*q - '0' > val[i + pos]) val[i + pos] = (uint8_t)(*q - '0');
                    q++;
                    continue;
                }
                const char *r = q;
                utf8_next(&r, q + strlen(q));
                q = r;
                pos++;
            }
        }
    // val[i] sits before letter i of the dotted word (index 1 = the first letter)
    int m = 0;
    for (int i = 1 + t->l->left; i <= letters + 1 - t->l->right && m < max; i++)
        if (val[i] & 1) out[m++] = off[i];
    return m;
}
