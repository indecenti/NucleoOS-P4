// nv_basic_main.c — Terminal entry point for the ubasic port: `basic <file.bas>` loads a whole
// program from /sdcard/home and runs it (no immediate-mode REPL: uBASIC's line-number index is
// built by tokenizing a full program buffer once, not by editing lines interactively).
//
// PEEK/POKE address a small in-VM cell array instead of real memory — there is nothing sane to
// expose as memory-mapped I/O over WASI, but the two statements are wired up so BASIC programs
// keep a working (if classic-BASIC-crude) way to stash values by index.
#include <stdio.h>
#include <stdlib.h>
#include "ubasic.h"

#define MEM_CELLS 1024
static VARIABLE_TYPE s_mem[MEM_CELLS];

static VARIABLE_TYPE nv_peek(VARIABLE_TYPE addr) {
    return (addr >= 0 && addr < MEM_CELLS) ? s_mem[addr] : 0;
}

static void nv_poke(VARIABLE_TYPE addr, VARIABLE_TYPE value) {
    if (addr >= 0 && addr < MEM_CELLS) s_mem[addr] = value;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: basic <file.bas>\n");
        return 1;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "basic: can't open %s\n", argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) {
        fclose(f);
        fprintf(stderr, "basic: can't read %s\n", argv[1]);
        return 1;
    }
    char *src = malloc((size_t)n + 1);
    if (!src) {
        fclose(f);
        fprintf(stderr, "basic: out of memory\n");
        return 1;
    }
    size_t got = fread(src, 1, (size_t)n, f);
    fclose(f);
    src[got] = '\0';

    ubasic_init_peek_poke(src, nv_peek, nv_poke);
    while (!ubasic_finished()) {
        ubasic_run();
    }

    free(src);
    return 0;
}
