// ANIMA offline lexicon — see nucleo_anima_lex.h. libc only, so it runs in the host harness unchanged.
#include "nucleo_anima_lex.h"
#include "nucleo_board.h"      // NUCLEO_SD_MOUNT
#include "anima_lang.h"        // the user's language in a Spanish/French/German turn
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LEX_IT    NUCLEO_SD_MOUNT "/data/anima/lex-it.tsv"
#define LEX_EN    NUCLEO_SD_MOUNT "/data/anima/lex-en.tsv"
#define FORMS_IT  NUCLEO_SD_MOUNT "/data/anima/forms-it.tsv"
#define FORMS_EN  NUCLEO_SD_MOUNT "/data/anima/forms-en.tsv"
#define DICT_IT_EN NUCLEO_SD_MOUNT "/data/anima/dict-it-en.tsv"
#define DICT_EN_IT NUCLEO_SD_MOUNT "/data/anima/dict-en-it.tsv"

#define LEX_LINE  2048          // the generator keeps every line shorter (gen_dicts.py MAX_LINE)
#define LEX_SENSES 3            // senses shown in one reply
#define LEX_SYNS   6            // synonyms / antonyms shown

// ---- shared normalization + lookup ------------------------------------------------------------------

// Fold an Italian accented vowel (the byte AFTER 0xC3) to bare ASCII; 0 if not one we fold. Exactly the
// set a_tokenize() folds: the generator writes keys with the same table.
static char d_fold(unsigned char d)
{
    switch (d) {
        case 0xA0: case 0xA1: case 0xA2: return 'a';
        case 0xA8: case 0xA9: case 0xAA: return 'e';
        case 0xAC: case 0xAD: case 0xAE: return 'i';
        case 0xB2: case 0xB3: case 0xB4: return 'o';
        case 0xB9: case 0xBA: case 0xBB: return 'u';
        default: return 0;
    }
}

int anima_dict_tokenize(const char *in, char tok[ANIMA_DICT_TOKENS][ANIMA_DICT_TOKLEN])
{
    int n = 0, len = 0;
    char cur[ANIMA_DICT_TOKLEN];
    for (const unsigned char *p = (const unsigned char *)in; ; p++) {
        const unsigned char c = *p;
        char out = 0;
        if (c == 0xC3 && p[1]) out = d_fold(*++p);
        else if (isalnum(c))   out = (char)tolower(c);
        if (out) {
            if (len < ANIMA_DICT_TOKLEN - 1) cur[len++] = out;
        } else {
            if (len > 0 && n < ANIMA_DICT_TOKENS) { cur[len] = 0; memcpy(tok[n++], cur, len + 1); len = 0; }
            if (c == 0) break;
        }
    }
    return n;
}

// Binary search over the byte range of a key-sorted TSV: ~log2(size) seeks, each snapping to the next
// line start, then a short linear scan of the last window (robust to line-boundary edge cases).
int anima_dict_get(const char *path, const char *key, char *out, size_t cap)
{
    if (!key || !key[0]) return 0;
    FILE *f = fopen(path, "rb");                   // binary: offsets must be byte-exact
    if (!f) return 0;
    char *line = (char *)malloc(LEX_LINE);         // heap: the anima task stack is tight
    if (!line || fseek(f, 0, SEEK_END) != 0) { free(line); fclose(f); return 0; }
    long hi = ftell(f), lo = 0;
    int c, found = 0;
    while (hi - lo > 4096) {
        const long mid = lo + (hi - lo) / 2;
        fseek(f, mid, SEEK_SET);
        while ((c = fgetc(f)) != EOF && c != '\n') {}
        const long ls = ftell(f);
        if (ls >= hi || !fgets(line, LEX_LINE, f)) { hi = mid; continue; }
        char *tab = strchr(line, '\t');
        if (!tab) { lo = ftell(f); continue; }
        *tab = 0;
        if (strcmp(line, key) < 0) lo = ftell(f);  // the key sorts after this line
        else hi = mid;
    }
    // lo is always a line start here (it only moves to an ftell() right after a whole line)
    fseek(f, lo, SEEK_SET);
    long scanned = 0;
    while (scanned < 3 * LEX_LINE + 8192 && fgets(line, LEX_LINE, f)) {
        scanned += (long)strlen(line);
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        const int cmp = strcmp(line, key);
        if (cmp == 0) {
            char *val = tab + 1; size_t vl = strlen(val);
            while (vl && (val[vl-1] == '\n' || val[vl-1] == '\r')) val[--vl] = 0;
            snprintf(out, cap, "%s", val); found = 1; break;
        }
        if (cmp > 0) break;
    }
    free(line);
    fclose(f);
    return found;
}

