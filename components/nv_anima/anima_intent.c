// ANIMA offline intent suggester — see anima_intent.h. Tokenization, features and FNV-1a hashing MUST stay
// byte-identical to tools/train_anima_intent.py (c_tokens / features / fnv): the host test compares the
// scores of both on golden sentences.
#include "anima_intent.h"
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define IT_MAX_TOK 24
#define IT_TOK_LEN 24
#define IT_MAX_NC  64      // classes the stack accumulator holds (the trainer has ~40)
#define IT_FEAT_TOK 12     // only the first 12 words count (commands are short; bounds the stack)

// a_tokenize() of nucleo_anima.c: Latin-1 accented vowels (UTF-8 0xC3 xx) fold to the base letter, ASCII
// alnum is lowercased, every other byte separates.
static int it_tokenize(const char *in, char tok[IT_MAX_TOK][IT_TOK_LEN])
{
    int n = 0, len = 0;
    char cur[IT_TOK_LEN];
    for (const unsigned char *p = (const unsigned char *)in; ; p++) {
        const unsigned char c = *p;
        char out = 0;
        if (c == 0xC3 && p[1]) {
            switch (*++p) {
                case 0xA0: case 0xA1: case 0xA2: out = 'a'; break;
                case 0xA8: case 0xA9: case 0xAA: out = 'e'; break;
                case 0xAC: case 0xAD: case 0xAE: out = 'i'; break;
                case 0xB2: case 0xB3: case 0xB4: out = 'o'; break;
                case 0xB9: case 0xBA: case 0xBB: out = 'u'; break;
                default: out = 0; break;
            }
        } else if (c < 128 && isalnum(c)) {
            out = (char)tolower(c);
        }
        if (out) {
            if (len < IT_TOK_LEN - 1) cur[len++] = out;
        } else {
            if (len > 0 && n < IT_MAX_TOK) { cur[len] = 0; memcpy(tok[n++], cur, (size_t)len + 1); }
            len = 0;
            if (c == 0) break;
        }
    }
    return n;
}

static uint32_t fnv_bytes(uint32_t h, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; }
    return h;
}
static uint32_t fnv_feat(const char *tag, const char *a, size_t na, const char *b)
{
    uint32_t h = fnv_bytes(2166136261u, tag, strlen(tag));
    h = fnv_bytes(h, a, na);
    if (b) { h = fnv_bytes(h, "_", 1); h = fnv_bytes(h, b, strlen(b)); }
    return h;
}

// The feature SET of the text (binary: a bucket counts once), as sorted unique bucket ids.
static int it_features(const char *text, uint16_t *f, int cap)
{
    char tok[IT_MAX_TOK][IT_TOK_LEN];
    int nt = it_tokenize(text, tok);
    if (nt > IT_FEAT_TOK) nt = IT_FEAT_TOK;
    int n = 0;
#define PUSH(h) do { if (n < cap) f[n++] = (uint16_t)((h) % (uint32_t)anima_intent_nb); } while (0)
    for (int t = 0; t < nt; t++) {
        PUSH(fnv_feat("w:", tok[t], strlen(tok[t]), NULL));
        char g[IT_TOK_LEN + 2];
        const size_t gl = (size_t)snprintf(g, sizeof g, "<%s>", tok[t]);
        for (size_t k = 3; k <= 5; k++)
            for (size_t i = 0; i + k <= gl; i++) PUSH(fnv_feat("c:", g + i, k, NULL));
    }
    for (int t = 0; t + 1 < nt; t++) PUSH(fnv_feat("b:", tok[t], strlen(tok[t]), tok[t + 1]));
#undef PUSH
    // sort + unique (n is a few hundred at most: insertion sort is fine)
    for (int i = 1; i < n; i++) { uint16_t v = f[i]; int j = i - 1; while (j >= 0 && f[j] > v) { f[j + 1] = f[j]; j--; } f[j + 1] = v; }
    int u = 0;
    for (int i = 0; i < n; i++) if (!u || f[u - 1] != f[i]) f[u++] = f[i];
    return u;
}

int anima_intent_top(const char *text, float *p_top, float *p_second)
{
    const int nc = anima_intent_nc;
    if (p_top) *p_top = 0;
    if (p_second) *p_second = 0;
    if (!text || nc <= 1 || nc > IT_MAX_NC) return -1;
    uint16_t f[800];   // 12 words x (1 + 3 n-gram sizes x <=24) + 11 pairs < 800
    const int nf = it_features(text, f, (int)(sizeof f / sizeof f[0]));
    int32_t acc[IT_MAX_NC] = { 0 };
    for (int i = 0; i < nf; i++) {
        const signed char *row = anima_intent_w + (size_t)f[i] * (size_t)nc;
        for (int c = 0; c < nc; c++) acc[c] += row[c];
    }
    float z[IT_MAX_NC], zmax = -1e30f;
    for (int c = 0; c < nc; c++) { z[c] = anima_intent_bias[c] + anima_intent_scale * (float)acc[c]; if (z[c] > zmax) zmax = z[c]; }
    float sum = 0;
    for (int c = 0; c < nc; c++) { z[c] = expf(z[c] - zmax); sum += z[c]; }
    int top = 0;
    for (int c = 1; c < nc; c++) if (z[c] > z[top]) top = c;
    float second = 0;
    for (int c = 0; c < nc; c++) if (c != top && z[c] > second) second = z[c];
    if (p_top) *p_top = z[top] / sum;
    if (p_second) *p_second = second / sum;
    return top;
}

