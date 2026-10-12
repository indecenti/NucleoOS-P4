/* nv_engine.c — sc-im as the calculation engine behind NucleoOS' spreadsheet app (Fogli).
 *
 * `sc-im --nocurses --nv_engine [file]` reads one command per line on stdin and answers on stdout,
 * one record per line, fields separated by tabs. The app draws the grid itself; sc-im keeps the
 * document, evaluates the formulas, keeps the undo history and reads/writes the files. A request
 * ends with "nv sync N", answered by the line "\x1eN" once everything before it is done, so the
 * app knows a batch is complete. Rows and columns are sc-im's (from 0); everything the user sees
 * or types (cell text, formulas) is in Excel's notation and the UI language (see nv_fx.c).
 *
 *   nv lang <en|it|es|fr|de>      number and formula notation of what follows
 *   nv info                       D name modified nsheets cursheet maxrow maxcol, then S idx name
 *   nv cols c0 c1                 W col width precision hidden          (width in characters)
 *   nv view r0 c0 r1 c1           C r c kind align style text           (non-empty cells only)
 *                                 kind n number, f formula, d date, s text, g text formula, e error
 *                                 align l c r; style: 1 bold 2 italic 4 underline
 *   nv cell r c                   X r c text                            (what the formula bar edits)
 *   nv set r c <text>             smart entry: =formula, number, date, 'text or text; "" clears
 *   nv clear r0 c0 r1 c1          empties a range (undoable)
 *   nv copy|cut r0 c0 r1 c1       into sc-im's yank buffer; nv paste r c [values]
 *   nv insrow|delrow r n          nv inscol|delcol c n
 *   nv sort r0 c0 r1 c1 col asc|desc
 *   nv width c w                  nv autofit c0 c1
 *   nv fmt r0 c0 r1 c1 <format>   number format ("" = general), e.g. #,##0.00 or 0%
 *   nv bold|italic|underline r0 c0 r1 c1 0|1
 *   nv align r0 c0 r1 c1 l|c|r    (text cells)
 *   nv undo / nv redo
 *   nv sheet idx | nv newsheet name | nv rensheet name | nv delsheet
 *   nv stats r0 c0 r1 c1          T count numbers sum average min max
 *   nv tsv r0 c0 r1 c1 [fx]       the range as text: L row(from 1) values..., for the clipboard;
 *                                 fx: formula cells add " [=FORMULA]" (Excel, English), for ANIMA
 *   nv find <text>                F r c (up to 200 matches, current sheet)
 *   nv check <id> <text>          K id 1|0: would "nv set" read this formula? (ANIMA's proposals)
 *   nv save <path>                by extension: .xlsx .csv .tsv .md .sc  -> OK or E
 *   nv backup <path>              a recovery copy (.sc): the document keeps its name and state
 *   nv name <path>                the document's file name from now on (a recovered copy)
 * Any other line goes to sc-im's interpreter as it is. Messages: E (error) / I (info) / V (value).
 */
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <wchar.h>

#include "sc.h"
#include "macros.h"
#include "conf.h"
#include "interp.h"
#include "file.h"
#include "format.h"
#include "yank.h"
#include "undo.h"
#include "graph.h"
#include "sheet.h"
#include "buffer.h"
#include "color.h"
#include "cmds/cmds.h"
#include "cmds/cmds_normal.h"
#include "utils/dictionary.h"
#include "vmtbl.h"
#include "xmalloc.h"
#ifndef NONE_COLOR
#define NONE_COLOR -2        /* tui.h: no colour of its own */
#endif
#include "nv_fx.h"

extern struct session * session;
extern int cmd_multiplier;
extern char line[FBUFLEN];
extern int linelim;

static int s_lang;          /* 0 en, 1 it, 2 es, 3 fr, 4 de */
static int s_errors;        /* error messages so far: a command failed when this moved */

#define LINE_MAX_ 16384

/* ---- output --------------------------------------------------------------------------------- */

/* A field: tabs and line breaks would break the record */
static void field(const char *s) {
    for (; *s; s++) putchar(*s == '\t' || *s == '\n' || *s == '\r' ? ' ' : *s);
}

void nv_engine_msg(int type, const char *t) {
    if (type == ERROR_MSG) s_errors++;
    putchar(type == ERROR_MSG ? 'E' : type == VALUE_MSG ? 'V' : 'I');
    putchar('\t');
    field(t);
    putchar('\n');
}

/* ---- numbers in the user's notation ---------------------------------------------------------- */

static int comma_lang(void) { return s_lang != 0; }

/* "1455.9" -> "1455,9", "1,234.50" -> "1.234,50" (only for the comma languages) */
static void localize_num(char *s) {
    if (! comma_lang()) return;
    for (; *s; s++) {
        if (*s == '.') *s = ',';
        else if (*s == ',') *s = '.';
    }
}

/* Parses a number as people type it in their language: thousands groups, decimal comma, a sign,
 * a currency symbol, a trailing %. Returns 1 and the value, *pct set for a percentage. */
