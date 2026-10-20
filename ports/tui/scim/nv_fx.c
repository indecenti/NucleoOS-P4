/* nv_fx.c — Excel-style formulas over sc-im's expressions, both ways.
 *
 * What people type (and what .xlsx files hold) is Excel's notation: rows from 1, SUM()/AVERAGE()/
 * IF(), "&" to join text, ";" between arguments in most European locales with a decimal comma,
 * and the function names of the user's own language (SOMMA, SUMA, SOMME, SUMME...). sc-im's own
 * expressions count rows from 0 and spell functions @sum/@avg/@if. These helpers translate one into
 * the other; a formula they cannot express exactly (another sheet, a whole column, an unknown
 * function) is refused rather than guessed, so the caller can keep the value instead.
 *
 *   nv_fx_user_to_sc   what the user typed after '=' (any of the 5 languages) -> sc-im
 *   nv_fx_sc_to_user   sc-im -> what the formula bar shows (the UI language's names and separators)
 *   nv_fx_excel_to_sc  a formula read from an .xlsx (English names) -> sc-im, `rowoff` added
 *   nv_fx_sc_to_excel  sc-im -> an .xlsx formula (English names), `rowoff` added to every row
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "nv_fx.h"

/* sc-im name, then the Excel name in en it es fr de (the first is what .xlsx files store) */
static const char * const kFn[][6] = {
    {"sum",    "SUM",     "SOMMA",        "SUMA",      "SOMME",      "SUMME"},
    {"avg",    "AVERAGE", "MEDIA",        "PROMEDIO",  "MOYENNE",    "MITTELWERT"},
    {"count",  "COUNT",   "CONTA.NUMERI", "CONTAR",    "NB",         "ANZAHL"},
    {"max",    "MAX",     "MAX",          "MAX",       "MAX",        "MAX"},
    {"min",    "MIN",     "MIN",          "MIN",       "MIN",        "MIN"},
    {"prod",   "PRODUCT", "PRODOTTO",     "PRODUCTO",  "PRODUIT",    "PRODUKT"},
    {"round",  "ROUND",   "ARROTONDA",    "REDONDEAR", "ARRONDI",    "RUNDEN"},
    {"abs",    "ABS",     "ASS",          "ABS",       "ABS",        "ABS"},
    {"sqrt",   "SQRT",    "RADQ",         "RAIZ",      "RACINE",     "WURZEL"},
    {"pow",    "POWER",   "POTENZA",      "POTENCIA",  "PUISSANCE",  "POTENZ"},
    {"if",     "IF",      "SE",           "SI",        "SI",         "WENN"},
    {"stddev", "STDEV",   "DEV.ST",       "DESVEST",   "ECARTYPE",   "STABW"},
    {"floor",  "INT",     "INT",          "ENTERO",    "ENT",        "GANZZAHL"},
    {"exp",    "EXP",     "EXP",          "EXP",       "EXP",        "EXP"},
    {"ln",     "LN",      "LN",           "LN",        "LN",         "LN"},
    {"log",    "LOG10",   "LOG10",        "LOG10",     "LOG10",      "LOG10"},
    {"now",    "NOW",     "ADESSO",       "AHORA",     "MAINTENANT", "JETZT"},
    {"year",   "YEAR",    "ANNO",         "AÑO",       "ANNEE",      "JAHR"},
    {"month",  "MONTH",   "MESE",         "MES",       "MOIS",       "MONAT"},
    {"day",    "DAY",     "GIORNO",       "DIA",       "JOUR",       "TAG"},
    {"hour",   "HOUR",    "ORA",          "HORA",      "HEURE",      "STUNDE"},
    {"minute", "MINUTE",  "MINUTO",       "MINUTO",    "MINUTE",     "MINUTE"},
    {"second", "SECOND",  "SECONDO",      "SEGUNDO",   "SECONDE",    "SEKUNDE"},
    {"upper",  "UPPER",   "MAIUSC",       "MAYUSC",    "MAJUSCULE",  "GROSS"},
    {"lower",  "LOWER",   "MINUSC",       "MINUSC",    "MINUSCULE",  "KLEIN"},
    {"slen",   "LEN",     "LUNGHEZZA",    "LARGO",     "NBCAR",      "LAENGE"},
};
/* other spellings accepted on input (Excel's newer names, accented forms) */
static const char * const kAlias[][2] = {
    {"STDEV.S", "stddev"}, {"_XLFN.STDEV.S", "stddev"}, {"DEV.ST.C", "stddev"}, {"DESVEST.M", "stddev"},
    {"ECARTYPE.STANDARD", "stddev"}, {"STABW.S", "stddev"}, {"ANNÉE", "year"}, {"DÍA", "day"},
    {"LÄNGE", "slen"}, {"GROSS2", "upper"}, {"AVG", "avg"}, {"MEAN", "avg"},
};
#define NFN (sizeof(kFn) / sizeof(kFn[0]))

