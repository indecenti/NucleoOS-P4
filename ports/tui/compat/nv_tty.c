// nv_tty.c — stdin of the NucleoOS full-screen ports (curses programs); termios is nv_termios.c.
//
// Resizing: WASI has no SIGWINCH and no TIOCGWINSZ. The Terminal reports a new grid size in-band
// on stdin, as xterm's mode 2048 does: CSI 48 ; rows ; cols ; height_px ; width_px t. stdin is
// read through here (link with -Wl,--wrap=read,--wrap=poll,--wrap=select,--wrap=ioctl): a report
// is taken out of the stream, LINES / COLUMNS are updated, ioctl(TIOCGWINSZ) answers with them
// and SIGWINCH is raised, exactly what a resized Unix terminal does. The on-screen keyboard showing and hiding
// is such a resize. poll() and select() count the bytes already buffered here, so curses still
// tells an ESC key from an escape sequence.
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>
#include "termios.h"   // struct winsize, TIOCGWINSZ

ssize_t __real_read(int fd, void *buf, size_t n);
int     __real_poll(struct pollfd *fds, nfds_t n, int timeout);
int     __real_select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *t);
int     __real_ioctl(int fd, int request, ...);

static unsigned char s_in[512];   // stdin read ahead: s_in[s_r .. s_n) not handed out yet
static size_t s_r, s_n;

// A report at p (p[0] == ESC)? Its length, 0 if p is something else, -1 if it may be one whose
// end has not arrived yet. rows/cols get its first two parameters.
static int resize_report(const unsigned char *p, size_t n, int *rows, int *cols) {
    static const char lead[] = "\x1b[48;";
    const size_t ln = sizeof lead - 1;
    if (n < ln) return memcmp(p, lead, n) ? 0 : -1;
    if (memcmp(p, lead, ln)) return 0;
    int par[4] = {0, 0, 0, 0}, np = 0, digits = 0;
    for (size_t i = ln; i < n; i++) {
        const unsigned char c = p[i];
        if (c >= '0' && c <= '9') {
            if (np < 4 && par[np] < 100000) par[np] = par[np] * 10 + (c - '0');
            digits++;
        } else if (c == ';') {
            if (!digits || ++np > 3) return 0;
            digits = 0;
        } else if (c == 't') {
            if (!digits || np < 1) return 0;
            *rows = par[0];
            *cols = par[1];
            return (int)(i + 1);
        } else {
            return 0;
        }
    }
    return n - ln < 40 ? -1 : 0;
}

// Take every complete report out of the buffered bytes. True if the size changed.
static int take_reports(void) {
    int resized = 0;
    for (size_t i = s_r; i < s_n; i++) {
        if (s_in[i] != 0x1b) continue;
        int rows = 0, cols = 0, len;
        while ((len = resize_report(s_in + i, s_n - i, &rows, &cols)) < 0) {
            // the report is written in one piece: the rest is already on its way
            if (s_n == sizeof s_in) break;
            struct pollfd pf = {0, POLLIN, 0};
            if (__real_poll(&pf, 1, 50) <= 0) break;
            const ssize_t k = __real_read(0, s_in + s_n, sizeof s_in - s_n);
            if (k <= 0) break;
            s_n += (size_t)k;
        }
        if (len <= 0) continue;
        memmove(s_in + i, s_in + i + len, s_n - i - len);
        s_n -= (size_t)len;
        i--;
        if (rows >= 2 && cols >= 2 && rows <= 1000 && cols <= 1000) {
            char v[16];
            snprintf(v, sizeof v, "%d", rows);
            setenv("LINES", v, 1);
            snprintf(v, sizeof v, "%d", cols);
            setenv("COLUMNS", v, 1);
            resized = 1;
        }
    }
    return resized;
}

ssize_t __wrap_read(int fd, void *buf, size_t n) {
    if (fd != 0 || n == 0) return __real_read(fd, buf, n);
    for (;;) {
        if (s_r < s_n) {
            const size_t k = n < s_n - s_r ? n : s_n - s_r;
            memcpy(buf, s_in + s_r, k);
            s_r += k;
            return (ssize_t)k;
        }
        const ssize_t k = __real_read(0, s_in, sizeof s_in);
        if (k <= 0) return k;
        s_r = 0;
        s_n = (size_t)k;
        if (take_reports()) {
            raise(SIGWINCH);
            if (s_r == s_n) { errno = EINTR; return -1; }   // like a read interrupted by the signal
        }
    }
}

int __wrap_poll(struct pollfd *fds, nfds_t n, int timeout) {
    int buffered = 0;
    for (nfds_t i = 0; i < n; i++) buffered |= fds[i].fd == 0 && (fds[i].events & POLLIN) && s_r < s_n;
    const int rc = __real_poll(fds, n, buffered ? 0 : timeout);
    if (!buffered || rc < 0) return rc;
    int ready = 0;
    for (nfds_t i = 0; i < n; i++) {
        if (fds[i].fd == 0 && (fds[i].events & POLLIN)) fds[i].revents |= POLLIN;
        ready += fds[i].revents != 0;
    }
    return ready;
}

// Bytes buffered here: stdin is ready now (the other sets are reported on the next call).
int __wrap_select(int nfds, fd_set *r, fd_set *w, fd_set *e, struct timeval *t) {
    if (!(r && nfds > 0 && FD_ISSET(0, r) && s_r < s_n)) return __real_select(nfds, r, w, e, t);
    FD_ZERO(r);
    FD_SET(0, r);
    if (w) FD_ZERO(w);
    if (e) FD_ZERO(e);
    return 1;
}

// The size of the terminal: the last one reported, or the one the program started with.
int __wrap_ioctl(int fd, int request, ...) {
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (request != TIOCGWINSZ) return __real_ioctl(fd, request, arg);
    if (fd < 0 || fd > 2 || !isatty(fd)) { errno = ENOTTY; return -1; }
    const char *l = getenv("LINES"), *c = getenv("COLUMNS");
    const int rows = l ? atoi(l) : 0, cols = c ? atoi(c) : 0;
    if (rows <= 0 || cols <= 0 || !arg) { errno = EINVAL; return -1; }
    struct winsize *w = (struct winsize *)arg;
    w->ws_row = (unsigned short)rows;
    w->ws_col = (unsigned short)cols;
    w->ws_xpixel = w->ws_ypixel = 0;
    return 0;
}
