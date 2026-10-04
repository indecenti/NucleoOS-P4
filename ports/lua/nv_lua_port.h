// nv_lua_port.h — force-included into every Lua source when building for NucleoOS (WASI).
//
// Lua recovers from errors with setjmp/longjmp (ldo.c LUAI_TRY/LUAI_THROW). The on-device WASM
// runtime has no setjmp, so the protected call goes through the host's nv_try_call and an error
// unwinds with nv_throw (see ports/common/nv_sjlj.h). LUAI_TRY is expanded once, in
// luaD_rawrunprotected, whose `struct lua_longjmp lj` lives on the shadow stack (its address is
// stored in L->errorJmp), so that function restores __stack_pointer when it returns.
#pragma once
#include "nv_sjlj.h"
#include <stdlib.h>
#include <time.h>

/* Local time. wasi-libc has no time zones (localtime == gmtime), so os.date() showed UTC. The OS
   passes the local offset (DST included) as NUCLEO_UTC_OFFSET: localtime is UTC + offset, and
   os.time{...} (a local date) goes back by the same amount. Read per call: a long-running script
   keeps the offset it was started with, which is fine for an interactive program. */
static inline long nv_lua_utc_offset(void) {
    const char *s = getenv("NUCLEO_UTC_OFFSET");
    return s ? atol(s) : 0;
}
static inline struct tm *nv_lua_localtime(const time_t *t, struct tm *r) {
    const time_t x = *t + (time_t)nv_lua_utc_offset();
    return gmtime_r(&x, r);
}
static inline time_t nv_lua_mktime(struct tm *tm) {
    const time_t x = (mktime)(tm);                 /* wasi: the fields read as UTC */
    return x == (time_t)-1 ? x : x - (time_t)nv_lua_utc_offset();
}
#define l_gmtime(t, r)    gmtime_r(t, r)
#define l_localtime(t, r) nv_lua_localtime(t, r)
#define mktime(tm)        nv_lua_mktime(tm)

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
