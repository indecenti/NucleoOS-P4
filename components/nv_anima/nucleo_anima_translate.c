// ANIMA offline translation tier — see nucleo_anima_translate.h.
// Network-free (libc only), so it compiles and runs in the host harness like the profile/learn tiers.
// Self-contained: it re-tokenizes the RAW input with a byte-for-byte port of a_tokenize() (the firmware
// normalizer is static in nucleo_anima.c), so the query phrase produces the same keys the generator wrote.
#include "nucleo_anima_translate.h"
#include "nucleo_board.h"      // NUCLEO_SD_MOUNT
#include "nucleo_anima_lex.h" // shared tokenizer, binary-search lookup, lemmas
#include "anima_lang.h"       // the user's language in a Spanish/French/German turn
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdbool.h>

#define T_MAX_TOKENS ANIMA_DICT_TOKENS   // mirror firmware a_tokenize()
#define T_TOK_LEN    ANIMA_DICT_TOKLEN   // mirror firmware (cap = 23 usable chars)

#define DICT_IT_EN  NUCLEO_SD_MOUNT "/data/anima/dict-it-en.tsv"   // IT key -> EN translations
#define DICT_EN_IT  NUCLEO_SD_MOUNT "/data/anima/dict-en-it.tsv"   // EN key -> IT translations

// One normalizer for every dictionary (nucleo_anima_lex.c): the generator writes keys the same way.
static int t_tokenize(const char *in, char tok[T_MAX_TOKENS][T_TOK_LEN]) { return anima_dict_tokenize(in, tok); }

static bool t_eq(const char *a, const char *b) { return strcmp(a, b) == 0; }
static bool t_starts(const char *s, const char *pre) { return strncmp(s, pre, strlen(pre)) == 0; }

// An explicit translate verb ("traduci/traduce/traduco/tradurre/traducimi", "translate/translating").
static bool t_is_verb(const char *w)
{
    return t_starts(w, "traduc") || t_starts(w, "tradur") || t_starts(w, "translat");
}
// The noun "traduzione/translation" — a weaker trigger, accepted only WITH an explicit target language.
static bool t_is_noun(const char *w)
{
    return t_starts(w, "traduzion") || t_eq(w, "translation") || t_eq(w, "translations");
}
// A target-language word -> 'e' (translate INTO English) / 'i' (INTO Italian) / 0 (not a language).
static char t_lang(const char *w)
{
    if (t_starts(w, "ingles") || t_eq(w, "english")) return 'e';
    if (t_starts(w, "italian") || t_eq(w, "italiano")) return 'i';
    if (t_starts(w, "spagnol") || t_eq(w, "spanish") || t_eq(w, "espanol") || t_eq(w, "castellano")) return 's';
    if (t_starts(w, "frances") || t_eq(w, "french") || t_eq(w, "francais")) return 'f';
    if (t_starts(w, "tedesc") || t_eq(w, "german") || t_eq(w, "aleman") || t_eq(w, "allemand") || t_eq(w, "deutsch")) return 'd';
    return 0;
}
// Function words trimmed from the BORDERS of the target span (never from the middle, so "come stai"
// survives). Deliberately excludes come/how/say/etc. — those are real content unless part of a trigger.
static bool t_is_border_word(const char *w)
{
    static const char *fw[] = { "di","del","dello","della","dell","la","il","lo","le","l","un","una","uno",
                                "parola","parole","frase","word","words","phrase","the","a","an","mi","me",
                                "per","in","into","to","verso","nel","nella","that","this", NULL };
    for (int i = 0; fw[i]; i++) if (t_eq(w, fw[i])) return true;
    return false;
}

// Is the token sequence `pat` (nul-terminated list) present consecutively starting somewhere in tok[]?
// Returns the start index or -1. Used for the multi-word frames ("come si dice", "how do you say").
static int t_phrase(char tok[T_MAX_TOKENS][T_TOK_LEN], int ntok, const char *const *pat)
{
    int np = 0; while (pat[np]) np++;
    for (int s = 0; s + np <= ntok; s++) {
        int k = 0;
        for (; k < np; k++) if (!t_eq(tok[s + k], pat[k])) break;
        if (k == np) return s;
    }
    return -1;
}

