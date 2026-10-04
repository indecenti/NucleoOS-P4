// ANIMA facts from Wikidata — see nucleo_anima_facts.h. libc only (runs in the host harness).
#include "nucleo_anima_facts.h"
#include "nucleo_anima_lex.h"     // anima_dict_tokenize
#include "anima_lang.h"           // anima_lang_fold
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- questions --------------------------------------------------------------------------------------
// A pattern is folded tokens with '*' where the entity goes: "quando e nato *", "when was * born".
typedef struct { const char *rel, *pat; } fpat_t;

static const fpat_t PATS[] = {
    // born (date)
    {"born","quando e nato *"},{"born","quando e nata *"},{"born","quando nacque *"},{"born","in che anno e nato *"},
    {"born","in che anno e nata *"},{"born","data di nascita di *"},{"born","anno di nascita di *"},
    {"born","when was * born"},{"born","when is * born"},{"born","date of birth of *"},{"born","birth date of *"},
    {"born","cuando nacio *"},{"born","en que ano nacio *"},{"born","fecha de nacimiento de *"},
    {"born","quand est ne *"},{"born","quand est nee *"},{"born","en quelle annee est ne *"},
    {"born","en quelle annee est nee *"},{"born","date de naissance de *"},
    {"born","wann wurde * geboren"},{"born","wann ist * geboren"},{"born","geburtsdatum von *"},
    // died (date)
    {"died","quando e morto *"},{"died","quando e morta *"},{"died","quando mori *"},{"died","in che anno e morto *"},
    {"died","in che anno e morta *"},{"died","data di morte di *"},
    {"died","when did * die"},{"died","when was * died"},{"died","date of death of *"},
    {"died","cuando murio *"},{"died","en que ano murio *"},{"died","fecha de muerte de *"},
    {"died","quand est mort *"},{"died","quand est morte *"},{"died","quand est decede *"},{"died","date de deces de *"},
    {"died","wann starb *"},{"died","wann ist * gestorben"},{"died","todesdatum von *"},
    // places of birth / death
    {"birthplace","dove e nato *"},{"birthplace","dove e nata *"},{"birthplace","dove nacque *"},
    {"birthplace","luogo di nascita di *"},{"birthplace","where was * born"},{"birthplace","birthplace of *"},
    {"birthplace","donde nacio *"},{"birthplace","lugar de nacimiento de *"},{"birthplace","ou est ne *"},
    {"birthplace","ou est nee *"},{"birthplace","lieu de naissance de *"},{"birthplace","wo wurde * geboren"},
    {"birthplace","wo ist * geboren"},{"birthplace","geburtsort von *"},
    {"deathplace","dove e morto *"},{"deathplace","dove e morta *"},{"deathplace","dove mori *"},
    {"deathplace","where did * die"},{"deathplace","donde murio *"},{"deathplace","ou est mort *"},
    {"deathplace","ou est morte *"},{"deathplace","wo starb *"},{"deathplace","wo ist * gestorben"},
    // capital
    {"capital","qual e la capitale di *"},{"capital","qual e la capitale del *"},{"capital","qual e la capitale della *"},
    {"capital","qual e la capitale dell *"},{"capital","capitale di *"},{"capital","capitale del *"},
    {"capital","capitale della *"},{"capital","capitale dell *"},{"capital","capitale dello *"},
    {"capital","what is the capital of *"},{"capital","capital of *"},{"capital","cual es la capital de *"},
    {"capital","capital de *"},{"capital","quelle est la capitale de *"},{"capital","quelle est la capitale du *"},
    {"capital","capitale de *"},{"capital","capitale du *"},{"capital","was ist die hauptstadt von *"},
    {"capital","hauptstadt von *"},
    // population
    {"population","quanti abitanti ha *"},{"population","quanti abitanti ci sono a *"},
    {"population","quanti abitanti ci sono in *"},{"population","popolazione di *"},{"population","popolazione del *"},
    {"population","popolazione della *"},{"population","quante persone vivono a *"},{"population","quante persone vivono in *"},
    {"population","what is the population of *"},{"population","population of *"},{"population","how many people live in *"},
    {"population","cuantos habitantes tiene *"},{"population","poblacion de *"},{"population","combien d habitants a *"},
    {"population","combien d habitants compte *"},{"population","combien d habitants y a t il a *"},
    {"population","population de *"},{"population","wie viele einwohner hat *"},{"population","einwohnerzahl von *"},
    {"population","einwohner von *"},{"population","qual e la popolazione di *"},{"population","quanti abitanti ha la *"},
    {"population","cual es la poblacion de *"},{"population","quelle est la population de *"},
    {"population","quelle est la population du *"},{"population","wie viele menschen leben in *"},
    {"population","wie hoch ist die einwohnerzahl von *"},{"population","how many inhabitants does * have"},
    // area
    {"area","quanto e grande *"},{"area","superficie di *"},{"area","superficie del *"},{"area","superficie della *"},
    {"area","how big is *"},{"area","area of *"},{"area","que superficie tiene *"},{"area","superficie de *"},
    {"area","quelle est la superficie de *"},{"area","wie gross ist *"},{"area","flache von *"},
    {"area","what is the area of *"},{"area","qual e la superficie di *"},{"area","cual es la superficie de *"},
    // currency / language
    {"currency","qual e la moneta di *"},{"currency","moneta di *"},{"currency","moneta del *"},
    {"currency","moneta della *"},{"currency","valuta di *"},{"currency","che moneta si usa in *"},
    {"currency","what is the currency of *"},{"currency","currency of *"},{"currency","moneda de *"},
    {"currency","cual es la moneda de *"},{"currency","monnaie de *"},{"currency","quelle est la monnaie de *"},
    {"currency","wahrung von *"},{"currency","welche wahrung hat *"},
    {"language","lingua ufficiale di *"},{"language","lingua ufficiale del *"},{"language","lingua ufficiale della *"},
    {"language","che lingua si parla in *"},{"language","che lingua si parla a *"},{"language","official language of *"},
    {"language","what language is spoken in *"},{"language","idioma oficial de *"},{"language","que idioma se habla en *"},
    {"language","langue officielle de *"},{"language","quelle langue parle t on en *"},{"language","amtssprache von *"},
    {"language","welche sprache spricht man in *"},
    // where
    {"continent","in che continente si trova *"},{"continent","in quale continente si trova *"},
    {"continent","what continent is * in"},{"continent","on which continent is *"},{"continent","en que continente esta *"},
    {"continent","sur quel continent se trouve *"},{"continent","auf welchem kontinent liegt *"},
    {"country","in che paese si trova *"},{"country","in quale paese si trova *"},{"country","in che stato si trova *"},
    {"country","what country is * in"},{"country","which country is * in"},{"country","en que pais esta *"},
    {"country","dans quel pays se trouve *"},{"country","in welchem land liegt *"},
    // works
    {"author","chi ha scritto *"},{"author","autore di *"},{"author","autore del *"},{"author","autore della *"},
    {"author","chi e l autore di *"},{"author","who wrote *"},{"author","author of *"},{"author","quien escribio *"},
    {"author","autor de *"},{"author","qui a ecrit *"},{"author","auteur de *"},{"author","wer schrieb *"},
    {"author","wer hat * geschrieben"},{"author","autor von *"},{"author","who is the author of *"},
    {"author","quien es el autor de *"},{"author","qui est l auteur de *"},{"author","wer ist der autor von *"},
    {"director","chi ha diretto *"},{"director","regista di *"},{"director","who directed *"},{"director","director of *"},
    {"director","quien dirigio *"},{"director","qui a realise *"},{"director","realisateur de *"},
    {"director","wer fuhrte regie bei *"},{"director","regisseur von *"},
    {"composer","chi ha composto *"},{"composer","compositore di *"},{"composer","who composed *"},
    {"composer","composer of *"},{"composer","quien compuso *"},{"composer","qui a compose *"},
    {"composer","compositeur de *"},{"composer","wer komponierte *"},{"composer","komponist von *"},
    // founded / height / occupation
    {"founded","quando e stato fondato *"},{"founded","quando e stata fondata *"},{"founded","quando fu fondato *"},
    {"founded","quando fu fondata *"},{"founded","anno di fondazione di *"},{"founded","when was * founded"},
    {"founded","cuando se fundo *"},{"founded","cuando fue fundado *"},{"founded","cuando fue fundada *"},
    {"founded","quand a ete fonde *"},{"founded","quand a ete fondee *"},{"founded","wann wurde * gegrundet"},
    {"elevation","quanto e alto *"},{"elevation","quanto e alta *"},{"elevation","altezza del *"},
    {"elevation","altezza di *"},{"elevation","how high is *"},{"elevation","how tall is *"},{"elevation","elevation of *"},
    {"elevation","que altura tiene *"},{"elevation","altura del *"},{"elevation","quelle est la hauteur de *"},
    {"elevation","altitude de *"},{"elevation","wie hoch ist *"},
    {"occupation","che lavoro faceva *"},{"occupation","che mestiere faceva *"},{"occupation","di cosa si occupava *"},
    {"occupation","what was * job"},{"occupation","occupation of *"},{"occupation","a que se dedicaba *"},
    {"occupation","quel etait le metier de *"},{"occupation","was war * von beruf"},
};

