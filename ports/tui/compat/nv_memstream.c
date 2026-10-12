// nv_memstream.c — open_memstream() that reports a failed write (link with
// -Wl,--wrap=open_memstream).
//
// wasi-libc's (musl's) memory stream returns 0 from its write callback when the buffer cannot
// grow, without marking the stream in error: fflush() succeeds and ferror() stays clear, so a
// program that ran out of memory carries on with a silently truncated buffer. libxlsxwriter
// builds every part of an .xlsx in such streams, and sc-im then saved a truncated workbook over
// the user's file. This version is a cookie stream whose write fails with ENOMEM instead: stdio
// marks the stream in error and the caller can tell.
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char  **bufp;
    size_t *sizep;
    char   *buf;
    size_t  pos, len, cap;   // cap excludes the terminating NUL kept after len
} nv_ms;

static void nv_ms_publish(nv_ms *m) {
    *m->bufp = m->buf;
    *m->sizep = m->pos < m->len ? m->pos : m->len;
}

static ssize_t nv_ms_write(void *c, const char *data, size_t n) {
    nv_ms *m = c;
    if (n > (size_t)-1 / 2 - m->pos) { errno = ENOMEM; return -1; }
    if (m->pos + n > m->cap) {
        size_t cap = m->cap ? m->cap : 1024;
        while (cap < m->pos + n) cap *= 2;
        char *nb = realloc(m->buf, cap + 1);
        if (!nb) { errno = ENOMEM; return -1; }
        memset(nb + m->cap, 0, cap + 1 - m->cap);
        m->buf = nb;
        m->cap = cap;
    }
    memcpy(m->buf + m->pos, data, n);
    m->pos += n;
    if (m->pos > m->len) m->len = m->pos;
    m->buf[m->len] = '\0';
    nv_ms_publish(m);
    return (ssize_t)n;
}

static ssize_t nv_ms_read(void *c, char *data, size_t n) {
    nv_ms *m = c;
    const size_t left = m->pos < m->len ? m->len - m->pos : 0;
    if (n > left) n = left;
    memcpy(data, m->buf + m->pos, n);
    m->pos += n;
    return (ssize_t)n;
}

static int nv_ms_seek(void *c, off_t *off, int whence) {
    nv_ms *m = c;
    const off_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (off_t)m->pos : (off_t)m->len;
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) { errno = EINVAL; return -1; }
    const off_t to = base + *off;
    if (to < 0) { errno = EINVAL; return -1; }
    m->pos = (size_t)to;
    nv_ms_publish(m);
    *off = to;
    return 0;
}

static int nv_ms_close(void *c) {
    nv_ms *m = c;
    nv_ms_publish(m);
    free(m);   // the buffer is the caller's
    return 0;
}

FILE *__wrap_open_memstream(char **bufp, size_t *sizep) {
    if (!bufp || !sizep) { errno = EINVAL; return NULL; }
    nv_ms *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->buf = calloc(1, 1);
    if (!m->buf) { free(m); return NULL; }
    m->bufp = bufp;
    m->sizep = sizep;
    nv_ms_publish(m);
    static const cookie_io_functions_t io = { nv_ms_read, nv_ms_write, nv_ms_seek, nv_ms_close };
    FILE *f = fopencookie(m, "w+", io);
    if (!f) { free(m->buf); free(m); *bufp = NULL; *sizep = 0; }
    return f;
}
