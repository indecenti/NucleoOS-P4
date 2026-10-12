// setjmp.h for sc-im on WASI. sc-im's setjmp/longjmp pairs only recover from SIGFPE (a division
// by zero trapped by the FPU): its longjmp calls live in the SIGFPE handlers. WASI raises no
// SIGFPE (IEEE arithmetic gives inf/nan, which sc-im shows as ERROR), so the handlers never run:
// setjmp returns 0 and a longjmp that ever happened would be a bug, so it stops the program.
#pragma once
#include <stdio.h>
#include <stdlib.h>

typedef int jmp_buf[1];
#define setjmp(b) ((void)(b), 0)
static inline _Noreturn void nv_scim_longjmp(void) {
    fputs("sc-im: unexpected longjmp\n", stderr);
    abort();
}
#define longjmp(b, v) ((void)(b), (void)(v), nv_scim_longjmp())