static const char *const ARTS[] = { "il", "lo", "la", "i", "gli", "le", "l", "the", "el", "los", "las", "les", "der",
    "die", "das", "den", "dem", "a", "in", "an", "en", "au", "aux", NULL };

static const char *const PREPS[] = { "a", "in", "en", "au", "aux", NULL };

static bool in_list(const char *w, const char *const *l) { for (int i = 0; l[i]; i++) if (!strcmp(w, l[i])) return true; return false; }

const char *nucleo_anima_facts_bare(const char *key)
{
    for (int guard = 0; guard < 3; guard++) {
        const char *sp = strchr(key, ' ');
        if (!sp) break;
        char w[ANIMA_DICT_TOKLEN];
        snprintf(w, sizeof w, "%.*s", (int)(sp - key), key);
        if (!in_list(w, ARTS)) break;
        key = sp + 1;
    }
    return key;
}

// Polite openers before the question: "dimmi quando è nato...", "tell me who wrote...", "sag mir...".
static const char *const OPEN[] = { "dimmi", "sai", "mi", "dire", "dirmi", "puoi", "tell", "me", "do", "you", "know",
    "dime", "sabes", "dis", "moi", "sais", "tu", "sag", "mir", "weisst", "du", "bitte", "please", "per", "favore", NULL };

