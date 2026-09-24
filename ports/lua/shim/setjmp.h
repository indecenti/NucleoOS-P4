// setjmp.h shim: Lua on NucleoOS unwinds through nv_try_call/nv_throw (nv_lua_port.h), so
// ldo.c needs no jmp_buf. Shadows the wasi-libc header, which refuses to compile without the
// WebAssembly exception-handling proposal.
#pragma once