// Exact lookup by binary search over the key-sorted SD file (anima_dict_get, shared with the lexicon).
static int t_lookup(const char *path, const char *key, char *out, size_t cap) { return anima_dict_get(path, key, out, cap); }

// ---- Spanish, French, German ------------------------------------------------------------------------
// dict-<x>-en.tsv / dict-en-<x>.tsv / forms-<x>.tsv (tools/dicts/gen_dicts.py, English Wiktionary). Italian
// reaches them through English: "cane" -> "dog" -> "perro". Language letters as t_lang(): s f d e i.

static const char *t_code(char l) { return l == 's' ? "es" : l == 'f' ? "fr" : l == 'd' ? "de" : NULL; }
static char t_letter(anima_xlang_t xl) { return xl == ANIMA_XL_ES ? 's' : xl == ANIMA_XL_FR ? 'f' : xl == ANIMA_XL_DE ? 'd' : 0; }

static const char *t_lang_name(char l, bool en)
{
    switch (l) {
        case 'e': return en ? "English" : "inglese";
        case 'i': return en ? "Italian" : "italiano";
        case 's': return en ? "Spanish" : "spagnolo";
        case 'f': return en ? "French" : "francese";
        case 'd': return en ? "German" : "tedesco";
        default:  return "";
    }
}

// Exact headword, else its lemma ("perros" -> "perro"): `used` gets the key that answered.
static bool t_get_x(const char *pair_fmt, const char *code, const char *key, char *out, size_t cap,
                    char *used, size_t ucap)
{
    char path[96];
    snprintf(path, sizeof path, pair_fmt, code);
    if (t_lookup(path, key, out, cap)) { snprintf(used, ucap, "%s", key); return true; }
    char lm[64], fpath[96];
    snprintf(fpath, sizeof fpath, NUCLEO_SD_MOUNT "/data/anima/forms-%s.tsv", code);
    if (anima_dict_get(fpath, key, lm, sizeof lm)) {
        lm[strcspn(lm, ",")] = 0;
        if (lm[0] && t_lookup(path, lm, out, cap)) { snprintf(used, ucap, "%s", lm); return true; }
    }
    return false;
}

// The first translation of a list, without the verb's "to " ("to go, to walk" -> "go").
static void t_first(const char *list, char *out, size_t cap)
{
    const char *p = list;
    if (!strncmp(p, "to ", 3)) p += 3;
    const size_t n = strcspn(p, ",");
    snprintf(out, cap, "%.*s", (int)(n < cap ? n : cap - 1), p);
}