int anima_lex_lemma(const char *key, bool it, char *out, size_t cap)
{
    char v[160];
    if (!anima_dict_get(it ? FORMS_IT : FORMS_EN, key, v, sizeof v)) return 0;
    const size_t n = strcspn(v, ",");               // the first lemma: the commonest reading
    if (!n || n >= cap) return 0;
    memcpy(out, v, n); out[n] = 0;
    return 1;
}

// ---- the lexicon tier ------------------------------------------------------------------------------

typedef enum { LX_NONE, LX_DEF, LX_SYN, LX_ANT } lx_kind_t;

static bool lx_in(const char *w, const char *const *list)
{
    for (int i = 0; list[i]; i++) if (!strcmp(w, list[i])) return true;
    return false;
}

static const char *const DEF_CUE[] = { "significa","significano","significato","significati","definizione",
    "definizioni","definisci","definiscimi","mean","means","meaning","meanings","define","definition", NULL };
static const char *const SYN_CUE[] = { "sinonimo","sinonimi","synonym","synonyms", NULL };
static const char *const ANT_CUE[] = { "contrario","contrari","opposto","opposti","antonimo","antonimi",
    "antonym","antonyms","opposite", NULL };
static const char *const OF[]      = { "di","del","dello","della","dell","dei","degli","delle","of","for","to", NULL };
static const char *const FILLER[]  = { "cosa","che","cos","e","il","lo","la","l","i","gli","le","un","una","uno",
    "parola","termine","vocabolo","word","term","the","a","an","what","whats","s","does","do","is","are",
    "mi","dimmi","dammi","qual","quale","quali","sono","puoi","potresti","per","favore","please","me","give",
    "tell","sai","dirmi","vuol","vuole","dire","in","italiano","inglese","italian","english","ci","c","ha",
    "some","any","altro","altri","un'","esiste","there", NULL };
// A pronoun is no headword to look up: "che significa questo?" asks about the conversation.
static const char *const DEICTIC[] = { "questo","questa","quello","quella","cio","this","that","it","tu","you", NULL };

// Find the request kind and the target words. "dire" is a cue only inside "vuol/vuole dire"; the
// "opposite" family only right before "di/of" ("sono contrario alla guerra" is no lexicon request).
static lx_kind_t lx_parse(const char *raw, char *target, size_t cap)
{
    char tok[ANIMA_DICT_TOKENS][ANIMA_DICT_TOKLEN];
    const int n = anima_dict_tokenize(raw, tok);
    if (n < 2 || n > 10) return LX_NONE;
    bool vuol = false;
    for (int i = 0; i < n; i++) if (!strcmp(tok[i], "vuol") || !strcmp(tok[i], "vuole")) vuol = true;
    lx_kind_t kind = LX_NONE;
    bool skip[ANIMA_DICT_TOKENS] = { false };
    for (int i = 0; i < n; i++) {
        lx_kind_t k = LX_NONE;
        if (lx_in(tok[i], DEF_CUE)) k = LX_DEF;
        else if (vuol && !strcmp(tok[i], "dire")) k = LX_DEF;
        else if (lx_in(tok[i], SYN_CUE)) k = LX_SYN;
        else if (lx_in(tok[i], ANT_CUE) && i + 1 < n && lx_in(tok[i + 1], OF)) k = LX_ANT;
        if (k == LX_NONE) continue;
        if (kind != LX_NONE && kind != k) return LX_NONE;      // two different requests: not ours
        kind = k; skip[i] = true;
    }
    if (kind == LX_NONE) return LX_NONE;
    int s = -1, e = -1;
    for (int i = 0; i < n; i++) {
        if (skip[i] || lx_in(tok[i], FILLER) || lx_in(tok[i], OF)) continue;
        if (s < 0) s = i;
        else if (e != i) return LX_NONE;                       // the target must be one contiguous run
        e = i + 1;
    }
    if (s < 0 || e - s > 3) return LX_NONE;
    if (e - s == 1 && lx_in(tok[s], DEICTIC)) return LX_NONE;
    for (int i = s; i < e; i++) if (isdigit((unsigned char)tok[i][0])) return LX_NONE;   // "cosa significa 404"
    int o = 0; target[0] = 0;
    for (int i = s; i < e; i++) o += snprintf(target + o, cap - o, "%s%s", o ? " " : "", tok[i]);
    return (o > 1 && o < (int)cap) ? kind : LX_NONE;
}

