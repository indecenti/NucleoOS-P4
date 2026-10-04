// ANIMA context across turns: a short FRAGMENT that continues the previous turn ("e a Milano?", "e domani?",
// "e 10?", "e in metri?", "di più") is rewritten into a whole query against the previous one, before any
// tier sees it — the way a paraphrase enters as its canonical. Pure string work over the previous query,
// intent and argument; the orchestrator (nucleo_anima_query) owns the state. Maths fragments ("più 5?")
// live in anima_solve.c (anima_followup_math), next to the solver they rewrite for. See docs/ANIMA_L0.md.
#include "anima_internal.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CTX_TOK 12
#define CTX_TLEN 24
typedef struct { char t[CTX_TOK][CTX_TLEN]; int n; } ctx_toks_t;

// Lowercase, de-accent, keep letters, digits and a decimal point/comma between digits; split on the rest.
// False when the text has more words than a fragment or a remembered query can (the caller declines).
static bool ctx_split(const char *raw, ctx_toks_t *k)
{
    char f[200]; int o = 0;
    for (const unsigned char *p = (const unsigned char *)raw; *p && o < (int)sizeof f - 1; p++) {
        unsigned char c = *p; char ch = ' ';
        if (c == 0xC3 && p[1]) {
            const unsigned char d = *++p;
            ch = (d>=0xA0&&d<=0xA2)?'a':(d>=0xA8&&d<=0xAA)?'e':(d>=0xAC&&d<=0xAE)?'i':(d>=0xB2&&d<=0xB4)?'o':(d>=0xB9&&d<=0xBB)?'u':' ';
        } else if (isalnum(c)) ch = (char)tolower(c);
        else if ((c == '.' || c == ',') && o > 0 && isdigit((unsigned char)f[o-1]) && isdigit(p[1])) ch = '.';
        f[o++] = ch;
    }
    f[o] = 0;
    k->n = 0;
    for (char *w = strtok(f, " "); w; w = strtok(NULL, " ")) {
        if (k->n == CTX_TOK || strlen(w) >= CTX_TLEN) return false;
        snprintf(k->t[k->n++], CTX_TLEN, "%s", w);
    }
    return true;
}

static bool ctx_in(const char *w, const char *const *list)
{
    for (int i = 0; list[i]; i++) if (!strcmp(w, list[i])) return true;
    return false;
}
static bool ctx_is_num(const char *w)
{
    if (!isdigit((unsigned char)w[0])) return false;
    for (const char *c = w; *c; c++) if (!isdigit((unsigned char)*c) && *c != '.') return false;
    return true;
}

static const char *const GLUE[]  = { "e","ed","and","invece","allora","ora","adesso","what","how","about","anche","pure", NULL };
static const char *const PREP[]  = { "a","ad","in","di","per","at","for", NULL };
static const char *const WHEN[]  = { "oggi","domani","dopodomani","stasera","stanotte","stamattina","today","tomorrow","tonight", NULL };
// Never a place: pronouns and filler ("e tu?", "e poi?", "e allora?") must not become "che tempo fa a tu".
static const char *const NOTLOC[] = { "tu","te","io","me","lui","lei","noi","voi","loro","you","it","that","this","quello",
                                      "questo","poi","then","li","la","lo","le","ci","si","casa","home","qui","qua","here",
                                      "dove","where","cosa","what","che", NULL };
static const char *const MORE[]  = { "ancora","piu","more","again","altro","even", NULL };
static const char *const LESS[]  = { "meno","less", NULL };
static const char *const FILL[]  = { "un","a","di","po","poco","bit","little","still", NULL };

static int ctx_join(const ctx_toks_t *k, int from, int to, char *out, size_t cap, int o)
{
    for (int i = from; i < to && o < (int)cap - 1; i++) {
        const int w = snprintf(out + o, cap - o, "%s%s", o ? " " : "", k->t[i]);
        if (w < 0 || w >= (int)cap - o) return (int)cap - 1;
        o += w;
    }
    return o;
}

// A place in the previous query: the words after its LAST preposition, up to a time word. -1 = none.
static int ctx_loc_start(const ctx_toks_t *p)
{
    for (int i = p->n - 2; i >= 1; i--) if (ctx_in(p->t[i], PREP) && !ctx_in(p->t[i+1], WHEN)) return i;
    return -1;
}