typedef struct { char *p; size_t n, cap; int bad; } out_t;

static void put(out_t *o, const char *s, size_t n) {
    if (o->n + n + 1 > o->cap) { o->bad = 1; return; }
    memcpy(o->p + o->n, s, n);
    o->n += n;
    o->p[o->n] = '\0';
}
static void puts_(out_t *o, const char *s) { put(o, s, strlen(s)); }

/* An ASCII-uppercased copy of a name (UTF-8 bytes above 0x7f kept as they are) */
static void upper(char *d, const char *s, size_t n, size_t cap) {
    size_t i;
    for (i = 0; i < n && i + 1 < cap; i++) d[i] = (char) toupper((unsigned char) s[i]);
    d[i] = '\0';
}

static const char *fn_to_sc(const char *name) {
    for (size_t i = 0; i < NFN; i++)
        for (int l = 1; l < 6; l++)
            if (! strcasecmp(name, kFn[i][l])) return kFn[i][0];
    for (size_t i = 0; i < sizeof(kAlias) / sizeof(kAlias[0]); i++)
        if (! strcasecmp(name, kAlias[i][0])) return kAlias[i][1];
    return NULL;
}

static const char *fn_from_sc(const char *sc, int lang) {
    for (size_t i = 0; i < NFN; i++)
        if (! strcasecmp(sc, kFn[i][0])) return kFn[i][1 + (lang >= 0 && lang < 5 ? lang : 0)];
    return NULL;
}

int nv_fx_lang(const char *code) {
    static const char *const k[] = {"en", "it", "es", "fr", "de"};
    for (int i = 0; i < 5; i++) if (code && ! strncasecmp(code, k[i], 2)) return i;
    return 0;
}

/* A cell reference at s: [$]letters[$]digits, not followed by a name character. Returns its
 * length, the column part's length in *collen and the row in *row. */
static size_t cellref(const char *s, size_t *collen, long *row) {
    size_t i = 0;
    if (s[i] == '$') i++;
    size_t l0 = i;
    while (isalpha((unsigned char) s[i]) && i - l0 < 4) i++;
    if (i == l0 || i - l0 > 3) return 0;
    *collen = i;
    if (s[i] == '$') i++;
    if (! isdigit((unsigned char) s[i])) return 0;
    char *e;
    *row = strtol(s + i, &e, 10);
    i = (size_t) (e - s);
    if (isalnum((unsigned char) s[i]) || s[i] == '_' || s[i] == '.' || s[i] == '(' || s[i] == '!') return 0;
    return i;
}

/* Copies one reference, its row moved by `off`. A row that would fall before the first refuses. */
static int put_ref(out_t *o, const char *s, size_t collen, long row, long off) {
    char col[8];
    upper(col, s, collen, sizeof col);
    long r = row + off;
    if (r < 0) return -1;
    char b[24];
    snprintf(b, sizeof b, "%s%s%ld", col, s[collen] == '$' ? "$" : "", r);
    puts_(o, b);
    return 0;
}

static int to_sc(const char *s, out_t *o, long off, int semi);

/* A cell or a range of cells, and nothing else (spaces around allowed) */
static int is_ref_arg(const char *s) {
    size_t cl;
    long row;
    while (*s == ' ') s++;
    size_t n = cellref(s, &cl, &row);
    if (! n) return 0;
    s += n;
    if (*s == ':') {
        s++;
        if (! (n = cellref(s, &cl, &row))) return 0;
        s += n;
    }
    while (*s == ' ') s++;
    return *s == '\0';
}

/* A function call: `a` is just past '('. Splits the arguments at the top level, translates each
 * one, and writes the sc-im form; Excel's many-argument SUM/AVERAGE/COUNT/PRODUCT/MAX/MIN become
 * sc-im's one-range functions combined. *end is set past the closing ')'. */
