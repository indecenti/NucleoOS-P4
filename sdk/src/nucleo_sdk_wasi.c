// nucleo_sdk_wasi.c — SDK runtime for apps built with build_app.ps1 -Wasi. wasi-libc already
// provides memcpy/strlen/printf, so only the nv_* helpers that aren't plain imports live here.
#include "nucleo_sdk.h"
#include <stdarg.h>
#include <stdio.h>

void nv_printf(const char *fmt, ...) {
    char out[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out, sizeof out, fmt, ap);
    va_end(ap);
    nv_print(out);
}
