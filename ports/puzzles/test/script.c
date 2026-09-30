// script.c — the scripted touch session both PC test hosts feed to the Puzzles app (one input
// sample per nv_gfx_present): for every puzzle, open it from the menu, tap around the board, drag,
// long-press (right click), press an on-screen key, Undo/Redo, Solve, New, pick the first preset
// from Type, Restart, then back to the menu. Layout constants mirror nv_puzzles.c (1024x600).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "script.h"

static script_frame *fr;
static int nfr, cap;

static script_frame *push(void) {
    if (nfr == cap) { cap = cap ? cap * 2 : 4096; fr = realloc(fr, sizeof *fr * cap); }
    memset(&fr[nfr], 0, sizeof *fr);
    return &fr[nfr++];
}
static void idle(int n) { while (n-- > 0) push(); }
static void hold(int x, int y, int n) {
    while (n-- > 0) { script_frame *f = push(); f->down = 1; f->x = x; f->y = y; }
}
static void tap(int x, int y) { hold(x, y, 2); idle(2); }
static void drag(int x0, int y0, int x1, int y1, int steps) {
    for (int i = 0; i <= steps; i++) hold(x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps, 1);
    idle(2);
}
static void back(void) { push()->back = 1; idle(2); }
static void shot(const char *name) { snprintf(push()->shot, sizeof fr[0].shot, "%s", name); }

// Top bar: Menu New Restart Undo Redo Solve Type Right (x from 8, gap 8, y 8..56).
static const int bar_w[] = { 104, 104, 124, 104, 104, 104, 120, 150 };
enum { MENU, NEW, RESTART, UNDO, REDO, SOLVE, TYPE, RCLICK };
static void bar(int which) {
    int x = 8;
    for (int i = 0; i < which; i++) x += bar_w[i] + 8;
    tap(x + bar_w[which] / 2, 32);
}

int script_build(int ngames, const char *only) {
    idle(3);
    shot("menu");
    for (int g = 0; g < ngames; g++) {
        char name[40];
        if (only && atoi(only) != g) continue;
        int c = g % 5, r = g / 5;
        tap(6 + c * 203 + 98, 60 + r * 67 + 30);        // menu tile
        idle(2);
        snprintf(name, sizeof name, "g%02d_open", g); shot(name);
        for (int i = 0; i < 6; i++) tap(200 + i * 120, 140 + (i % 3) * 110);
        drag(300, 200, 620, 380, 12);
        hold(512, 300, 36);                               // long press: right click
        idle(2);
        drag(420, 260, 560, 260, 8);                      // plain drag after
        hold(480, 330, 34); drag(480, 330, 600, 330, 6);  // right drag
        tap(80, 600 - 32 - 30);                           // on-screen key row (or the board)
        tap(512, 600 - 32 - 30);
        bar(UNDO); bar(REDO); bar(UNDO);
        bar(RCLICK); tap(350, 250); bar(RCLICK);
        snprintf(name, sizeof name, "g%02d_play", g); shot(name);
        bar(SOLVE); idle(40);                             // (animations run on the timer)
        snprintf(name, sizeof name, "g%02d_solved", g); shot(name);
        bar(NEW); idle(4);
        bar(TYPE); idle(2);
        snprintf(name, sizeof name, "g%02d_types", g); shot(name);
        tap(10 + 164, 74 + 32);                           // first preset
        idle(4);
        bar(RESTART); idle(4);
        tap(512, 300);
        back();                                           // OS back gesture: to the menu
        idle(2);
    }
    shot("menu_end");
    tap(6 + 98, 60 + 30);                                 // resume the first game (saved)
    idle(3);
    shot("resume");
    back();
    back();                                               // back at the menu: the app returns
    idle(20);
    return nfr;
}

const script_frame *script_at(int i) { return i < nfr ? &fr[i] : NULL; }
