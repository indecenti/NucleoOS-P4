// nv_lua_glue.c — libc pieces wasi-libc leaves out that the Lua sources reference.
#include <errno.h>
#include <stdio.h>

// io.tmpfile(): WASI has no temporary directory. Lua turns NULL into (nil, message).
FILE *tmpfile(void) {
    errno = ENOTSUP;
    return NULL;
}
