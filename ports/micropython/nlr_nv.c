// Non-local returns for MicroPython on NucleoOS, without setjmp (MICROPY_NLR_NV).
//
// wasi-sdk implements setjmp/longjmp with the WebAssembly exception-handling proposal, which the
// device runtime (WAMR fast-interp + AOT) does not execute. The patched core
// (micropython.patch) runs every protected region as a callback of nlr_nv_try(), which goes
// through the host's protected call (ports/common/nv_sjlj.h): nlr_jump() becomes nv_throw(),
// a trap the innermost nv_try_call catches. A jump aimed at an outer buffer is re-thrown until it
// reaches the nv_try_call of that buffer.
#include "py/mpstate.h"
#include "py/runtime.h"
#include "nv_sjlj.h"

static nlr_buf_t *nv_target;   // the buffer the pending jump unwinds to

int nlr_nv_try(nlr_buf_t *buf, void (*fn)(void *), void *ud) {
    // keep a frame on the C shadow stack: the trap does not rewind __stack_pointer, this
    // function's epilogue does (see nv_sjlj.h)
    volatile int frame = 0;
    nlr_push_tail(buf);
    if (nv_try_call(fn, ud) == 0) {
        if (MP_STATE_THREAD(nlr_top) == buf) {
            nlr_pop();
        }
        return frame;
    }
    if (nv_target != buf) {
        nv_throw();   // aimed at an outer handler: keep unwinding
    }
    nv_target = NULL;
    return 1 + frame;
}

MP_NORETURN void nlr_jump(void *val) {
    MP_NLR_JUMP_HEAD(val, top);
    nv_target = top;
    nv_throw();
}