typedef struct {
    char val[LEX_LINE];        // the line after the key: senses \t syns \t ants
    char field[LEX_LINE];      // scratch for one field of val (heap with the hit, not on the task stack)
    char head[64];             // the headword actually found (the lemma, for a form)
    bool it;                   // which lexicon answered
    bool via_form;             // the asked word is an inflected form of `head`
} lx_hit_t;

// The user's language first, the other one second; exact headwords before inflected forms, so "dog"
// asked in Italian is the English noun, not some Italian form that happens to fold to "dog".
static bool lx_find(const char *key, bool prefer_it, lx_hit_t *h)
{
    const bool order[2] = { prefer_it, !prefer_it };
    for (int k = 0; k < 2; k++)
        if (anima_dict_get(order[k] ? LEX_IT : LEX_EN, key, h->val, sizeof h->val)) {
            h->it = order[k]; h->via_form = false; snprintf(h->head, sizeof h->head, "%s", key); return true;
        }
    for (int k = 0; k < 2; k++) {
        char lemma[64];
        if (anima_lex_lemma(key, order[k], lemma, sizeof lemma) && strcmp(lemma, key) &&
            anima_dict_get(order[k] ? LEX_IT : LEX_EN, lemma, h->val, sizeof h->val)) {
            h->it = order[k]; h->via_form = true; snprintf(h->head, sizeof h->head, "%s", lemma); return true;
        }
    }
    return false;
}

// Field `idx` (0 senses, 1 synonyms, 2 antonyms) of a hit, in place-safe copy.
static void lx_field(const char *val, int idx, char *out, size_t cap)
{
    const char *p = val;
    for (int i = 0; i < idx && p; i++) { p = strchr(p, '\t'); if (p) p++; }
    if (!p) { out[0] = 0; return; }
    const size_t n = strcspn(p, "\t");
    snprintf(out, cap, "%.*s", (int)(n < cap ? n : cap - 1), p);
}

// The first `max` comma-separated items of `list`.
static void lx_first(const char *list, int max, char *out, size_t cap)
{
    int o = 0, k = 0; out[0] = 0;
    for (const char *p = list; *p && k < max; ) {
        const size_t n = strcspn(p, ",");
        const char *q = p; size_t m = n;
        while (m && *q == ' ') { q++; m--; }
        if (m) { o += snprintf(out + o, cap - o, "%s%.*s", k ? ", " : "", (int)m, q); k++; }
        if (o >= (int)cap - 1) break;
        p += n; if (*p == ',') p++;
    }
}

static void lx_result(anima_result_t *r, const char *intent, int conf, const char *trace)
{
    r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = conf;
    snprintf(r->intent, sizeof r->intent, "%s", intent);
    snprintf(r->state, sizeof r->state, "tool");
    snprintf(r->trace, sizeof r->trace, "%s", trace);
}

