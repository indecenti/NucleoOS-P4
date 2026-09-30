/* setjmp.h shim (wasm build only): shadows the wasi-libc header, which refuses to compile without
 * the WebAssembly exception-handling proposal (the device's WAMR does not run it).
 *
 * FreeType's ftstdlib.h includes <setjmp.h> for the table validators (gxvalid/otvalid, not built)
 * and ft_validator_error() in ftobjs.c, which only those validators reach. So setjmp() here
 * always returns 0 and longjmp() traps: it is unreachable in this build.
 */
#ifndef NV_HASP_SETJMP_H
#define NV_HASP_SETJMP_H

typedef long jmp_buf[8];

#define setjmp(env) ((void)(env), 0)
#define longjmp(env, val) ((void)(env), (void)(val), __builtin_trap())

#endif