bool nucleo_anima_facts_parse(const char *q, char *rel, size_t rcap, char *key, size_t kcap)
{
    char folded[256];
    anima_lang_fold(q, folded, sizeof folded);
    char tok[ANIMA_DICT_TOKENS][ANIMA_DICT_TOKLEN];
    int n = anima_dict_tokenize(folded, tok);
    if (n < 2 || n >= ANIMA_DICT_TOKENS) return false;
    int lead = 0;
    while (lead < n - 2 && in_list(tok[lead], OPEN)) lead++;
    if (lead) { for (int i = lead; i < n; i++) memcpy(tok[i - lead], tok[i], sizeof tok[0]); n -= lead; }
    int best_s = -1, best_e = -1, best_len = 0; const char *best_rel = NULL;
    for (size_t p = 0; p < sizeof PATS / sizeof PATS[0]; p++) {
        char pt[16][ANIMA_DICT_TOKLEN]; int np = 0, star = -1;
        char buf[96]; snprintf(buf, sizeof buf, "%s", PATS[p].pat);
        for (char *t = strtok(buf, " "); t && np < 16; t = strtok(NULL, " ")) {
            if (!strcmp(t, "*")) star = np; else snprintf(pt[np++], ANIMA_DICT_TOKLEN, "%s", t);
        }
        const int pre = star, post = np - star;
        if (pre + post >= n) continue;
        bool m = true;
        for (int i = 0; i < pre && m; i++) m = !strcmp(tok[i], pt[i]);
        for (int i = 0; i < post && m; i++) m = !strcmp(tok[n - post + i], pt[pre + i]);
        if (m && np > best_len) { best_len = np; best_s = pre; best_e = n - post; best_rel = PATS[p].rel; }
    }
    if (!best_rel) return false;
    // prepositions left by the pattern go ("ci sono a Roma"); articles stay, the caller tries the key with and
    // without them ("les miserables" is a title, "la francia" is not): nucleo_anima_facts_bare()
    while (best_s < best_e - 1 && in_list(tok[best_s], PREPS)) best_s++;
    int o = 0; key[0] = 0;
    for (int i = best_s; i < best_e && o < (int)kcap - 1; i++) o += snprintf(key + o, kcap - o, "%s%s", o ? " " : "", tok[i]);
    snprintf(rel, rcap, "%s", best_rel);
    return o > 1 && o < (int)kcap;
}