static bool t_translate_x(const char *key, char target, bool en, anima_result_t *r)
{
    const char ux = t_letter(anima_lang_current());             // the user's language, in an es/fr/de turn
    const char *tx = t_code(target);
    char val[512], mid[512], first[96], used[64] = "";
    char src = 0;
    bool ok = false;
    if (tx) {                                                   // INTO Spanish / French / German
        const char *const EN_X = NUCLEO_SD_MOUNT "/data/anima/dict-en-%s.tsv";
        if (t_get_x(EN_X, tx, key, val, sizeof val, used, sizeof used)) { ok = true; src = 'e'; }
        else if (t_lookup(NUCLEO_SD_MOUNT "/data/anima/dict-it-en.tsv", key, mid, sizeof mid)) {
            t_first(mid, first, sizeof first);                  // Italian: through its English
            if (first[0] && t_get_x(EN_X, tx, first, val, sizeof val, used, sizeof used)) {
                ok = true; src = 'i'; snprintf(used, sizeof used, "%s", key);
            }
        }
    } else if (ux && (target == 'e' || target == 'i' || target == 0)) {   // FROM the user's language
        const char *const X_EN = NUCLEO_SD_MOUNT "/data/anima/dict-%s-en.tsv";
        if (t_get_x(X_EN, t_code(ux), key, mid, sizeof mid, used, sizeof used)) {
            src = ux;
            if (target == 'i') {
                t_first(mid, first, sizeof first);
                ok = first[0] && t_lookup(NUCLEO_SD_MOUNT "/data/anima/dict-en-it.tsv", first, val, sizeof val);
            } else { snprintf(val, sizeof val, "%s", mid); target = 'e'; ok = true; }
        }
    }
    if (!ok) return false;
    r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = src == 'i' || (src && target == 'i') ? 80 : 92;
    snprintf(r->intent, sizeof r->intent, "translate");
    snprintf(r->state, sizeof r->state, "tool");
    const char *ln = t_lang_name(target, en);
    if (strcmp(used, key))
        snprintf(r->reply, sizeof r->reply, en ? "\"%s\" (a form of \"%s\") in %s: %s." : "\"%s\" (forma di \"%s\") in %s: %s.",
                 key, used, ln, val);
    else
        snprintf(r->reply, sizeof r->reply, "\"%s\" in %s: %s.", key, ln, val);
    snprintf(r->trace, sizeof r->trace, en ? "dict lookup · %s%s" : "dizionario · %s%s", ln,
             (src == 'i' && tx) || (target == 'i' && ux) ? (en ? " (via English)" : " (via inglese)") : "");
    return true;
}

// Multi-word frames that signal a translation request ("come si dice", "how do you say"). File-scope so
// both the translator and the detect-only entry point share one definition.
static const char *const T_FR_csd[]  = { "come", "si", "dice", NULL };
static const char *const T_FR_csdn[] = { "come", "si", "dicono", NULL };
static const char *const T_FR_hdys[] = { "how", "do", "you", "say", NULL };
static const char *const T_FR_hdis[] = { "how", "do", "i", "say", NULL };
static const char *const T_FR_hts[]  = { "how", "to", "say", NULL };
static const char *const *const T_FRAMES[] = { T_FR_csd, T_FR_csdn, T_FR_hdys, T_FR_hdis, T_FR_hts, NULL };

// Mark each present frame's tokens in `excl` (when non-NULL) and return whether any frame matched.
static bool t_mark_frames(char tok[T_MAX_TOKENS][T_TOK_LEN], int ntok, bool *excl)
{
    bool found = false;
    for (int fi = 0; T_FRAMES[fi]; fi++) {
        int at = t_phrase(tok, ntok, T_FRAMES[fi]);
        if (at >= 0) { found = true; if (excl) for (int k = 0; T_FRAMES[fi][k]; k++) excl[at + k] = true; }
    }
    return found;
}

// Detect-ONLY: is `raw` a translation request? No dictionary lookup. Lets the orchestrator route to the
// online teacher (Grok) in hybrid/online mode, keeping the dictionary as the offline floor / fallback.
bool nucleo_anima_translate_is_request(const char *raw)
{
    if (!raw || !raw[0]) return false;
    char tok[T_MAX_TOKENS][T_TOK_LEN];
    int ntok = t_tokenize(raw, tok);
    if (ntok < 1) return false;
    bool verb = false, noun = false; char lang = 0;
    for (int i = 0; i < ntok; i++) {
        if (t_is_verb(tok[i])) verb = true;
        else if (t_is_noun(tok[i])) noun = true;
        char l = t_lang(tok[i]); if (l && !lang) lang = l;
    }
    return verb || t_mark_frames(tok, ntok, NULL) || (noun && lang);
}

