// What the stdio I/O variant of pForth leaves to the platform, for NucleoOS (WASI).
#include <stdio.h>
#include <time.h>
#include "pf_all.h"

// MS ( n -- ): pforth's posix variant uses usleep; WASI has nanosleep.
cell_t sdSleepMillis(cell_t msec) {
    if (msec <= 0) return 0;
    struct timespec ts = { msec / 1000, (long)(msec % 1000) * 1000000L };
    return nanosleep(&ts, NULL) == 0 ? 0 : -1;
}

// wasi-libc has no tmpfile(): RESIZE-FILE that shrinks a file reports failure (ior != 0).
FILE *tmpfile(void) { return NULL; }