// ---- facts of an entity -----------------------------------------------------------------------------

// "key=v1;v2|key2=..." -> the value of `rel` into out (";"-separated). False when absent.
static bool fact_get(const char *line, const char *rel, char *out, size_t cap)
{
    const size_t rl = strlen(rel);
    for (const char *p = line; p && *p; ) {
        const char *bar = strchr(p, '|');
        const size_t len = bar ? (size_t)(bar - p) : strlen(p);
        if (len > rl && !strncmp(p, rel, rl) && p[rl] == '=') {
            const size_t vl = len - rl - 1;
            snprintf(out, cap, "%.*s", (int)(vl < cap ? vl : cap - 1), p + rl + 1);
            return out[0] != 0;
        }
        p = bar ? bar + 1 : NULL;
    }
    return false;
}

bool nucleo_anima_facts_has(const anima_kb_ref_t *ref, const char *rel)
{
    char line[1024], v[256];
    if (!nucleo_anima_kb_facts(ref, line, sizeof line)) return false;
    if (!strcmp(rel, "born") || !strcmp(rel, "died"))       // a date or, failing that, the place still answers
        return fact_get(line, rel, v, sizeof v) || fact_get(line, !strcmp(rel, "born") ? "birthplace" : "deathplace", v, sizeof v);
    return fact_get(line, rel, v, sizeof v);
}

// ---- formatting -------------------------------------------------------------------------------------
static int L(const char *lang)
{
    return !strcmp(lang, "en") ? 1 : !strcmp(lang, "es") ? 2 : !strcmp(lang, "fr") ? 3 : !strcmp(lang, "de") ? 4 : 0;
}

static const char *const MON[5][12] = {
    { "gennaio","febbraio","marzo","aprile","maggio","giugno","luglio","agosto","settembre","ottobre","novembre","dicembre" },
    { "January","February","March","April","May","June","July","August","September","October","November","December" },
    { "enero","febrero","marzo","abril","mayo","junio","julio","agosto","septiembre","octubre","noviembre","diciembre" },
    { "janvier","février","mars","avril","mai","juin","juillet","août","septembre","octobre","novembre","décembre" },
    { "Januar","Februar","März","April","Mai","Juni","Juli","August","September","Oktober","November","Dezember" } };
static const char *const BC[5] = { " a.C.", " BC", " a. C.", " av. J.-C.", " v. Chr." };

