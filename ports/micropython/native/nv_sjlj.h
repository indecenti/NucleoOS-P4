// Native test build: the host's protected call emulated with setjmp (see ../../common/nv_sjlj.h).
#pragma once
int nv_try_call(void (*fn)(void *), void *ud);
__attribute__((noreturn)) void nv_throw(void);
