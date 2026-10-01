// Native test build of the host imports in ../../common/nv_sjlj.h, so the patched core runs the
// upstream test suite on the PC (tools: bash ports/micropython/build.sh test).
#include <setjmp.h>
#include <stddef.h>
#include "nv_sjlj.h"

static jmp_buf *cur;

int nv_try_call(void (*fn)(void *), void *ud) {
    jmp_buf jb, *prev = cur;
    cur = &jb;
    if (setjmp(jb) == 0) {
        fn(ud);
        cur = prev;
        return 0;
    }
    cur = prev;
    return 1;
}

void nv_throw(void) {
    longjmp(*cur, 1);
}
