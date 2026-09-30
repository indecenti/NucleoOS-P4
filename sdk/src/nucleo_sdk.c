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

// Shared formatter for nv_printf / nv_snprintf. Always NUL-terminates when n > 0.
int nv_vsnprintf(char *out, size_t n, const char *fmt, va_list ap) {
    char num[12];
    size_t o = 0;
    if (!out || !n) return 0;
    for (const char *p = fmt; *p && o < n - 1; p++) {
        if (*p != '%') { out[o++] = *p; continue; }
        p++;
        const char *s = NULL;
        switch (*p) {
            case 's': s = va_arg(ap, const char *); if (!s) s = "(null)"; break;
            case 'c': out[o++] = (char)va_arg(ap, int); continue;
            case 'd': {
                const int32_t v = va_arg(ap, int32_t);
                if (v < 0 && o < n - 1) out[o++] = '-';
                s = u2s(num + sizeof(num), v < 0 ? 0u - (uint32_t)v : (uint32_t)v, 10, 0);
                break;
            }
            case 'u': s = u2s(num + sizeof(num), va_arg(ap, uint32_t), 10, 0); break;
            case 'x': s = u2s(num + sizeof(num), va_arg(ap, uint32_t), 16, 0); break;
            case 'X': s = u2s(num + sizeof(num), va_arg(ap, uint32_t), 16, 1); break;
            case '%': out[o++] = '%'; continue;
            case '\0': p--; continue;   // trailing lone '%': ignore
            default:                    // unknown verb: emit it literally
                out[o++] = '%';
                if (o < n - 1) out[o++] = *p;
                continue;
        }
        while (s && *s && o < n - 1) out[o++] = *s++;
    }
    out[o] = '\0';
    return (int)o;
}

int nv_snprintf(char *out, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int r = nv_vsnprintf(out, n, fmt, ap);
    va_end(ap);
    return r;
}

void nv_printf(const char *fmt, ...) {
    char out[256];
    va_list ap;
    va_start(ap, fmt);
    nv_vsnprintf(out, sizeof out, fmt, ap);
    va_end(ap);
    nv_print(out);
}
