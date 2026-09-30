// nv_compat.c — the few libc / Chocolate Doom helpers WASI does not provide.
#include <stdio.h>
#include <stdlib.h>

#include "i_system.h"

// i_system.c shells out to show a message box on fatal errors; there is no shell here.
int system(const char *cmd) {
    (void)cmd;
    return -1;
}

// Chocolate Doom (i_oplmusic.c / midifile.c) expects this from its newer i_system.c.
void *I_Realloc(void *ptr, size_t size) {
    void *p = realloc(ptr, size);
    if (p == NULL && size != 0) I_Error("I_Realloc: failed on reallocation of %u bytes", (unsigned)size);
    return p;
}