static int parse_number(const char *in, double *v, int *pct) {
    char b[96];
    size_t k = 0;
    const char *s = in;
    *pct = 0;
    while (*s == ' ') s++;
    int neg = 0;
    if (*s == '-' || *s == '+') { neg = *s == '-'; s++; }
    if (! strncmp(s, "\xE2\x82\xAC", 3)) s += 3;           /* € */
    else if (*s == '$') s++;
    while (*s == ' ') s++;
    int ndot = 0, ncomma = 0, digits = 0;
    const char *p = s;
    for (; *p && (isdigit((unsigned char) *p) || *p == '.' || *p == ','); p++) {
        if (*p == '.') ndot++;
        else if (*p == ',') ncomma++;
        else digits++;
    }
    if (! digits) return 0;
    const char *end = p;
    while (*p == ' ') p++;
    if (! strncmp(p, "\xE2\x82\xAC", 3)) p += 3;
    else if (*p == '%') { *pct = 1; p++; }
    while (*p == ' ') p++;
    if (*p) {                                               /* 1e5 and the like: strtod's job */
        char *e;
        double d = strtod(in, &e);
        while (*e == ' ') e++;
        if (*e || e == in) return 0;
        *v = d;
        return 1;
    }
    /* which mark is the decimal one: in the comma languages "1.234,5", "1.234" and "12.5" are
     * 1234.5, 1234 and 12.5 (one dot and three digits reads as thousands, as Excel does) */
    char dec = '.', grp = ',';
    if (comma_lang()) {
        if (ncomma) { dec = ','; grp = '.'; }
        else if (ndot > 1) { dec = ','; grp = '.'; }
        else if (ndot == 1) {
            const char *q = strchr(s, '.');
            if (end - q - 1 == 3 && q > s && q - s <= 3) { dec = ','; grp = '.'; }
        }
    }
    if ((dec == '.' && ndot > 1) || (dec == ',' && ncomma > 1)) return 0;
    for (const char *q = s; q < end && k + 1 < sizeof b; q++) {
        if (*q == grp) continue;
        b[k++] = *q == dec ? '.' : *q;
    }
    b[k] = '\0';
    *v = strtod(b, NULL) * (neg ? -1 : 1);
    if (*pct) *v /= 100.0;
    return 1;
}

/* A date typed as d/m/y (m/d/y in English), d.m.y, d-m-y or y-m-d: local midnight, in seconds */
static int parse_date(const char *s, double *v, const char **fmt) {
    int a, b, c;
    char s1, s2;
    char tail;
    if (sscanf(s, "%d%c%d%c%d%c", &a, &s1, &b, &s2, &c, &tail) != 5) return 0;
    if (s1 != s2 || ! (s1 == '/' || s1 == '-' || s1 == '.')) return 0;
    int d, m, y;
    if (a > 31) { y = a; m = b; d = c; *fmt = "%Y-%m-%d"; }
    else if (s_lang == 0 && s1 == '/') { m = a; d = b; y = c; *fmt = "%m/%d/%Y"; }
    else { d = a; m = b; y = c; *fmt = s1 == '.' ? "%d.%m.%Y" : s1 == '-' ? "%d-%m-%Y" : "%d/%m/%Y"; }
    if (y < 100) y += y < 70 ? 2000 : 1900;
    if (m < 1 || m > 12 || d < 1 || d > 31 || y < 1900 || y > 9999) return 0;
    struct tm tm = {0};
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    tm.tm_hour = 0;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    if (t == (time_t) -1) return 0;
    *v = (double) t;
    return 1;
}

/* ---- cells ------------------------------------------------------------------------------------ */

static struct ent * cell_at(struct sheet * sh, int r, int c) {
    if (r < 0 || c < 0 || r > sh->maxrows || c > sh->maxcols) return NULL;
    return *ATBL(sh, sh->tbl, r, c);
}

/* What the grid shows for a numeric cell */
static void num_text(struct sheet * sh, struct ent * p, int col, char * out, size_t n) {
    out[0] = '\0';
    if (p->format && *p->format == 'd') {
        time_t t = (time_t) p->v;
        struct tm * tm = localtime(&t);
        if (tm) strftime(out, n, p->format + 1, tm);
        return;
    }
    if (p->format) {
        format(p->format, sh->precision[col], p->v, out, (int) n);
    } else if (sh->precision[col] == DEFPREC && sh->realfmt[col] == DEFREFMT) {
        /* Excel's "General": as many digits as it takes, no padding */
        double v = p->v;
        if (fabs(v) >= 1e15 || (fabs(v) < 1e-9 && v != 0)) snprintf(out, n, "%.6g", v);
        else {
            snprintf(out, n, "%.10f", v);
            char * dot = strchr(out, '.');
            if (dot) {
                char * e = out + strlen(out) - 1;
                while (e > dot && *e == '0') *e-- = '\0';
                if (e == dot) *e = '\0';
            }
            if (! strcmp(out, "-0")) strcpy(out, "0");
        }
    } else {
        engformat(sh->realfmt[col], sh->fwidth[col], sh->precision[col], p->v, out, (int) n);
    }
    char * s = out;
    while (*s == ' ') s++;
    memmove(out, s, strlen(s) + 1);
    size_t l = strlen(out);
    while (l && out[l - 1] == ' ') out[--l] = '\0';
    localize_num(out);
}

