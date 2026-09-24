// nv_sjlj.h — non-local error exits for WASI ports, without setjmp/longjmp.
//
// wasi-sdk implements setjmp/longjmp with the WebAssembly exception-handling proposal, which the
// NucleoOS runtime (WAMR fast-interp + AOT) does not execute. Interpreters that recover from
// errors with longjmp (Lua) use these two host imports instead (host ABI v8, no permission):
//
//   nv_try_call(fn, ud)  calls fn(ud) through the host. Returns 0 when fn returned normally, 1
//                        when something inside it called nv_throw().
//   nv_throw()           unwinds every WASM frame up to the innermost pending nv_try_call.
//
// The host implements the unwind as a trap it catches and clears, so everything between the two
// calls is discarded exactly like a longjmp. The C shadow stack (__stack_pointer) is NOT rewound
// by the trap: the function that called nv_try_call must keep a frame on the shadow stack (a
// local whose address is taken does it), so its epilogue restores the pointer when it returns.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

__attribute__((import_module("nv"), import_name("try_call")))
int nv_try_call(void (*fn)(void *), void *ud);

__attribute__((import_module("nv"), import_name("throw"), noreturn))
void nv_throw(void);

#ifdef __cplusplus
}
#endif
