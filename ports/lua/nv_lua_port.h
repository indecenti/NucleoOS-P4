// nv_lua_port.h — force-included into every Lua source when building for NucleoOS (WASI).
//
// Lua recovers from errors with setjmp/longjmp (ldo.c LUAI_TRY/LUAI_THROW). The on-device WASM
// runtime has no setjmp, so the protected call goes through the host's nv_try_call and an error
// unwinds with nv_throw (see ports/common/nv_sjlj.h). LUAI_TRY is expanded once, in
// luaD_rawrunprotected, whose `struct lua_longjmp lj` lives on the shadow stack (its address is
// stored in L->errorJmp), so that function restores __stack_pointer when it returns.
#pragma once
#include "nv_sjlj.h"

struct lua_State;

typedef struct {
    void (*f)(struct lua_State *, void *);
    struct lua_State *L;
    void *ud;
} nv_lua_pcall_t;

static inline void nv_lua_tramp(void *p) {
    nv_lua_pcall_t *c = (nv_lua_pcall_t *)p;
    c->f(c->L, c->ud);
}

#define luai_jmpbuf      int   /* unused: the host keeps the unwind target */
#define LUAI_THROW(L, c) ((void)(c), nv_throw())
#define LUAI_TRY(L, c, a)                                                   \
    {                                                                       \
        nv_lua_pcall_t nvc_ = { f, L, ud };                                 \
        if (nv_try_call(nv_lua_tramp, &nvc_) != 0 && (c)->status == 0)      \
            (c)->status = -1;                                               \
    }

/* No processes on the device: os.execute() reports "no shell", io.popen() is unsupported
   (liolib's default without LUA_USE_POSIX) and os.tmpname() fails cleanly. */
#define l_system(cmd)        ((cmd) == NULL ? 0 : -1)
#define LUA_TMPNAMBUFSIZE    32
#define lua_tmpnam(b, e)     { (b)[0] = '\0'; e = 1; }