static void cmd_view(struct sheet * sh, int r0, int c0, int r1, int c1) {
    char txt[FBUFLEN];
    if (r1 > sh->maxrow) r1 = sh->maxrow;
    if (c1 > sh->maxcol) c1 = sh->maxcol;
    for (int r = r0; r <= r1; r++) {
        if (sh->row_hidden[r]) { printf("R\t%d\th\n", r); continue; }
        for (int c = c0; c <= c1; c++) {
            struct ent * p = cell_at(sh, r, c);
            if (p == NULL || ! (p->flags & is_valid || p->label || p->cellerror)) continue;
            char kind, align = 'r';
            txt[0] = '\0';
            if (p->cellerror) {
                kind = 'e';
                strcpy(txt, p->cellerror == CELLERROR ? "#ERR" : "#REF!");
            } else if (p->flags & is_valid && ! (p->label && (p->flags & is_strexpr))) {
                kind = p->format && *p->format == 'd' ? 'd' : p->expr ? 'f' : 'n';
                num_text(sh, p, c, txt, sizeof txt);
            } else if (p->label) {
                kind = p->flags & is_strexpr ? 'g' : 's';
                snprintf(txt, sizeof txt, "%s", p->label);
                align = p->flags & is_label ? 'c' : p->flags & is_leftflush ? 'l' : 'r';
            } else {
                continue;
            }
            int style = 0;
            if (p->ucolor) style = (p->ucolor->bold ? 1 : 0) | (p->ucolor->italic ? 2 : 0) | (p->ucolor->underline ? 4 : 0);
            printf("C\t%d\t%d\t%c\t%c\t%d\t", r, c, kind, align, style);
            field(txt);
            putchar('\n');
        }
    }
}

/* What editing a cell starts from: its formula, its number, its date, or its text */
static void cmd_cell(struct sheet * sh, int r, int c) {
    char out[FBUFLEN] = "";
    struct ent * p = cell_at(sh, r, c);
    if (p && p->expr) {
        line[0] = '\0';
        linelim = 0;
        if (p->flags & is_strexpr) edits(sh, r, c, 0); else editexp(sh, r, c);
        linelim = -1;
        if (nv_fx_sc_to_user(line, s_lang, out, sizeof out) < 0) snprintf(out, sizeof out, "=%s", line);
    } else if (p && p->flags & is_valid) {
        if (p->format && *p->format == 'd') {
            time_t t = (time_t) p->v;
            struct tm * tm = localtime(&t);
            if (tm) strftime(out, sizeof out, s_lang == 0 ? "%m/%d/%Y" : "%d/%m/%Y", tm);
        } else {
            snprintf(out, sizeof out, "%.15g", p->v);
            localize_num(out);
        }
    } else if (p && p->label) {
        double v;
        int pct;
        const char * f;
        /* text that would be read back as something else keeps its apostrophe */
        if (p->label[0] == '=' || p->label[0] == '\'' || parse_number(p->label, &v, &pct) || parse_date(p->label, &v, &f))
            snprintf(out, sizeof out, "'%s", p->label);
        else
            snprintf(out, sizeof out, "%s", p->label);
    }
    printf("X\t%d\t%d\t", r, c);
    field(out);
    putchar('\n');
}