#define MAXARGS 30
static int call_to_sc(const char *f, const char *a, const char **end, out_t *o, long off, int semi) {
    const char *st[MAXARGS], *p = a;
    size_t len[MAXARGS];
    int n = 0, depth = 0, q = 0;
    const char *arg0 = a;
    for (;; p++) {
        if (! *p) return -1;
        if (q) { if (*p == '"') q = 0; continue; }
        if (*p == '"') { q = 1; continue; }
        if (*p == '(') { depth++; continue; }
        if (*p == ')' && depth) { depth--; continue; }
        if ((*p == ')' && ! depth) || (! depth && (*p == ';' || (*p == ',' && ! semi)))) {
            if (n == MAXARGS) return -1;
            st[n] = arg0;
            len[n] = (size_t) (p - arg0);
            n++;
            arg0 = p + 1;
            if (*p == ')') break;
        }
    }
    *end = p + 1;
    if (n == 1) {                                      /* f() */
        const char *t = st[0];
        while (t < st[0] + len[0] && *t == ' ') t++;
        if (t == st[0] + len[0]) n = 0;
    }
    char *tr[MAXARGS];
    int refs[MAXARGS];
    int rc = 0;
    for (int i = 0; i < n; i++) tr[i] = NULL;
    for (int i = 0; i < n && ! rc; i++) {
        char *src = malloc(len[i] + 1);
        tr[i] = malloc(4096);
        if (! src || ! tr[i]) { free(src); rc = -1; break; }
        memcpy(src, st[i], len[i]);
        src[len[i]] = '\0';
        refs[i] = is_ref_arg(src);
        out_t sub = {tr[i], 0, 4096, 0};
        tr[i][0] = '\0';
        rc = to_sc(src, &sub, off, semi);
        free(src);
    }
    if (! rc) {
        const int multi = n > 1 && (! strcmp(f, "sum") || ! strcmp(f, "avg") || ! strcmp(f, "count") ||
                                    ! strcmp(f, "prod") || ! strcmp(f, "max") || ! strcmp(f, "min"));
        if (! strcmp(f, "now")) {
            puts_(o, "@now");
        } else if (! multi) {
            puts_(o, "@"); puts_(o, f); puts_(o, "(");
            for (int i = 0; i < n; i++) { if (i) puts_(o, ","); puts_(o, tr[i]); }
            puts_(o, ")");
        } else if (! strcmp(f, "max") || ! strcmp(f, "min")) {
            puts_(o, "@"); puts_(o, f); puts_(o, "(");
            for (int i = 0; i < n; i++) {
                if (i) puts_(o, ",");
                if (refs[i]) { puts_(o, "@"); puts_(o, f); puts_(o, "("); puts_(o, tr[i]); puts_(o, ")"); }
                else puts_(o, tr[i]);
            }
            puts_(o, ")");
        } else if (! strcmp(f, "avg")) {
            puts_(o, "((");
            for (int i = 0; i < n; i++) {
                if (i) puts_(o, "+");
                if (refs[i]) { puts_(o, "@sum("); puts_(o, tr[i]); puts_(o, ")"); }
                else { puts_(o, "("); puts_(o, tr[i]); puts_(o, ")"); }
            }
            puts_(o, ")/(");
            for (int i = 0; i < n; i++) {
                if (i) puts_(o, "+");
                if (refs[i]) { puts_(o, "@count("); puts_(o, tr[i]); puts_(o, ")"); }
                else puts_(o, "1");
            }
            puts_(o, "))");
        } else {                                       /* sum, count, prod */
            const char *op = ! strcmp(f, "prod") ? "*" : "+";
            puts_(o, "(");
            for (int i = 0; i < n; i++) {
                if (i) puts_(o, op);
                if (refs[i]) { puts_(o, "@"); puts_(o, f); puts_(o, "("); puts_(o, tr[i]); puts_(o, ")"); }
                else if (! strcmp(f, "count")) puts_(o, "1");
                else { puts_(o, "("); puts_(o, tr[i]); puts_(o, ")"); }
            }
            puts_(o, ")");
        }
    }
    for (int i = 0; i < n; i++) free(tr[i]);
    return rc ? -1 : 0;
}

/* ---- Excel notation -> sc-im ----------------------------------------------------------------
 * `semi`: ';' separates arguments and ',' is the decimal mark (decided once per formula).
 * `en_only`: only the English names (a file), else any of the five languages. */
