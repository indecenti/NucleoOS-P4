// setjmp.h shim for bc on NucleoOS (WASI): the WAMR runtime has no setjmp/longjmp (wasi-libc's
// header refuses to compile without the exception-handling proposal). bc's BC_SETJMP macros
// still push a jmp_buf per cleanup label to keep their bookkeeping, but sigsetjmp() never
// returns twice: errors unwind with nv_throw() to an nv_try_call in bc_vm_process() or main()
// (ports/cli/bc/bc.patch, ports/common/nv_sjlj.h).
#pragma once
#include "nv_sjlj.h"
typedef int jmp_buf[1];
typedef int sigjmp_buf[1];
#define sigsetjmp(j, s) ((void)(j), 0)
#define setjmp(j) ((void)(j), 0)