static void interp(const char * fmt, ...) {
    char b[FBUFLEN + 64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    wchar_t w[FBUFLEN + 64];
    if (mbstowcs(w, b, sizeof w / sizeof w[0]) == (size_t) -1) { nv_engine_msg(ERROR_MSG, "bad text"); return; }
    send_to_interp(w);
}

/* sc-im strings cannot hold a double quote */
static void unquote(char * s) {
    for (; *s; s++) if (*s == '"') *s = '\'';
}

static void erase_range(struct sheet * sh, int r0, int c0, int r1, int c1) {
    extern struct ent_ptr * deps;
    struct roman * roman = session->cur_doc;
    if (r0 > sh->maxrow || c0 > sh->maxcol) return;          /* empty already */
    if (r1 > sh->maxrow) r1 = sh->maxrow;
    if (c1 > sh->maxcol) c1 = sh->maxcol;
    if (any_locked_cells(sh, r0, c0, r1, c1)) { nv_engine_msg(ERROR_MSG, "locked"); return; }
#ifdef UNDO
    create_undo_action();
    ents_that_depends_on_range(sh, r0, c0, r1, c1);
    copy_to_undostruct(sh, r0, c0, r1, c1, UNDO_DEL, HANDLE_DEPS, NULL);
    if (deps != NULL) { free(deps); deps = NULL; }
#endif
    erase_area(sh, r0, c0, r1, c1, 0, 0);
    roman->modflg++;
    sync_refs(sh);
    EvalAll();
#ifdef UNDO
    copy_to_undostruct(sh, r0, c0, r1, c1, UNDO_ADD, HANDLE_DEPS, NULL);
    if (deps != NULL) { free(deps); deps = NULL; }
    end_undo_action();
#endif
}

/* A value and its format in one undoable step (a date, a percentage); the cell keeps its look */
static void set_value_fmt(struct sheet * sh, int r, int c, double v, const char * fmt) {
    extern struct ent_ptr * deps;
    struct roman * roman = session->cur_doc;
    if (locked_cell(sh, r, c)) { nv_engine_msg(ERROR_MSG, "locked"); return; }
#ifdef UNDO
    create_undo_action();
    ents_that_depends_on_range(sh, r, c, r, c);
    copy_to_undostruct(sh, r, c, r, c, UNDO_DEL, HANDLE_DEPS, NULL);
    if (deps != NULL) { free(deps); deps = NULL; }
#endif
    struct ent * p = lookat(sh, r, c);
    if (p->label) { scxfree(p->label); p->label = NULL; }
    if (p->expr) { efree(p->expr); p->expr = NULL; }
    if (p->format) { scxfree(p->format); p->format = NULL; }
    p->format = scxmalloc((unsigned) strlen(fmt) + 1);
    strcpy(p->format, fmt);
    p->v = v;
    p->cellerror = CELLOK;
    p->flags = (p->flags & ~(is_strexpr | is_label | is_leftflush | is_deleted)) | is_valid | is_changed;
    roman->modflg++;
    EvalAll();
#ifdef UNDO
    copy_to_undostruct(sh, r, c, r, c, UNDO_ADD, HANDLE_DEPS, NULL);
    if (deps != NULL) { free(deps); deps = NULL; }
    end_undo_action();
#endif
}

static void cmd_set(struct sheet * sh, int r, int c, char * text) {
    char sc[FBUFLEN];
    double v;
    int pct;
    const char * dfmt;
    char * t = text;
    while (*t == ' ') t++;
    size_t l = strlen(t);
    while (l && t[l - 1] == ' ') t[--l] = '\0';
    if (! *t) { erase_range(sh, r, c, r, c); return; }
    if (*t == '\'') {                                      /* 'text: always text */
        unquote(t + 1);
        interp("leftstring %s%d = \"%s\"", coltoa(c), r, t + 1);
        return;
    }
    if (*t == '=' && t[1]) {
        if (nv_fx_user_to_sc(t, sc, sizeof sc) < 0) {
            nv_engine_msg(ERROR_MSG, "formula");
            return;
        }
        /* text results (a "constant", & joins, @upper, @if with text branches) are string
         * expressions in sc-im, numbers the rest */
        interp("%s %s%d = %s", nv_fx_is_string(sc) ? "leftstring" : "let", coltoa(c), r, sc);
        EvalRange(sh, r, c, r, c);
        return;
    }
    if (parse_number(t, &v, &pct)) {
        if (pct) set_value_fmt(sh, r, c, v, strchr(t, ',') || strchr(t, '.') ? "0.00%" : "0%");
        else interp("let %s%d = %.15g", coltoa(c), r, v);
        EvalRange(sh, r, c, r, c);
        return;
    }
    if (parse_date(t, &v, &dfmt)) {
        char f[24];
        snprintf(f, sizeof f, "d%s", s_lang == 0 ? "%m/%d/%Y" : "%d/%m/%Y");
        set_value_fmt(sh, r, c, v, f);
        EvalRange(sh, r, c, r, c);
        return;
    }
    unquote(t);
    interp("leftstring %s%d = \"%s\"", coltoa(c), r, t);
    EvalRange(sh, r, c, r, c);
}

/* One normal-mode command of sc-im (its undo handling comes with it), as if typed */
static void normal_keys(struct sheet * sh, int r, int c, const wchar_t * keys, int mult) {
    struct block * b = create_buf();
    for (; *keys; keys++) addto_buf(b, *keys);
    sh->currow = r < 0 ? 0 : r;
    sh->curcol = c < 0 ? 0 : c;
    cmd_multiplier = mult;
    do_normalmode(b);
    cmd_multiplier = 0;
    erase_buf(b);
}

static void cmd_stats(struct sheet * sh, int r0, int c0, int r1, int c1) {
    long count = 0, nums = 0;
    double sum = 0, mn = 0, mx = 0;
    if (r1 > sh->maxrow) r1 = sh->maxrow;
    if (c1 > sh->maxcol) c1 = sh->maxcol;
    for (int r = r0; r <= r1; r++)
        for (int c = c0; c <= c1; c++) {
            struct ent * p = cell_at(sh, r, c);
            if (p == NULL) continue;
            if (p->flags & is_valid && ! p->cellerror) {
                if (! nums || p->v < mn) mn = p->v;
                if (! nums || p->v > mx) mx = p->v;
                sum += p->v;
                nums++;
                count++;
            } else if (p->label) {
                count++;
            }
        }
    char a[48], b[48], cmin[48], cmax[48];
    snprintf(a, sizeof a, "%.10g", sum);
    snprintf(b, sizeof b, "%.10g", nums ? sum / nums : 0);
    snprintf(cmin, sizeof cmin, "%.10g", mn);
    snprintf(cmax, sizeof cmax, "%.10g", mx);
    localize_num(a); localize_num(b); localize_num(cmin); localize_num(cmax);
    printf("T\t%ld\t%ld\t%s\t%s\t%s\t%s\n", count, nums, a, b, cmin, cmax);
}

/* The range as text, one L line per row. With `fx`, a formula cell also shows its formula in
 * Excel's English notation after the value, "2250 [=SUM(B2:D2)]": what ANIMA needs to check or
 * explain a sheet. */
static void cmd_tsv(struct sheet * sh, int r0, int c0, int r1, int c1, int fx) {
    char txt[FBUFLEN], f[FBUFLEN];
    if (r1 > sh->maxrow) r1 = sh->maxrow;
    if (c1 > sh->maxcol) c1 = sh->maxcol;
    for (int r = r0; r <= r1; r++) {
        printf("L\t%d", r + 1);
        for (int c = c0; c <= c1; c++) {
            struct ent * p = cell_at(sh, r, c);
            txt[0] = '\0';
            if (p && p->cellerror) {
                snprintf(txt, sizeof txt, "%s", p->cellerror == CELLERROR ? "#ERR" : "#REF!");
            } else if (p && p->flags & is_valid && ! (p->label && p->flags & is_strexpr)) {
                if (p->format && *p->format == 'd') num_text(sh, p, c, txt, sizeof txt);
                else snprintf(txt, sizeof txt, "%.10g", p->v);
            } else if (p && p->label) {
                snprintf(txt, sizeof txt, "%s", p->label);
            }
            if (fx && p && p->expr) {
                line[0] = '\0';
                linelim = 0;
                if (p->flags & is_strexpr) edits(sh, r, c, 0); else editexp(sh, r, c);
                linelim = -1;
                if (nv_fx_sc_to_user(line, 0, f, sizeof f) < 0) snprintf(f, sizeof f, "=%s", line);
                const size_t l = strlen(txt);
                snprintf(txt + l, sizeof txt - l, " [%s]", f);
            }
            putchar('\t');
            field(txt);
        }
        putchar('\n');
    }
}

static void cmd_find(struct sheet * sh, const char * what) {
    char txt[FBUFLEN];
    int n = 0;
    for (int r = 0; r <= sh->maxrow && n < 200; r++)
        for (int c = 0; c <= sh->maxcol && n < 200; c++) {
            struct ent * p = cell_at(sh, r, c);
            if (p == NULL) continue;
            if (p->label) snprintf(txt, sizeof txt, "%s", p->label);
            else if (p->flags & is_valid) num_text(sh, p, c, txt, sizeof txt);
            else continue;
            if (strcasestr(txt, what)) { printf("F\t%d\t%d\n", r, c); n++; }
        }
}

static void cmd_info(void) {
    struct roman * roman = session->cur_doc;
    struct sheet * sh = roman->cur_sh;
    int idx = 0, cur = 0;
    for (struct sheet * s = roman->first_sh; s; s = s->next, idx++) if (s == sh) cur = idx;
    printf("D\t");
    field(roman->name ? roman->name : "");
    printf("\t%d\t%d\t%d\t%d\t%d\n", roman->modflg ? 1 : 0, idx, cur, sh->maxrow, sh->maxcol);
    idx = 0;
    for (struct sheet * s = roman->first_sh; s; s = s->next, idx++) {
        printf("S\t%d\t", idx);
        field(s->name ? s->name : "");
        putchar('\n');
    }
}

static void cmd_cols(struct sheet * sh, int c0, int c1) {
    if (c0 < 0) c0 = 0;
    if (c1 >= sh->maxcols) c1 = sh->maxcols - 1;
    for (int c = c0; c <= c1; c++)
        printf("W\t%d\t%d\t%d\t%d\n", c, sh->fwidth[c], sh->precision[c], sh->col_hidden[c] ? 1 : 0);
}

/* A number format on a range ("" = general), one undo step; empty cells inside the used area get
 * it too, so what is typed there later shows the same way. Dates keep their own format. */
static void cmd_fmt(struct sheet * sh, int r0, int c0, int r1, int c1, const char * f) {
    struct roman * roman = session->cur_doc;
    /* a whole column or row selected: up to the last used row / column */
    if (r1 - r0 > 2000 && r1 > sh->maxrow) r1 = sh->maxrow > r0 ? sh->maxrow : r0;
    if (c1 - c0 > 200 && c1 > sh->maxcol) c1 = sh->maxcol > c0 ? sh->maxcol : c0;
    if (r1 < r0 || c1 < c0) return;
    if ((long) (r1 - r0 + 1) * (c1 - c0 + 1) > 20000) { nv_engine_msg(ERROR_MSG, "range too big"); return; }
    if (any_locked_cells(sh, r0, c0, r1, c1)) { nv_engine_msg(ERROR_MSG, "locked"); return; }
#ifdef UNDO
    create_undo_action();
    copy_to_undostruct(sh, r0, c0, r1, c1, UNDO_DEL, IGNORE_DEPS, NULL);
#endif
    for (int r = r0; r <= r1; r++)
        for (int c = c0; c <= c1; c++) {
            struct ent * p = lookat(sh, r, c);
            if (p->format && *p->format == 'd') continue;
            if (p->format) { scxfree(p->format); p->format = NULL; }
            if (*f) {
                p->format = scxmalloc((unsigned) strlen(f) + 1);
                strcpy(p->format, f);
            }
            p->flags |= is_changed;
        }
    roman->modflg++;
#ifdef UNDO
    copy_to_undostruct(sh, r0, c0, r1, c1, UNDO_ADD, IGNORE_DEPS, NULL);
    end_undo_action();
#endif
}

/* Bold / italic / underline on a range, one undo step for all of it */
static void cmd_style(struct sheet * sh, const char * what, int r0, int c0, int r1, int c1, int on) {
    struct roman * roman = session->cur_doc;
    if (any_locked_cells(sh, r0, c0, r1, c1)) { nv_engine_msg(ERROR_MSG, "locked"); return; }
#ifdef UNDO
    create_undo_action();
    copy_to_undostruct(sh, r0, c0, r1, c1, UNDO_DEL, IGNORE_DEPS, NULL);
#endif
    for (int r = r0; r <= r1; r++)
        for (int c = c0; c <= c1; c++) {
            struct ent * n = lookat(sh, r, c);
            if (n->ucolor == NULL) {
                if (! on) continue;
                n->ucolor = calloc(1, sizeof(struct ucolor));
                if (n->ucolor == NULL) continue;
                n->ucolor->fg = NONE_COLOR;
                n->ucolor->bg = NONE_COLOR;
            }
            if (*what == 'b') n->ucolor->bold = on;
            else if (*what == 'i') n->ucolor->italic = on;
            else n->ucolor->underline = on;
        }
    roman->modflg++;
#ifdef UNDO
    copy_to_undostruct(sh, r0, c0, r1, c1, UNDO_ADD, IGNORE_DEPS, NULL);
    end_undo_action();
#endif
}

static void cmd_align(struct sheet * sh, int r0, int c0, int r1, int c1, char how) {
    struct roman * roman = session->cur_doc;
    for (int r = r0; r <= r1 && r <= sh->maxrow; r++)
        for (int c = c0; c <= c1 && c <= sh->maxcol; c++) {
            struct ent * p = cell_at(sh, r, c);
            if (p == NULL || ! p->label || (p->flags & is_strexpr)) continue;
            char * t = strdup(p->label);
            if (t == NULL) continue;
            unquote(t);
            interp("%s %s%d = \"%s\"", how == 'l' ? "leftstring" : how == 'c' ? "label" : "rightstring",
                   coltoa(c), r, t);
            free(t);
        }
    roman->modflg++;
}

/* The document's file name: sc-im copies names into it, so it is always a PATHLEN buffer */
static void set_name(struct roman * doc, const char * path) {
    if (doc->name == NULL) doc->name = malloc(PATHLEN);
    if (doc->name) snprintf(doc->name, PATHLEN, "%s", path);
}

static void cmd_save(const char * path) {
    struct roman * doc = session->cur_doc;
    if (strlen(path) >= PATHLEN - 1) { nv_engine_msg(ERROR_MSG, "path"); return; }
    swprintf(inputline, BUFFERSIZE, L"w! %s", path);
    int before = s_errors;
    savefile();
    inputline[0] = L'\0';
    if (s_errors == before) {
        set_name(doc, path);
        doc->modflg = 0;
        printf("OK\tsave\n");
    }
}

static int parse_ints(const char * s, int * v, int n) {
    int k = 0;
    while (k < n) {
        char * e;
        long x = strtol(s, &e, 10);
        if (e == s) break;
        v[k++] = (int) x;
        s = e;
    }
    return k;
}

/* The rest of the line after `n` integers */
static char * after_ints(char * s, int n) {
    while (n--) {
        while (*s == ' ') s++;
        if (*s == '-') s++;
        while (isdigit((unsigned char) *s)) s++;
    }
    if (*s == ' ') s++;
    return s;
}

/* Undo and redo put back copies of the cells they saved: the formulas that pointed at the
 * replaced cells must point at the copies, or they keep reading (and saving) a detached cell. */
static void after_undo(void) {
    for (struct sheet * s = session->cur_doc->first_sh; s; s = s->next) sync_refs(s);
    rebuild_graph();
    EvalAll();
}

/* Cell names for one command line (coltoa() has a single static buffer) */
static const char * a1(int r, int c) { static char b[16]; snprintf(b, sizeof b, "%s%d", coltoa(c), r); return b; }
static const char * a2(int r, int c) { static char b[16]; snprintf(b, sizeof b, "%s%d", coltoa(c), r); return b; }

static void order(int * a, int * b) { if (*a > *b) { int t = *a; *a = *b; *b = t; } }

static void cmd_nv(char * cmd) {
    struct roman * roman = session->cur_doc;
    struct sheet * sh = roman->cur_sh;
    char * arg = strchr(cmd, ' ');
    if (arg) *arg++ = '\0'; else arg = cmd + strlen(cmd);
    int v[6] = {0};
    int n = parse_ints(arg, v, 6);
    int r0 = v[0], c0 = v[1], r1 = v[2], c1 = v[3];
    if (n >= 4) {
        order(&r0, &r1);
        order(&c0, &c1);
        if (r0 < 0 || c0 < 0) { nv_engine_msg(ERROR_MSG, "range"); return; }
        /* a whole row or column selected: the used part of it (cells past the table are not
         * allocated, and sc-im walks ranges without bounds checks) */
        if (r1 > sh->maxrow) r1 = sh->maxrow > r0 ? sh->maxrow : r0;
        if (c1 > sh->maxcol) c1 = sh->maxcol > c0 ? sh->maxcol : c0;
        /* nothing there to clear, copy or sort: sc-im would walk past its table */
        if ((r0 > sh->maxrow || c0 > sh->maxcol) &&
            (! strcmp(cmd, "clear") || ! strcmp(cmd, "copy") || ! strcmp(cmd, "cut") || ! strcmp(cmd, "sort")))
            return;
    }

    if (! strcmp(cmd, "sync")) { printf("\x1e%s\n", arg); }
    else if (! strcmp(cmd, "lang")) { s_lang = nv_fx_lang(arg); }
    else if (! strcmp(cmd, "info")) cmd_info();
    else if (! strcmp(cmd, "cols") && n >= 2) cmd_cols(sh, v[0], v[1]);
    else if (! strcmp(cmd, "view") && n >= 4) cmd_view(sh, r0, c0, r1, c1);
    else if (! strcmp(cmd, "cell") && n >= 2) cmd_cell(sh, v[0], v[1]);
    else if (! strcmp(cmd, "set") && n >= 2) { cmd_set(sh, v[0], v[1], after_ints(arg, 2)); }
    else if (! strcmp(cmd, "clear") && n >= 4) erase_range(sh, r0, c0, r1, c1);
    else if ((! strcmp(cmd, "copy") || ! strcmp(cmd, "cut")) && n >= 4) {
        yank_area(sh, r0, c0, r1, c1, r0 == r1 && c0 == c1 ? 'e' : 'a', 1);
        if (cmd[1] == 'u') erase_range(sh, r0, c0, r1, c1);
    }
    else if (! strcmp(cmd, "paste") && n >= 2) {
        sh->currow = v[0];
        sh->curcol = v[1];
        if (paste_yanked_ents(sh, 0, strstr(arg, "values") ? 'v' : 'a') == -1) nv_engine_msg(ERROR_MSG, "locked");
        EvalAll();
    }
    else if (! strcmp(cmd, "insrow") && n >= 1) { int k = n > 1 && v[1] > 0 ? v[1] : 1; while (k--) normal_keys(sh, v[0], 0, L"ir", 1); }
    else if (! strcmp(cmd, "inscol") && n >= 1) { int k = n > 1 && v[1] > 0 ? v[1] : 1; while (k--) normal_keys(sh, 0, v[0], L"ic", 1); }
    else if (! strcmp(cmd, "delrow") && n >= 1) normal_keys(sh, v[0], 0, L"dr", n > 1 && v[1] > 0 ? v[1] : 1);
    else if (! strcmp(cmd, "delcol") && n >= 1) normal_keys(sh, 0, v[0], L"dc", n > 1 && v[1] > 0 ? v[1] : 1);
    else if (! strcmp(cmd, "undo")) { do_undo(); after_undo(); }
    else if (! strcmp(cmd, "redo")) { do_redo(); after_undo(); }
    else if (! strcmp(cmd, "sort") && n >= 5) {
        const int desc = strstr(arg, "desc") != NULL;
        /* "auto" (the whole table): a first row of text over a row with numbers is the header and
         * stays where it is, as Excel does */
        if (strstr(arg, "auto") && r1 > r0) {
            int text_head = 1, num_next = 0, any = 0;
            for (int c = c0; c <= c1; c++) {
                struct ent * h = cell_at(sh, r0, c), * x = cell_at(sh, r0 + 1, c);
                if (h && (h->flags & is_valid) && ! (h->label && (h->flags & is_strexpr))) text_head = 0;
                if (h && h->label) any = 1;
                if (x && (x->flags & is_valid) && ! (x->label && (x->flags & is_strexpr))) num_next = 1;
            }
            if (text_head && any && num_next) r0++;
        }
        if (r1 <= r0) return;
        /* text sorts as text, numbers as numbers: both keys, numbers first like Excel */
        char key[8];
        snprintf(key, sizeof key, "%s", coltoa(v[4]));
        interp("sort %s:%s \"%c#%s;%c$%s\"", a1(r0, c0), a2(r1, c1),
               desc ? '-' : '+', key, desc ? '-' : '+', key);
    }
    else if (! strcmp(cmd, "width") && n >= 2) {
        if (v[1] < 1) v[1] = 1;
        if (v[1] > 200) v[1] = 200;
        interp("format %s %d %d %d", coltoa(v[0]), v[1], sh->precision[v[0]], sh->realfmt[v[0]]);
    }
    else if (! strcmp(cmd, "autofit") && n >= 2) { auto_fit(sh, v[0], v[1], DEFWIDTH); roman->modflg++; }
    else if (! strcmp(cmd, "fmt") && n >= 4) {
        char * f = after_ints(arg, 4);
        unquote(f);
        cmd_fmt(sh, r0, c0, r1, c1, f);
    }
    else if ((! strcmp(cmd, "bold") || ! strcmp(cmd, "italic") || ! strcmp(cmd, "underline")) && n >= 5)
        cmd_style(sh, cmd, r0, c0, r1, c1, v[4]);
    else if (! strcmp(cmd, "align") && n >= 4) cmd_align(sh, r0, c0, r1, c1, *after_ints(arg, 4));
    else if (! strcmp(cmd, "sheet") && n >= 1) {
        int i = 0;
        for (struct sheet * s = roman->first_sh; s; s = s->next, i++)
            if (i == v[0]) { roman->cur_sh = s; break; }
    }
    else if (! strcmp(cmd, "newsheet")) {
        unquote(arg);
        if (*arg && search_sheet(roman, arg) == NULL) {
            struct sheet * s = new_sheet(roman, arg);
            growtbl(s, GROWNEW, 0, 0);
            erasedb(s, 0);
            roman->cur_sh = s;
            roman->modflg++;
        } else nv_engine_msg(ERROR_MSG, "sheet name");
    }
    else if (! strcmp(cmd, "saved")) { roman->modflg = 0; }     /* a fresh document is not "changed" */
    else if (! strcmp(cmd, "rensheet")) {
        if (*arg && search_sheet(roman, arg) == NULL) {
            free(sh->name);
            sh->name = strdup(arg);
            roman->modflg++;
        } else nv_engine_msg(ERROR_MSG, "sheet name");
    }
    else if (! strcmp(cmd, "delsheet")) {
        if (sh->next == NULL && sh->prev == NULL) nv_engine_msg(ERROR_MSG, "only sheet");
        else {
            roman->cur_sh = sh->next ? sh->next : sh->prev;
            delete_sheet(roman, sh, 0);
            roman->modflg++;
        }
    }
    else if (! strcmp(cmd, "stats") && n >= 4) cmd_stats(sh, r0, c0, r1, c1);
    else if (! strcmp(cmd, "tsv") && n >= 4) cmd_tsv(sh, r0, c0, r1, c1, strstr(after_ints(arg, 4), "fx") != NULL);
    else if (! strcmp(cmd, "find")) { if (*arg) cmd_find(sh, arg); }
    else if (! strcmp(cmd, "check") && n >= 1) {
        /* would `nv set` take this text? (ANIMA's proposals are checked before they are offered) */
        char * t = after_ints(arg, 1);
        while (*t == ' ') t++;
        char sc[FBUFLEN];
        const int ok = *t != '=' || ! t[1] || nv_fx_user_to_sc(t, sc, sizeof sc) == 0;
        printf("K\t%d\t%d\n", v[0], ok);
    }
    else if (! strcmp(cmd, "save")) { if (*arg) cmd_save(arg); }
    else if (! strcmp(cmd, "backup")) {
        /* write_fd(), not writefile(): that one renames the document and clears "modified" */
        FILE * f = *arg ? fopen(arg, "w") : NULL;
        if (f) {
            write_fd(f, roman);
            if (fclose(f) == 0) printf("OK\tbackup\n"); else nv_engine_msg(ERROR_MSG, "backup");
        }
    }
    else if (! strcmp(cmd, "name")) {
        if (*arg && strlen(arg) < PATHLEN - 1) { set_name(roman, arg); roman->modflg++; }
    }
    else nv_engine_msg(ERROR_MSG, "unknown command");
}

/* The engine's loop: until stdin ends (the app closed). */
void nv_engine_run(void) {
    char * buf = malloc(LINE_MAX_);
    if (buf == NULL) return;
    put(user_conf_d, "xlsx_readformulas", "1");
    put(user_conf_d, "autocalc", "1");
    EvalAll();                                           /* formulas read from a file */
    printf("\x1e" "0\n");                                 /* ready: the file (if any) is loaded */
    fflush(stdout);
    while (fgets(buf, LINE_MAX_, stdin) != NULL) {
        size_t l = strlen(buf);
        while (l && (buf[l - 1] == '\n' || buf[l - 1] == '\r')) buf[--l] = '\0';
        if (! l) continue;
        if (! strncmp(buf, "nv ", 3)) {
            cmd_nv(buf + 3);
        } else {
            wchar_t * w = malloc(sizeof(wchar_t) * (l + 1));
            if (w && mbstowcs(w, buf, l + 1) != (size_t) -1) send_to_interp(w);
            free(w);
        }
        fflush(stdout);
    }
    free(buf);
}