// "che tempo fa a roma [domani]" + place / time -> the same question for the new place or time.
static bool ctx_place_time(const ctx_toks_t *p, const ctx_toks_t *f, int s, bool en, bool allow_bare,
                           char *out, size_t cap)
{
    const int left = f->n - s;
    // a time word alone: "e domani?" -> the previous question without its time word, plus this one
    if (left == 1 && ctx_in(f->t[s], WHEN)) {
        int o = 0; out[0] = 0;
        for (int i = 0; i < p->n; i++) if (!ctx_in(p->t[i], WHEN)) o = ctx_join(p, i, i + 1, out, cap, o);
        snprintf(out + o, cap - o, " %s", f->t[s]);
        return true;
    }
    // a place: "a milano", "in new york", or bare "new york" where the caller allows it
    int ls = s;
    if (ctx_in(f->t[s], PREP)) ls = s + 1;
    else if (!allow_bare) return false;
    if (ls >= f->n || f->n - ls > 3) return false;
    for (int i = ls; i < f->n; i++)
        if (ctx_is_num(f->t[i]) || ctx_in(f->t[i], NOTLOC) || ctx_in(f->t[i], GLUE) || ctx_in(f->t[i], WHEN)) return false;
    const int at = ctx_loc_start(p);
    int o = 0; out[0] = 0;
    if (at >= 0) o = ctx_join(p, 0, at + 1, out, cap, 0);           // keep the previous preposition
    else { o = ctx_join(p, 0, p->n, out, cap, 0); o += snprintf(out + o, cap - o, " %s", en ? "in" : "a"); }
    if (o >= (int)cap - 1) return false;
    o = ctx_join(f, ls, f->n, out, cap, o);
    for (int i = 0; i < p->n; i++) if (ctx_in(p->t[i], WHEN)) o = ctx_join(p, i, i + 1, out, cap, o);   // "domani" stays
    return o < (int)cap - 1;
}

// "converti 5 km in miglia" + "e 10?" -> "converti 10 km in miglia"; + "e in metri?" -> "converti 5 km in metri".
static bool ctx_convert(const ctx_toks_t *p, const ctx_toks_t *f, int s, char *out, size_t cap)
{
    const int left = f->n - s;
    if (left == 1 && ctx_is_num(f->t[s])) {
        int o = 0; bool done = false; out[0] = 0;
        for (int i = 0; i < p->n; i++) {
            const char *w = (!done && ctx_is_num(p->t[i])) ? (done = true, f->t[s]) : p->t[i];
            o += snprintf(out + o, cap - o, "%s%s", o ? " " : "", w);
            if (o >= (int)cap - 1) return false;
        }
        return done;
    }
    static const char *const INTO[] = { "in","to","into", NULL };
    if (left >= 2 && left <= 3 && ctx_in(f->t[s], INTO)) {
        int at = -1;
        for (int i = p->n - 2; i >= 1 && at < 0; i--) if (ctx_in(p->t[i], INTO)) at = i;
        if (at < 0) return false;
        int o = ctx_join(p, 0, at + 1, out, cap, 0);
        o = ctx_join(f, s + 1, f->n, out, cap, o);
        return o < (int)cap - 1;
    }
    return false;
}

// After a volume / brightness step: "ancora", "di più", "un altro po'" go on in the same direction (up after
// an absolute level); "di meno" always lowers. Returns the canonical command, which the rules understand.
static bool ctx_level(const char *intent, const char *arg, const ctx_toks_t *f, char *out, size_t cap, bool en)
{
    bool more = false, less = false;
    for (int i = 0; i < f->n; i++) {
        if (ctx_in(f->t[i], LESS)) less = true;
        else if (ctx_in(f->t[i], MORE)) more = true;
        else if (!ctx_in(f->t[i], FILL) && !ctx_in(f->t[i], GLUE)) return false;   // any other word: not a level fragment
    }
    if (more == less) return false;
    const bool up = less ? false : arg[0] != '-';
    const bool vol = !strcmp(intent, "set_volume");
    if (en) snprintf(out, cap, "turn the %s %s", vol ? "volume" : "brightness", up ? "up" : "down");
    else    snprintf(out, cap, "%s %s", up ? "alza" : "abbassa", vol ? "il volume" : "la luminosità");
    return true;
}

int anima_ctx_rewrite(const char *prev_q, const char *prev_intent, const char *prev_arg, const char *frag,
                      bool en, char *out, size_t cap)
{
    if (!prev_q || !prev_q[0] || !prev_intent || !prev_intent[0] || !frag || cap < 8) return 0;
    ctx_toks_t *k = (ctx_toks_t *)malloc(2 * sizeof *k);         // ~600 B: heap, not the anima task stack
    if (!k) return 0;
    ctx_toks_t *p = &k[0], *f = &k[1];
    int ok = 0;
    if (ctx_split(frag, f) && f->n > 0 && f->n <= 6 && ctx_split(prev_q, p) && p->n > 0) {
        int s = 0;
        while (s < f->n && ctx_in(f->t[s], GLUE)) s++;
        const bool glued = s > 0;
        if (s < f->n) {
            if (!strcmp(prev_intent, "weather"))
                ok = ctx_place_time(p, f, s, en, false, out, cap);
            else if (!strcmp(prev_intent, "worldclock"))
                ok = ctx_place_time(p, f, s, en, glued, out, cap);      // validated by the caller (known city)
            else if (!strcmp(prev_intent, "convert"))
                ok = ctx_convert(p, f, s, out, cap);
            else if ((!strcmp(prev_intent, "set_volume") || !strcmp(prev_intent, "set_brightness")) && prev_arg)
                ok = ctx_level(prev_intent, prev_arg, f, out, cap, en);
        }
    }
    free(k);
    return ok;
}