// ── polarity guard: the same word sets as UP / DOWN / OPEN / CLOSE / TOO in train_anima_intent.py ──
static bool in_set(const char *w, const char *const *set)
{
    for (; *set; set++) if (!strcmp(*set, w)) return true;
    return false;
}
static void polarity(const char *text, int *dir, int *open)
{
    static const char *const UP[] = { "alza","alzare","alzami","aumenta","aumentare","aumentami","piu","su","forte","alto",
        "alta","chiaro","luminoso","louder","up","raise","increase","more","higher","brighter","pump", NULL };
    static const char *const DOWN[] = { "abbassa","abbassare","abbassami","diminuisci","riduci","cala","giu","meno","piano",
        "basso","bassa","scuro","quieter","down","lower","reduce","decrease","less","dim","dimmer","darker","tone","softer",
        "debole","deboli","weak","minimo","zero","muto","togli","silenzio","mute","silence","quiet", NULL };
    static const char *const OPEN[] = { "apri","aprire","aprimi","avvia","lancia","open","launch","start", NULL };
    static const char *const CLOSE[] = { "chiudi","chiudere","ferma","spegni","stoppa","stop","close","basta","kill",
        "quit","exit", NULL };
    static const char *const TOO[] = { "troppo","troppa","too", NULL };
    // a STATE ("il volume è basso") with no imperative verb and no comparative is a complaint: flip, like TOO
    static const char *const COPULA[] = { "e","is","sono","are","sembra","seems","looks", NULL };
    static const char *const VERBS[] = { "alza","alzare","alzami","aumenta","abbassa","abbassare","abbassami","diminuisci",
        "riduci","cala","metti","imposta","fai","rendi","turn","make","set","raise","lower","increase","decrease","put",
        "dim","brighten","reduce","pump","togli","spegni", NULL };
    static const char *const COMPAR[] = { "piu","meno","more","less", NULL };
    char tok[IT_MAX_TOK][IT_TOK_LEN];
    const int nt = it_tokenize(text, tok);
    bool up = false, down = false, op = false, cl = false, too = false, cop = false, verb = false, cmp = false;
    for (int t = 0; t < nt; t++) {
        up |= in_set(tok[t], UP); down |= in_set(tok[t], DOWN);
        op |= in_set(tok[t], OPEN); cl |= in_set(tok[t], CLOSE); too |= in_set(tok[t], TOO);
        cop |= in_set(tok[t], COPULA); verb |= in_set(tok[t], VERBS); cmp |= in_set(tok[t], COMPAR);
    }
    *dir = (int)up - (int)down;
    if (too || (cop && !verb && !cmp)) *dir = -*dir;
    *open = (int)op - (int)cl;
}

// A story or a negation is never a request: "ho alzato il volume ieri", "non toccare la musica".
static bool no_offer(const char *text)
{
    static const char *const PAST[] = { "alzato","abbassato","aumentato","diminuito","aperto","chiuso","spento","acceso",
        "messo","ieri","turned","lowered","raised","opened","closed","yesterday","was","were","era","erano","avevo", NULL };
    static const char *const NEG[] = { "non","dont","don","never","mai", NULL };
    char tok[IT_MAX_TOK][IT_TOK_LEN];
    const int nt = it_tokenize(text, tok);
    if (nt > 0 && in_set(tok[0], NEG)) return true;
    for (int t = 0; t < nt; t++) if (in_set(tok[t], PAST)) return true;
    return false;
}

bool anima_intent_compatible(const char *text, const char *canon)
{
    int qd, qo, cd, co;
    polarity(text, &qd, &qo);
    polarity(canon, &cd, &co);
    return !(qd && cd && qd != cd) && !(qo && co && qo != co);
}

const char *anima_intent_suggest(const char *text, bool en, float *p_out)
{
    float p = 0, p2 = 0;
    const int k = anima_intent_top(text, &p, &p2);
    if (p_out) *p_out = p;
    if (k < 0 || p < ANIMA_INTENT_P_MIN || p - p2 < ANIMA_INTENT_MARGIN) return NULL;
    const char *lab = anima_intent_label[k];
    if (!strcmp(lab, "-") || (anima_intent_en[k] != 0) != en || !anima_intent_compatible(text, lab) || no_offer(text))
        return NULL;
    return lab;
}