// Build the reply for one hit. The asked word, its lemma when it was a form, its language when it is
// not the user's, a translation in that case, then the senses or the list asked for.
static void lx_reply(const char *asked, lx_hit_t *h, lx_kind_t kind, bool en, anima_result_t *r)
{
    char *buf = r->reply; const size_t cap = sizeof r->reply; int o = 0;
    char lead[200];
    const bool foreign = h->it == en;                           // an Italian word for an English user, or v.v.
    const char *lang = h->it ? (en ? "Italian" : "italiano") : (en ? "English" : "inglese");
    char shown[96];                                             // the headword as written ("però"), else the key
    lx_field(h->val, 3, shown, sizeof shown);
    if (!shown[0]) snprintf(shown, sizeof shown, "%s", h->head);
    if (h->via_form)
        snprintf(lead, sizeof lead, en ? "\"%s\" is a form of \"%s\"" : "«%s» è una forma di «%s»", asked, shown);
    else
        snprintf(lead, sizeof lead, en ? "\"%s\"" : "«%s»", shown);
    o += snprintf(buf + o, cap - o, "%s%s%s%s", lead, foreign ? " (" : "", foreign ? lang : "", foreign ? ")" : "");
    if (foreign) {                                              // say what it is in the user's language too
        char tr[300], tl[200];
        if (anima_dict_get(h->it ? DICT_IT_EN : DICT_EN_IT, h->head, tr, sizeof tr)) {
            lx_first(tr, 4, tl, sizeof tl);
            o += snprintf(buf + o, cap - o, en ? ", in English: %s" : ", in italiano: %s", tl);
        }
    }

    char *const field = h->field; const size_t fcap = sizeof h->field;
    if (kind == LX_SYN || kind == LX_ANT) {
        lx_field(h->val, kind == LX_SYN ? 1 : 2, field, fcap);
        char list[400]; lx_first(field, LEX_SYNS, list, sizeof list);
        const char *what = kind == LX_SYN ? (en ? "synonyms" : "sinonimi") : (en ? "opposites" : "contrari");
        if (list[0]) snprintf(buf + o, cap - o, en ? " — %s: %s." : " — %s: %s.", what, list);
        else snprintf(buf + o, cap - o, en ? ": I have no %s for it in the offline dictionary."
                                            : ": non ho %s nel dizionario offline.", what);
        return;
    }
    lx_field(h->val, 0, field, fcap);
    int ns = 0;
    for (const char *p = field; *p; ) { const char *q = strstr(p, " | "); ns++; if (!q) break; p = q + 3; }
    o += snprintf(buf + o, cap - o, " — ");
    int k = 0;
    for (const char *p = field; *p && k < LEX_SENSES && o < (int)cap - 1; k++) {
        const char *q = strstr(p, " | ");
        const size_t n = q ? (size_t)(q - p) : strlen(p);
        if (ns > 1) o += snprintf(buf + o, cap - o, "%s%d) %.*s", k ? "; " : "", k + 1, (int)n, p);
        else        o += snprintf(buf + o, cap - o, "%.*s", (int)n, p);
        if (!q) break;
        p = q + 3;
    }
    if (o < (int)cap - 1) o += snprintf(buf + o, cap - o, ".");
    lx_field(h->val, 1, field, fcap);
    char list[300]; lx_first(field, 4, list, sizeof list);
    if (list[0] && o < (int)cap - 1) snprintf(buf + o, cap - o, en ? " Synonyms: %s." : " Sinonimi: %s.", list);
}

static const char *lx_source(bool it, bool en)
{
    if (it) return en ? "lexicon · Wiktionary (it)" : "lessico · Wikizionario";
    return en ? "lexicon · Open English WordNet" : "lessico · Open English WordNet";
}

