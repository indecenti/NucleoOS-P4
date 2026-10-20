// hyph.c — hyphenation by Liang's algorithm (the TeX patterns): every substring of ".word." found
// among a language's patterns adds its digits between letters; an odd value is a break point.
// The patterns are compiled into the app by gen_hyph.py (hyphdata.c, one string per language,
// patterns separated by spaces); a hash table of them is built the first time a language is used.
// Breaks leave at least 2 letters before and 3 after (the usual left/right minimums).
#include "bk.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *lang; const char *patterns; int left, right; } hyph_lang_t;
extern const hyph_lang_t bk_hyph[];   // hyphdata.c (gen_hyph.py); ends with {NULL}

#define MAXW 64
#define PAT_LETTERS 12

typedef struct {
    uint32_t key;            // hash of the letters
    uint8_t n;               // letters
    uint32_t cp[PAT_LETTERS];
    uint8_t dig[PAT_LETTERS + 1];
} pat_t;

typedef struct {
    const hyph_lang_t *l;
    pat_t *tab;
    uint32_t mask;
    int ready;               // 1 built, -1 failed
} table_t;

static table_t s_tab[16];

static uint32_t lower(uint32_t c) {
    if (c < 0x80) return (uint32_t)tolower((int)c);
    if ((c >= 0xC0 && c <= 0xDE && c != 0xD7)) return c + 0x20;
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

static uint32_t hash(const uint32_t *cp, int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) h = (h ^ cp[i]) * 16777619u;
    return h ? h : 1;
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
    // count, then fill an open-addressing table at most half full
    int count = 0;
    for (const char *p = hl->patterns; *p;) {
        while (*p == ' ') p++;
        if (!*p) break;
        count++;
        while (*p && *p != ' ') p++;
    }
    uint32_t size = 64;
    while (size < (uint32_t)count * 2) size <<= 1;
    t->tab = (pat_t *)calloc(size, sizeof(pat_t));
    if (!t->tab) return NULL;
    t->mask = size - 1;
    for (const char *p = hl->patterns; *p;) {
        while (*p == ' ') p++;
        if (!*p) break;
        pat_t pt;
        memset(&pt, 0, sizeof pt);
        const char *e = p;
        while (*e && *e != ' ') e++;
        int ok = 1;
        while (p < e) {
            if (*p >= '0' && *p <= '9') { pt.dig[pt.n] = (uint8_t)(*p - '0'); p++; continue; }
            const uint32_t c = utf8_next(&p, e);
            if (pt.n == PAT_LETTERS) { ok = 0; break; }
            pt.cp[pt.n++] = c == '.' ? '.' : lower(c);
        }
        p = e;
        if (!ok || !pt.n) continue;
        pt.key = hash(pt.cp, pt.n);
        uint32_t h = pt.key & t->mask;
        while (t->tab[h].key) h = (h + 1) & t->mask;
        t->tab[h] = pt;
    }
    t->ready = 1;
    return t;
}

int hyph_has(const char *lang) { return table_for(lang) != NULL; }

int hyph_word(const char *lang, const char *w, int n, int *out, int max) {
    table_t *t = table_for(lang);
    if (!t) return 0;
    // the word's letters (punctuation around it is not part of it) and their byte offsets
    uint32_t cp[MAXW + 2];
    int off[MAXW + 2];
    int k = 0;
    const char *p = w, *e = w + n;
    // skip leading non-letters
    const char *start = p;
    while (start < e) {
        const char *q = start;
        if (is_letter(utf8_next(&q, e))) break;
        start = q;
    }
    p = start;
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
    if (p < e) {   // something after the letters that is not trailing punctuation: no breaks
        const char *q = p;
        while (q < e) { const char *r = q; if (is_letter(utf8_next(&r, e))) return 0; q = r; }
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
            const uint32_t key = hash(cp + i, len);
            for (uint32_t h = key & t->mask; t->tab[h].key; h = (h + 1) & t->mask) {
                const pat_t *pt = &t->tab[h];
                if (pt->key != key || pt->n != len || memcmp(pt->cp, cp + i, sizeof(uint32_t) * (size_t)len)) continue;
                for (int j = 0; j <= len; j++)
                    if (pt->dig[j] > val[i + j]) val[i + j] = pt->dig[j];
                break;
            }
        }
    // val[i] sits before letter i of the dotted word (index 1 = the first letter)
    int m = 0;
    for (int i = 1 + t->l->left; i <= letters + 1 - t->l->right && m < max; i++)
        if (val[i] & 1) out[m++] = off[i];
    return m;
}