// "1879-03-14" -> the date phrase with its preposition: "il 14 marzo 1879" / "nel 1769" / "on March 14, 1879".
static void fmt_date(const char *iso, int l, char *out, size_t cap)
{
    const bool neg = iso[0] == '-';
    int y = 0, m = 0, d = 0;
    const int k = sscanf(iso + (neg ? 1 : 0), "%d-%d-%d", &y, &m, &d);
    char ys[24]; snprintf(ys, sizeof ys, "%d%s", y, neg ? BC[l] : "");
    const bool day = k == 3 && m >= 1 && m <= 12 && d >= 1, mon = k >= 2 && m >= 1 && m <= 12;
    switch (l) {
        case 0: if (day) snprintf(out, cap, "il %d%s %s %s", d, d == 1 ? "º" : "", MON[0][m-1], ys); else if (mon) snprintf(out, cap, "nel %s %s", MON[0][m-1], ys); else snprintf(out, cap, "nel %s", ys); break;
        case 1: if (day) snprintf(out, cap, "on %s %d, %s", MON[1][m-1], d, ys); else if (mon) snprintf(out, cap, "in %s %s", MON[1][m-1], ys); else snprintf(out, cap, "in %s", ys); break;
        case 2: if (day) snprintf(out, cap, "el %d de %s de %s", d, MON[2][m-1], ys); else if (mon) snprintf(out, cap, "en %s de %s", MON[2][m-1], ys); else snprintf(out, cap, "en %s", ys); break;
        case 3: if (day) snprintf(out, cap, "le %d%s %s %s", d, d == 1 ? "er" : "", MON[3][m-1], ys); else if (mon) snprintf(out, cap, "en %s %s", MON[3][m-1], ys); else snprintf(out, cap, "en %s", ys); break;
        default: if (day) snprintf(out, cap, "am %d. %s %s", d, MON[4][m-1], ys); else if (mon) snprintf(out, cap, "im %s %s", MON[4][m-1], ys); else snprintf(out, cap, "im Jahr %s", ys); break;
    }
}

// "123802000.5" -> "123.802.000" (it/es/de), "123,802,000" (en), "123 802 000" (fr). Decimals dropped.
static void fmt_num(const char *v, int l, char *out, size_t cap)
{
    const double x = strtod(v, NULL);
    char digits[32]; snprintf(digits, sizeof digits, "%.0f", x < 0 ? -x : x);
    const char sep = l == 1 ? ',' : l == 3 ? ' ' : '.';
    const int n = (int)strlen(digits);
    size_t o = 0;
    for (int i = 0; i < n && o + 2 < cap; i++) {
        if (i && (n - i) % 3 == 0) out[o++] = sep;
        out[o++] = digits[i];
    }
    out[o] = 0;
}

// "a;b;c" -> "a, b e c" in the language.
static void fmt_list(const char *v, int l, char *out, size_t cap)
{
    static const char *const AND[5] = { " e ", " and ", " y ", " et ", " und " };
    char items[4][96]; int n = 0;
    for (const char *p = v; *p && n < 4; ) {
        const size_t k = strcspn(p, ";");
        snprintf(items[n++], sizeof items[0], "%.*s", (int)(k < 95 ? k : 95), p);
        p += k; if (*p == ';') p++;
    }
    int o = 0; out[0] = 0;
    for (int i = 0; i < n; i++)
        o += snprintf(out + o, cap - o, "%s%s", i == 0 ? "" : i == n - 1 ? AND[l] : ", ", items[i]);
}

static void entity_name(const anima_kb_ref_t *ref, char *out, size_t cap)
{
    snprintf(out, cap, "%s", ref->title);
    char *par = strstr(out, " (");                            // "Mercurio (astronomia)" -> "Mercurio"
    if (par) *par = 0;
}

