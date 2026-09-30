// Functions wasi-libc lacks, for Jim Tcl on NucleoOS.
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

// mktemp: fill the trailing XXXXXX with letters until the name does not exist yet.
char *mktemp(char *tmpl) {
    static unsigned seed;
    size_t n = strlen(tmpl);
    if (n < 6 || strcmp(tmpl + n - 6, "XXXXXX") != 0) { tmpl[0] = '\0'; return tmpl; }
    if (!seed) seed = (unsigned)time(NULL) * 2654435761u + 1;
    for (int tries = 0; tries < 100; tries++) {
        struct stat st;
        for (int i = 0; i < 6; i++) {
            seed = seed * 1103515245u + 12345u;
            tmpl[n - 6 + i] = "abcdefghijklmnopqrstuvwxyz0123456789"[(seed >> 16) % 36];
        }
        if (stat(tmpl, &st) != 0) return tmpl;
    }
    tmpl[0] = '\0';
    return tmpl;
}
