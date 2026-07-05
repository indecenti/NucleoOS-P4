// nucleo_sdk.c — SDK runtime linked into every NucleoOS WASM app (freestanding, no libc).
#include "nucleo_sdk.h"
#include <stdarg.h>

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

size_t strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

// Render an unsigned value right-to-left into end[-1]..; returns the first digit's address.
static char *u2s(char *end, uint32_t v, uint32_t base, int upper) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    *--end = '\0';
    if (!v) { *--end = '0'; return end; }
    while (v) { *--end = digits[v % base]; v /= base; }
    return end;
}

void nv_printf(const char *fmt, ...) {
    char out[256];
    char num[12];
    size_t o = 0;
    va_list ap;
    va_start(ap, fmt);
    for (const char *p = fmt; *p && o < sizeof(out) - 1; p++) {
        if (*p != '%') { out[o++] = *p; continue; }
        p++;
        const char *s = NULL;
        switch (*p) {
            case 's': s = va_arg(ap, const char *); if (!s) s = "(null)"; break;
            case 'c': out[o++] = (char)va_arg(ap, int); continue;
            case 'd': {
                int32_t v = va_arg(ap, int32_t);
                if (v < 0 && o < sizeof(out) - 1) { out[o++] = '-'; v = -v; }
                s = u2s(num + sizeof(num), (uint32_t)v, 10, 0);
                break;
            }
            case 'u': s = u2s(num + sizeof(num), va_arg(ap, uint32_t), 10, 0); break;
            case 'x': s = u2s(num + sizeof(num), va_arg(ap, uint32_t), 16, 0); break;
            case 'X': s = u2s(num + sizeof(num), va_arg(ap, uint32_t), 16, 1); break;
            case '%': out[o++] = '%'; continue;
            case '\0': p--; continue;   // trailing lone '%': ignore
            default:                    // unknown verb: emit it literally
                out[o++] = '%';
                if (o < sizeof(out) - 1) out[o++] = *p;
                continue;
        }
        while (s && *s && o < sizeof(out) - 1) out[o++] = *s++;
    }
    va_end(ap);
    out[o] = '\0';
    nv_print(out);
}