int nucleo_anima_translate(const char *raw, bool en, anima_result_t *r)
{
    if (!raw || !raw[0]) return 0;
    char tok[T_MAX_TOKENS][T_TOK_LEN];
    int ntok = t_tokenize(raw, tok);
    if (ntok < 1) return 0;                        // empty input -> nothing to do

    // --- classify each token; detect the trigger -------------------------------------------------
    bool excl[T_MAX_TOKENS] = { 0 };
    bool verb = false, noun = false;
    char lang = 0;                                  // target language: 'e' EN, 'i' IT, 0 = auto
    for (int i = 0; i < ntok; i++) {
        if (t_is_verb(tok[i])) { excl[i] = true; verb = true; continue; }
        if (t_is_noun(tok[i])) { excl[i] = true; noun = true; continue; }
        char l = t_lang(tok[i]);
        if (l) { excl[i] = true; if (!lang) lang = l; }
    }
    // Multi-word frames. Each present frame flags the trigger and masks its own tokens (excl).
    bool phrase = t_mark_frames(tok, ntok, excl);

    bool trigger = verb || phrase || (noun && lang);
    if (!trigger) return 0;                         // not a translation request -> let the cascade run

    // --- isolate the target span: the longest run of non-excluded tokens, border-trimmed -----------
    int bs = -1, bl = 0, cs = -1, cl = 0;
    for (int i = 0; i <= ntok; i++) {
        if (i < ntok && !excl[i]) { if (cs < 0) { cs = i; cl = 1; } else cl++; }
        else { if (cl > bl) { bl = cl; bs = cs; } cs = -1; cl = 0; }
    }
    int s = bs, e = bs + bl;                        // [s, e)
    while (s < e && t_is_border_word(tok[s])) s++;  // trim leading function words
    while (e > s && t_is_border_word(tok[e - 1])) e--; // trim trailing

    // A NUMBER is not an IT<->EN word: "traduci 10 in binario" is a base conversion, "traduci 5" is nothing.
    // Decline so the math/base skill owns it — don't swallow the turn with a dictionary miss.
    for (int i = s; i < e; i++) {
        bool alldig = tok[i][0] != 0;
        for (const char *p = tok[i]; *p; p++) if (!isdigit((unsigned char)*p)) { alldig = false; break; }
        if (alldig) return 0;
    }

    r->tier = ANIMA_TIER_COMMAND;
    r->action = ANIMA_ACT_ANSWER;
    snprintf(r->intent, sizeof r->intent, "translate");
    snprintf(r->state, sizeof r->state, "tool");

    if (s >= e) {                                   // a translate verb, but nothing to translate
        r->confidence = 60;
        snprintf(r->reply, sizeof r->reply,
                 en ? "What should I translate? e.g. \"translate dog to Italian\"."
                    : "Cosa traduco? es. \"traduci cane in inglese\".");
        return 1;
    }

    // Rebuild the normalized key from the span (single-spaced) — same shape gen_dicts.py wrote.
    char key[T_MAX_TOKENS * T_TOK_LEN];
    int ko = 0;
    for (int i = s; i < e; i++) {
        if (i > s && ko < (int)sizeof key - 1) key[ko++] = ' ';
        for (const char *q = tok[i]; *q && ko < (int)sizeof key - 1; q++) key[ko++] = *q;
    }
    key[ko] = 0;

    // A single-letter source is degenerate and an accent-fold trap: "traduci è" folds to "e" and would
    // return "and" (è = copula "is", NOT the conjunction "e"/and). Decline rather than assert a false hit.
    if (ko <= 1) return 0;

    // Spanish / French / German (as the target, or as the user's own language): through English.
    if (t_translate_x(key, lang, en, r)) return 1;

    // --- lookup, honoring the requested direction (or auto-detecting it) ---------------------------
    char val[512], itv[512], env[512];
    bool to_en = false, hit = false;
    bool in_it = t_lookup(DICT_IT_EN, key, itv, sizeof itv);   // key is an Italian headword
    bool in_en = t_lookup(DICT_EN_IT, key, env, sizeof env);   // key is an English headword
    // Not a headword: maybe an inflected form ("andavo", "case", "went"). Translate its lemma and say so.
    char lemma[64]; lemma[0] = 0;
    if (!in_it && !in_en) {
        char lm[64];
        if (lang != 'i' && anima_lex_lemma(key, true, lm, sizeof lm) && t_lookup(DICT_IT_EN, lm, itv, sizeof itv)) {
            in_it = true; snprintf(lemma, sizeof lemma, "%s", lm);
        } else if (lang != 'e' && anima_lex_lemma(key, false, lm, sizeof lm) && t_lookup(DICT_EN_IT, lm, env, sizeof env)) {
            in_en = true; snprintf(lemma, sizeof lemma, "%s", lm);
        }
    }
    // HOMOGRAPH GUARD: a word that exists in BOTH languages ("male", "sole", "estate", "fame", "camera",
    // "fine") is ambiguous about its SOURCE — the engine must NOT confidently emit one assumed-source
    // reading ("translate male" / "how do you say male" -> "evil" for the English word "male"). Show BOTH
    // labelled readings so a single answer can never mislead. Applies to EVERY path (explicit direction,
    // "come si dice"/"how do you say" frames, and bare auto) — the frame/auto path set no t_lang, which is
    // exactly where the IT-first default used to surface the wrong-language sense at conf 95.
    // DIREZIONE ESPLICITA ("in inglese"/"in italiano") -> la SORGENTE e' l'ALTRA lingua, quindi NON e'
    // ambigua: traduci in quel verso. (Era il bug: "traduci cane in inglese" dava il dump omografo invece
    // di "dog" perche' la guardia omografo scattava PRIMA della direzione.)
    if (lang == 'e')      { hit = in_it; to_en = true;  if (hit) snprintf(val, sizeof val, "%s", itv); }
    else if (lang == 'i') { hit = in_en; to_en = false; if (hit) snprintf(val, sizeof val, "%s", env); }
    // AUTO (nessuna direzione): se la parola esiste in ENTRAMBE, la SORGENTE e' ambigua -> mostra i due
    // sensi etichettati per non ingannare ("translate male" -> non assumere l'EN "evil"). Solo qui.
    else if (in_it && in_en) {
        r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 75;
        snprintf(r->intent, sizeof r->intent, "translate"); snprintf(r->state, sizeof r->state, "tool");
        snprintf(r->reply, sizeof r->reply,
                 en ? "\"%s\" exists in both languages. IT->EN: %s. EN->IT: %s."
                    : "\"%s\" esiste in entrambe le lingue. IT->EN: %s. EN->IT: %s.", key, itv, env);
        snprintf(r->trace, sizeof r->trace, en ? "dict · homograph" : "dizionario · omografo");
        return 1;
    }
    else {                                          // auto, non-omografo: deduci la lingua sorgente
        if (in_it) { hit = true; to_en = true;  snprintf(val, sizeof val, "%s", itv); }
        else if (in_en) { hit = true; to_en = false; snprintf(val, sizeof val, "%s", env); }
    }

    const char *ln = to_en ? (en ? "English" : "inglese") : (en ? "Italian" : "italiano");
    if (hit) {
        r->confidence = 95;
        if (lemma[0])
            snprintf(r->reply, sizeof r->reply, en ? "\"%s\" (a form of \"%s\") in %s: %s."
                                                   : "\"%s\" (forma di \"%s\") in %s: %s.", key, lemma, ln, val);
        else
            snprintf(r->reply, sizeof r->reply, "\"%s\" %s %s: %s.", key, en ? "in" : "in", ln, val);
        snprintf(r->trace, sizeof r->trace, en ? "dict lookup · %s" : "dizionario · %s", ln);
        return 1;
    }

    // Miss on an explicit translation request: decline honestly and STOP (no later tier fabricates one).
    r->confidence = 55;
    snprintf(r->reply, sizeof r->reply,
             en ? "I don't have \"%s\" in the offline IT<->EN dictionary."
                : "Non ho \"%s\" nel dizionario offline IT<->EN.", key);
    snprintf(r->trace, sizeof r->trace, en ? "dict miss" : "dizionario: assente");
    return 1;
}
