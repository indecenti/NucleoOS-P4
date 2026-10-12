/* test_nv_fx.c — the Excel <-> sc-im formula translation (ports/tui/scim/nv_fx.c), on the PC:
 *   gcc -I ports/tui/scim -o /tmp/t ports/tui/tests/test_nv_fx.c ports/tui/scim/nv_fx.c && /tmp/t */
#include <stdio.h>
#include <string.h>

#include "nv_fx.h"

static int fails;

static void eq(const char *what, int rc, const char *got, const char *want) {
    if (want == NULL ? rc == 0 : (rc != 0 || strcmp(got, want))) {
        printf("FAIL %s: rc %d got [%s] want [%s]\n", what, rc, rc ? "" : got, want ? want : "(refused)");
        fails++;
    }
}

static void user(const char *in, const char *want) {
    char b[512];
    int rc = nv_fx_user_to_sc(in, b, sizeof b);
    eq(in, rc, b, want);
}

static void show(const char *sc, int lang, const char *want) {
    char b[512];
    int rc = nv_fx_sc_to_user(sc, lang, b, sizeof b);
    eq(sc, rc, b, want);
}

static void xin(const char *in, const char *want) {
    char b[512];
    int rc = nv_fx_excel_to_sc(in, -1, b, sizeof b);
    eq(in, rc, b, want);
}

static void xout(const char *sc, long off, const char *want) {
    char b[512];
    int rc = nv_fx_sc_to_excel(sc, off, b, sizeof b);
    eq(sc, rc, b, want);
}

int main(void) {
    /* typed by people: any of the five languages, ';' with a decimal comma or ',' */
    user("=SUM(B2:B8)", "@sum(B1:B7)");
    user("=somma(b2:b8)", "@sum(B1:B7)");
    user("=SOMMA(B2:B7;C2:C7)", "(@sum(B1:B6)+@sum(C1:C6))");
    user("=MEDIA(B2:B7;10)", "((@sum(B1:B6)+(10))/(@count(B1:B6)+1))");
    user("=MAX(A1:A3;7)", "@max(@max(A0:A2),7)");
    user("=B2*1,5", "B1*1,5");                    /* no ';': ',' is not a decimal mark */
    user("=ARRONDA(B2*1,5;0)", NULL);              /* (French typo: unknown function) */
    user("=ARROTONDA(B2*1,5;0)", "@round(B1*1.5,0)");
    user("=ROUND(B2*1.5,0)", "@round(B1*1.5,0)");
    user("=SE(B2>700;\"alto\";\"basso\")", "@if(B1>700,\"alto\",\"basso\")");
    user("=WENN(A1<>0;1;0)", "@if(A0<>0,1,0)");
    user("=B2&\" EUR\"", "B1#\" EUR\"");
    user("=$B$2+B$3+$C4", "$B$1+B$2+$C3");
    user("=10%*B2", "0.1*B1");
    user("=ADESSO()", "@now");
    user("=NOW()-A1", "@now-A0");
    user("=VERO+FALSO", "1+0");
    user("=Foglio2!A1", NULL);                      /* another sheet */
    user("=A:A", NULL);                             /* a whole column */
    user("=A0", NULL);                              /* Excel has no row 0 */
    user("=CERCA.VERT(1;A1:B3;2)", NULL);
    user("=\"a\"\"b\"", NULL);                       /* "" inside a text */
    user("=LEN(A1)", "@slen(A0)");
    user("=ANNÉE(A1)", "@year(A0)");

    /* shown in the formula bar */
    show("@sum(B1:B7)", 1, "=SOMMA(B2:B8)");
    show("@sum(B1:B7)", 0, "=SUM(B2:B8)");
    show("@round(B1*1.5,0)", 1, "=ARROTONDA(B2*1,5;0)");
    show("@round(B1*1.5,0)", 4, "=RUNDEN(B2*1,5;0)");
    show("@if(B1>700,\"a,b\",\"c\")", 2, "=SI(B2>700;\"a,b\";\"c\")");
    show("B1#\" EUR\"", 3, "=B2&\" EUR\"");
    show("@now", 1, "=ADESSO()");
    show("@lastrow", 1, "=@lastrow");                /* no Excel name: shown as sc-im spells it */

    /* .xlsx files */
    xin("SUM(B2:B7)", "@sum(B1:B6)");
    xin("AVERAGE(B2:B7)", "@avg(B1:B6)");
    xin("_xlfn.STDEV.S(A1:A9)", "@stddev(A0:A8)");
    xin("IF(B2>700,\"alto\",\"basso\")", "@if(B1>700,\"alto\",\"basso\")");
    xin("SUM(Sheet2!A1:A3)", NULL);
    xin("VLOOKUP(1,A1:B3,2,FALSE)", NULL);
    xout("@avg(B2:B7)", 0, "AVERAGE(B2:B7)");
    xout("@avg(B1:B6)", 1, "AVERAGE(B2:B7)");
    xout("(@sum(B1:B6)+@sum(C1:C6))", 1, "(SUM(B2:B7)+SUM(C2:C7))");
    xout("@now", 1, "NOW()");
    xout("@lastrow", 1, NULL);
    xout("A0&A1", 1, NULL);                          /* sc-im's logical and: no Excel spelling */

    if (fails) { printf("nv_fx: %d failure(s)\n", fails); return 1; }
    printf("nv_fx: all passed\n");
    return 0;
}