int nucleo_anima_lex_define(const char *word, bool en, anima_result_t *r)
{
    char tok[ANIMA_DICT_TOKENS][ANIMA_DICT_TOKLEN];
    const int n = anima_dict_tokenize(word, tok);
    if (n < 1 || n > 3) return 0;
    char key[80]; int o = 0;
    for (int i = 0; i < n; i++) o += snprintf(key + o, sizeof key - o, "%s%s", o ? " " : "", tok[i]);
    if (o < 2 || o >= (int)sizeof key) return 0;
    lx_hit_t *h = (lx_hit_t *)malloc(sizeof *h);
    if (!h) return 0;
    int ok = 0;
    if (lx_find(key, !en, h) && h->val[0] && h->val[0] != '\t') {
        lx_result(r, "define", 90, lx_source(h->it, en));
        lx_reply(key, h, LX_DEF, en, r);
        ok = 1;
    } else {
        // No definition, but a bilingual dictionary knows the word: say so, and give the translation. In a
        // Spanish/French/German turn the word is first looked up in the user's own language.
        const anima_xlang_t ux = anima_lang_current();
        if (ux != ANIMA_XL_NONE) {
            char path[96], lm[64];
            snprintf(path, sizeof path, NUCLEO_SD_MOUNT "/data/anima/dict-%s-en.tsv", anima_xlang_code(ux));
            bool got = anima_dict_get(path, key, h->field, sizeof h->field);
            if (!got) {
                char fpath[96];
                snprintf(fpath, sizeof fpath, NUCLEO_SD_MOUNT "/data/anima/forms-%s.tsv", anima_xlang_code(ux));
                if (anima_dict_get(fpath, key, lm, sizeof lm)) { lm[strcspn(lm, ",")] = 0; got = lm[0] && anima_dict_get(path, lm, h->field, sizeof h->field); }
            }
            if (got) {
                char tl[240]; lx_first(h->field, 5, tl, sizeof tl);
                lx_result(r, "define", 75, "lexicon · bilingual");
                snprintf(r->reply, sizeof r->reply, "\"%s\": I have no offline definition; in English: %s.", key, tl);
                ok = 1;
            }
        }
        const bool it_first = !en;
        for (int k = 0; k < 2 && !ok; k++) {   // (skipped when the user's own language answered)
            const bool it = k == 0 ? it_first : !it_first;
            if (!anima_dict_get(it ? DICT_IT_EN : DICT_EN_IT, key, h->field, sizeof h->field)) continue;
            char tl[240]; lx_first(h->field, 5, tl, sizeof tl);
            lx_result(r, "define", 70, en ? "lexicon · bilingual" : "lessico · bilingue");
            if (en) snprintf(r->reply, sizeof r->reply, "\"%s\": I have no offline definition; in %s: %s.",
                             key, it ? "English" : "Italian", tl);
            else    snprintf(r->reply, sizeof r->reply, "«%s»: non ho la definizione offline; in %s: %s.",
                             key, it ? "inglese" : "italiano", tl);
            ok = 1;
        }
    }
    free(h);
    return ok;
}

bool nucleo_anima_lex_is_request(const char *raw)
{
    char target[80];
    return raw && raw[0] && lx_parse(raw, target, sizeof target) != LX_NONE;
}

int nucleo_anima_lex(const char *raw, bool en, anima_result_t *r)
{
    if (!raw || !raw[0]) return 0;
    char target[80];
    const lx_kind_t kind = lx_parse(raw, target, sizeof target);
    if (kind == LX_NONE) return 0;
    if (kind == LX_DEF) return nucleo_anima_lex_define(target, en, r);
    lx_hit_t *h = (lx_hit_t *)malloc(sizeof *h);
    if (!h) return 0;
    const char *intent = kind == LX_SYN ? "synonyms" : "antonyms";
    if (lx_find(target, !en, h)) {
        lx_result(r, intent, 90, lx_source(h->it, en));
        lx_reply(target, h, kind, en, r);
    } else {                                                    // a clear request: an honest miss, not a guess
        lx_result(r, intent, 55, en ? "lexicon: absent" : "lessico: assente");
        snprintf(r->reply, sizeof r->reply, en ? "\"%s\" is not in the offline dictionary."
                                               : "«%s» non è nel dizionario offline.", target);
    }
    free(h);
    return 1;
}
