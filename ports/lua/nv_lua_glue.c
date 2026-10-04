// nv_lua_glue.c — libc pieces wasi-libc leaves out that the Lua sources reference.
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// The Terminal's folder: the OS passes it as PWD (the program's own view of the card), wasi-libc
// starts every program in "/". Entering it first makes `cd ~/proj; lua main.lua` and require
// "./mod" work as on a PC.
__attribute__((constructor)) static void nv_lua_enter_pwd(void) {
    const char *p = getenv("PWD");
    if (p && p[0] == '/' && p[1]) (void)chdir(p);
}

// io.tmpfile(): WASI has no temporary directory. Lua turns NULL into (nil, message).
FILE *tmpfile(void) {
    errno = ENOTSUP;
    return NULL;
}