static int to_sc(const char *s, out_t *o, long off, int semi) {
    while (*s && ! o->bad) {
        unsigned char ch = (unsigned char) *s;
        if (ch == ' ' || ch == '\t') { put(o, " ", 1); s++; continue; }
        if (ch == '"') {                                   /* a text constant */
            const char *e = s + 1;
            while (*e && *e != '"') e++;
            if (! *e || e[1] == '"') return -1;             /* unterminated, or "" inside: no way to say it */
            put(o, s, (size_t) (e - s + 1));
            s = e + 1;
            continue;
        }
        if (isdigit(ch) || (ch == '.' && isdigit((unsigned char) s[1])) ||
            (ch == ',' && semi && isdigit((unsigned char) s[1]))) {
            char num[64];
            size_t k = 0;
            while (k + 1 < sizeof num && (isdigit((unsigned char) *s) || *s == '.' || (semi && *s == ',') ||
                   ((*s == 'e' || *s == 'E') && k && isdigit((unsigned char) s[1])) ||
                   ((*s == '+' || *s == '-') && k && (num[k-1] == 'e' || num[k-1] == 'E')))) {
                num[k++] = *s == ',' ? '.' : *s;
                s++;
            }
            num[k] = '\0';
            if (*s == '%') {                                 /* 10% is 0.1 */
                char b[80];
                snprintf(b, sizeof b, "%.15g", strtod(num, NULL) / 100.0);
                puts_(o, b);
                s++;
            } else {
                puts_(o, num);
            }
            continue;
        }
        if (isalpha(ch) || ch == '$' || ch == '_' || ch >= 0x80) {
            size_t cl;
            long row;
            size_t rl = cellref(s, &cl, &row);
            if (rl) {
                if (put_ref(o, s, cl, row, off) < 0) return -1;
                s += rl;
                continue;
            }
            const char *e = s;
            while (isalnum((unsigned char) *e) || *e == '.' || *e == '_' || (unsigned char) *e >= 0x80) e++;
            char name[48];
            if ((size_t) (e - s) >= sizeof name) return -1;
            upper(name, s, (size_t) (e - s), sizeof name);
            const char *after = e;
            while (*after == ' ') after++;
            if (*after == '(') {
                const char *f = fn_to_sc(name);
                if (f == NULL) return -1;                     /* a function sc-im does not have */
                const char *end;
                if (call_to_sc(f, after + 1, &end, o, off, semi) < 0) return -1;
                s = end;
                continue;
            }
            if (! strcmp(name, "TRUE") || ! strcmp(name, "VERO") || ! strcmp(name, "VERDADERO") ||
                ! strcmp(name, "VRAI") || ! strcmp(name, "WAHR")) { puts_(o, "1"); s = e; continue; }
            if (! strcmp(name, "FALSE") || ! strcmp(name, "FALSO") || ! strcmp(name, "FAUX") ||
                ! strcmp(name, "FALSCH")) { puts_(o, "0"); s = e; continue; }
            return -1;                                        /* a name, another sheet, a column: no */
        }
        switch (ch) {
            case '&': puts_(o, "#"); s++; continue;          /* Excel joins text with & */
            case ';': puts_(o, ","); s++; continue;
            case ',': if (semi) return -1; puts_(o, ","); s++; continue;
            case '+': case '-': case '*': case '/': case '^': case '(': case ')': case ':':
            case '<': case '>': case '=': case '%':
                put(o, s, 1); s++; continue;
            default:
                return -1;                                    /* '!' (sheets), '{' arrays, '#' errors... */
        }
    }
    return o->bad ? -1 : 0;
}

/* Does the formula separate its arguments with ';' (outside text constants)? */
static int uses_semicolons(const char *s) {
    int q = 0;
    for (; *s; s++) {
        if (*s == '"') q = ! q;
        else if (! q && *s == ';') return 1;
    }
    return 0;
}

int nv_fx_user_to_sc(const char *in, char *out, size_t n) {
    out_t o = {out, 0, n, 0};
    if (n) out[0] = '\0';
    if (*in == '=') in++;
    return to_sc(in, &o, -1, uses_semicolons(in));
}

int nv_fx_excel_to_sc(const char *in, long rowoff, char *out, size_t n) {
    out_t o = {out, 0, n, 0};
    if (n) out[0] = '\0';
    if (*in == '=') in++;
    if (! strncasecmp(in, "_xlfn.", 6)) in += 6;
    return to_sc(in, &o, rowoff, 0);
}