int nucleo_anima_facts_answer(const char *rel, const anima_kb_ref_t *ref, const char *lang, char *out, size_t cap)
{
    char line[1024], v[256], v2[256], e[96], a[256], b[160];
    if (!nucleo_anima_kb_facts(ref, line, sizeof line)) return 0;
    entity_name(ref, e, sizeof e);
    const int l = L(lang);
    char g[4] = "";
    fact_get(line, "gender", g, sizeof g);
    const bool f = g[0] == 'f';
    if (!strcmp(rel, "born") || !strcmp(rel, "died")) {
        const bool born = rel[0] == 'b';
        const bool hd = fact_get(line, rel, v, sizeof v);
        const bool hp = fact_get(line, born ? "birthplace" : "deathplace", v2, sizeof v2);
        if (!hd && !hp) return 0;
        if (hd) fmt_date(v, l, a, sizeof a); else a[0] = 0;
        if (hp) { char t[200]; fmt_list(v2, l, t, sizeof t);
                  snprintf(b, sizeof b, "%s%s", l == 0 ? " a " : l == 3 ? " à " : " in ", t);
                  if (l == 2) snprintf(b, sizeof b, " en %s", t); }
        else b[0] = 0;
        if (born) switch (l) {
            case 0: snprintf(out, cap, "%s è %s %s%s.", e, f ? "nata" : "nato", a, b); break;
            case 1: snprintf(out, cap, "%s was born %s%s.", e, a, b); break;
            case 2: snprintf(out, cap, "%s nació %s%s.", e, a, b); break;
            case 3: snprintf(out, cap, "%s est %s %s%s.", e, f ? "née" : "né", a, b); break;
            default: snprintf(out, cap, "%s wurde %s%s geboren.", e, a, b); break;
        } else switch (l) {
            case 0: snprintf(out, cap, "%s è %s %s%s.", e, f ? "morta" : "morto", a, b); break;
            case 1: snprintf(out, cap, "%s died %s%s.", e, a, b); break;
            case 2: snprintf(out, cap, "%s murió %s%s.", e, a, b); break;
            case 3: snprintf(out, cap, "%s est %s %s%s.", e, f ? "morte" : "mort", a, b); break;
            default: snprintf(out, cap, "%s starb %s%s.", e, a, b); break;
        }
        // tidy "è nato  a Ulma" when the date is missing
        for (char *p; (p = strstr(out, "  ")); ) memmove(p, p + 1, strlen(p));
        return 1;
    }
    if (!strcmp(rel, "birthplace") || !strcmp(rel, "deathplace")) {
        const bool born = rel[0] == 'b';
        if (!fact_get(line, rel, v, sizeof v)) return 0;
        fmt_list(v, l, a, sizeof a);
        static const char *const B[5][2] = { { "%s è %s a %s.", "" }, { "%s was born in %s.", "" }, { "%s nació en %s.", "" },
                                             { "%s est %s à %s.", "" }, { "%s wurde in %s geboren.", "" } };
        static const char *const D[5] = { "%s è %s a %s.", "%s died in %s.", "%s murió en %s.", "%s est %s à %s.", "%s starb in %s." };
        const char *word = l == 0 ? (born ? (f ? "nata" : "nato") : (f ? "morta" : "morto"))
                         : l == 3 ? (born ? (f ? "née" : "né") : (f ? "morte" : "mort")) : NULL;
        const char *fmt = born ? B[l][0] : D[l];
        if (word) snprintf(out, cap, fmt, e, word, a); else snprintf(out, cap, fmt, e, a);
        return 1;
    }
    if (!fact_get(line, rel, v, sizeof v)) return 0;
    const bool many = strchr(v, ';') != NULL;
    fmt_list(v, l, a, sizeof a);
    if (!strcmp(rel, "capital")) {
        static const char *const T[5] = { "%s ha come capitale %s.", "The capital of %s is %s.", "La capital de %s es %s.",
                                          "%s — capitale : %s.", "Die Hauptstadt von %s ist %s." };
        snprintf(out, cap, T[l], e, a);
    } else if (!strcmp(rel, "population")) {
        fmt_num(v, l, b, sizeof b);
        static const char *const T[5] = { "%s ha %s abitanti.", "%s has a population of %s.", "%s tiene %s habitantes.",
                                          "%s compte %s habitants.", "%s hat %s Einwohner." };
        snprintf(out, cap, T[l], e, b);
    } else if (!strcmp(rel, "area")) {
        fmt_num(v, l, b, sizeof b);
        static const char *const T[5] = { "%s ha una superficie di %s km².", "%s has an area of %s km².",
                                          "%s tiene una superficie de %s km².", "%s a une superficie de %s km².",
                                          "%s hat eine Fläche von %s km²." };
        snprintf(out, cap, T[l], e, b);
    } else if (!strcmp(rel, "elevation")) {
        fmt_num(v, l, b, sizeof b);
        static const char *const T[5] = { "%s: %s m di altezza.", "%s is %s m high.", "%s tiene una altura de %s m.",
                                          "%s culmine à %s m.", "%s ist %s m hoch." };
        snprintf(out, cap, T[l], e, b);
    } else if (!strcmp(rel, "currency")) {
        static const char *const T[5] = { "%s — moneta: %s.", "The currency of %s is %s.", "La moneda de %s es %s.",
                                          "%s — monnaie : %s.", "Die Währung von %s ist %s." };
        snprintf(out, cap, T[l], e, a);
    } else if (!strcmp(rel, "language")) {
        static const char *const T1[5] = { "%s — lingua ufficiale: %s.", "The official language of %s is %s.",
                                           "El idioma oficial de %s es %s.", "%s — langue officielle : %s.",
                                           "Die Amtssprache von %s ist %s." };
        static const char *const TN[5] = { "%s — lingue ufficiali: %s.", "The official languages of %s are %s.",
                                           "Los idiomas oficiales de %s son %s.", "%s — langues officielles : %s.",
                                           "Die Amtssprachen von %s sind %s." };
        snprintf(out, cap, many ? TN[l] : T1[l], e, a);
    } else if (!strcmp(rel, "continent") || !strcmp(rel, "country")) {
        static const char *const T[5] = { "%s si trova in %s.", "%s is in %s.", "%s está en %s.", "%s se trouve en %s.",
                                          "%s liegt in %s." };
        if (l == 3 && rel[1] == 'o' && rel[2] == 'u') snprintf(out, cap, "%s — pays : %s.", e, a);   // "en Japon" is wrong
        else snprintf(out, cap, T[l], e, a);
    } else if (!strcmp(rel, "author")) {
        static const char *const T[5] = { "%s è stato scritto da %s.", "%s was written by %s.", "%s fue escrito por %s.",
                                          "%s a été écrit par %s.", "%s wurde von %s geschrieben." };
        snprintf(out, cap, T[l], e, a);
    } else if (!strcmp(rel, "director")) {
        static const char *const T[5] = { "%s: regia di %s.", "%s was directed by %s.", "%s fue dirigida por %s.",
                                          "%s a été réalisé par %s.", "Regie bei %s führte %s." };
        snprintf(out, cap, T[l], e, a);
    } else if (!strcmp(rel, "composer")) {
        static const char *const T[5] = { "%s: musica di %s.", "%s was composed by %s.", "%s fue compuesta por %s.",
                                          "%s a été composé par %s.", "%s wurde von %s komponiert." };
        snprintf(out, cap, T[l], e, a);
    } else if (!strcmp(rel, "founded")) {
        fmt_date(v, l, b, sizeof b);
        static const char *const T[5] = { "Fondazione di %s: %s.", "%s was founded %s.", "Fundación de %s: %s.",
                                          "Fondation de %s : %s.", "%s wurde %s gegründet." };
        // the Italian/Spanish/French label wants the bare date ("nel 1947" -> "1947")
        const char *bd = b;
        if (l == 0 || l == 2 || l == 3) { const char *sp = strchr(b, ' '); if (sp) bd = sp + 1; }
        snprintf(out, cap, T[l], e, bd);
    } else if (!strcmp(rel, "occupation")) {
        static const char *const T[5] = { "Professione di %s: %s.", "%s's occupation: %s.", "Ocupación de %s: %s.",
                                          "Profession de %s : %s.", "Beruf von %s: %s." };
        snprintf(out, cap, T[l], e, a);
    } else {
        return 0;
    }
    return 1;
}