/* ---- sc-im -> Excel notation ---------------------------------------------------------------- */
static int from_sc(const char *s, out_t *o, long off, int lang, int excel) {
    const int semi = ! excel && lang != 0;       /* the user's European notation: ';' and ',' */
    while (*s && ! o->bad) {
        unsigned char ch = (unsigned char) *s;
        if (ch == '"') {
            const char *e = strchr(s + 1, '"');
            if (e == NULL) return -1;
            put(o, s, (size_t) (e - s + 1));
            s = e + 1;
            continue;
        }
        if (ch == '@') {
            const char *e = s + 1;
            while (isalnum((unsigned char) *e)) e++;
            char name[24];
            if ((size_t) (e - s - 1) >= sizeof name) return -1;
            memcpy(name, s + 1, (size_t) (e - s - 1));
            name[e - s - 1] = '\0';
            const char *f = fn_from_sc(name, excel ? 0 : lang);
            if (f == NULL) {
                if (excel) return -1;                         /* Excel would not know it */
                puts_(o, "@");                                /* shown as sc-im spells it */
                puts_(o, name);
            } else {
                puts_(o, f);
                if (*e != '(') puts_(o, "()");                /* @now: NOW() */
            }
            s = e;
            continue;
        }
        if (isdigit(ch) || (ch == '.' && isdigit((unsigned char) s[1]))) {
            while (isdigit((unsigned char) *s) || *s == '.' || *s == 'e' || *s == 'E' ||
                   ((*s == '-' || *s == '+') && (s[-1] == 'e' || s[-1] == 'E'))) {
                put(o, *s == '.' && semi ? "," : s, 1);
                s++;
            }
            continue;
        }
        if (isalpha(ch) || ch == '$') {
            size_t cl;
            long row;
            size_t rl = cellref(s, &cl, &row);
            if (rl) {
                if (put_ref(o, s, cl, row, off) < 0) return -1;
                s += rl;
                continue;
            }
            const char *e = s;
            while (isalnum((unsigned char) *e) || *e == '_') e++;
            put(o, s, (size_t) (e - s));                      /* (not expected: names) */
            if (excel) return -1;
            s = e;
            continue;
        }
        switch (ch) {
            case '#': puts_(o, "&"); s++; continue;
            case ',': puts_(o, semi ? ";" : ","); s++; continue;
            case '&': case '|': case '?': case '!': case '~':
                if (excel) return -1;                         /* logic sc-im spells its own way */
                put(o, s, 1); s++; continue;
            default:
                put(o, s, 1); s++; continue;
        }
    }
    return o->bad ? -1 : 0;
}

int nv_fx_sc_to_user(const char *sc, int lang, char *out, size_t n) {
    out_t o = {out, 0, n, 0};
    if (n < 2) return -1;
    out[0] = '=';
    out[1] = '\0';
    o.n = 1;
    return from_sc(sc, &o, 1, lang, 0);
}

int nv_fx_sc_to_excel(const char *sc, long rowoff, char *out, size_t n) {
    out_t o = {out, 0, n, 0};
    if (n) out[0] = '\0';
    return from_sc(sc, &o, rowoff, 0, 1);
}

/* Is this sc-im expression a text one? (its value is a string: sc-im wants it as a label) */
int nv_fx_is_string(const char * e) {
    static const char * const sfn[] = {"@upper", "@lower", "@capital", "@substr", "@ext", "@fmt",
        "@coltoa", "@sval", "@date", "@replace", "@chr", "@filename", "@sevaluate", NULL};
    while (*e == ' ') e++;
    if (*e == '"') return 1;
    for (int i = 0; sfn[i]; i++) if (! strncasecmp(e, sfn[i], strlen(sfn[i]))) return 1;
    if (! strncasecmp(e, "@if(", 4)) {                  /* the type of its "then" branch */
        const char * p = e + 4;
        int depth = 0, q = 0, commas = 0;
        for (; *p; p++) {
            if (q) { if (*p == '"') q = 0; continue; }
            if (*p == '"') q = 1;
            else if (*p == '(') depth++;
            else if (*p == ')') { if (! depth) break; depth--; }
            else if (*p == ',' && ! depth && ++commas == 1) return nv_fx_is_string(p + 1);
        }
    }
    if (*e == '(') {
        int depth = 0, q = 0;
        const char * p = e;
        for (; *p; p++) {
            if (q) { if (*p == '"') q = 0; continue; }
            if (*p == '"') q = 1;
            else if (*p == '(') depth++;
            else if (*p == ')' && --depth == 0) break;
        }
        if (*p == ')' && ! p[1]) {
            char * in = malloc((size_t) (p - e));
            if (in) { memcpy(in, e + 1, (size_t) (p - e - 1)); in[p - e - 1] = '\0'; }
            int s = in ? nv_fx_is_string(in) : 0;
            free(in);
            return s;
        }
    }
    int depth = 0, q = 0;                                /* a top-level # joins text */
    for (const char * p = e; *p; p++) {
        if (q) { if (*p == '"') q = 0; continue; }
        if (*p == '"') q = 1;
        else if (*p == '(') depth++;
        else if (*p == ')') depth--;
        else if (*p == '#' && ! depth) return 1;
    }
    return 0;
}
