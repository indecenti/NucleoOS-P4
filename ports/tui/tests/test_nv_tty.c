// test_nv_tty.c — unit test of ../compat/nv_tty.c (stdin of the full-screen ports), built natively
// on the PC by ../test.sh: the in-band resize reports the Terminal sends must vanish from stdin
// and become LINES / COLUMNS + SIGWINCH, everything else must pass through byte for byte, and
// poll()/select() must see the bytes nv_tty already holds.
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>
#include "termios.h"

ssize_t __wrap_read(int fd, void *buf, size_t n);
int __wrap_poll(struct pollfd *fds, nfds_t n, int timeout);
int __wrap_select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *t);
int __wrap_ioctl(int fd, int request, ...);

// ---- the "real" stdin: a script of chunks, each delivered by one read ---------------------------
static const char *s_chunks[16];
static size_t s_lens[16];
static int s_nchunks, s_next;

static void script(int n, ...) {
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) {
        s_chunks[i] = va_arg(ap, const char *);
        s_lens[i] = strlen(s_chunks[i]);
    }
    va_end(ap);
    s_nchunks = n;
    s_next = 0;
}

ssize_t __real_read(int fd, void *buf, size_t n) {
    if (fd != 0 || s_next >= s_nchunks) return 0;
    size_t k = s_lens[s_next] < n ? s_lens[s_next] : n;
    memcpy(buf, s_chunks[s_next], k);
    s_next++;
    return (ssize_t)k;
}
int __real_poll(struct pollfd *fds, nfds_t n, int timeout) {
    (void)timeout;
    int ready = 0;
    for (nfds_t i = 0; i < n; i++) {
        fds[i].revents = (fds[i].fd == 0 && s_next < s_nchunks) ? POLLIN : 0;
        ready += fds[i].revents != 0;
    }
    return ready;
}
int __real_select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *t) {
    (void)n; (void)w; (void)e; (void)t;
    if (r && FD_ISSET(0, r) && s_next < s_nchunks) return 1;
    if (r) FD_ZERO(r);
    return 0;
}
int __real_ioctl(int fd, int request, ...) { (void)fd; (void)request; errno = EINVAL; return -1; }
int isatty(int fd) { return fd >= 0 && fd <= 2; }

// ---- checks ------------------------------------------------------------------------------------
static int s_fail, s_winch;
static void on_winch(int sig) { (void)sig; s_winch++; }

#define CHECK(name, cond) do { if (cond) printf("  ok   %s\n", name); \
                               else { printf("  FAIL %s\n", name); s_fail = 1; } } while (0)

// Everything the program reads until EOF (one byte at a time, as curses does); EINTR is skipped.
static size_t drain(char *out, size_t cap, int *eintr) {
    size_t n = 0;
    *eintr = 0;
    for (;;) {
        char c;
        ssize_t k = __wrap_read(0, &c, 1);
        if (k < 0 && errno == EINTR) { (*eintr)++; continue; }
        if (k <= 0 || n + 1 >= cap) break;
        out[n++] = c;
    }
    out[n] = '\0';
    return n;
}

int main(void) {
    char out[256];
    int eintr;
    signal(SIGWINCH, on_winch);

    script(1, "abc\x1b[A\x1b");
    drain(out, sizeof out, &eintr);
    CHECK("keys pass through", !strcmp(out, "abc\x1b[A\x1b") && s_winch == 0);

    unsetenv("LINES");
    unsetenv("COLUMNS");
    s_winch = 0;
    script(1, "\x1b[48;27;100;567;1000t");
    drain(out, sizeof out, &eintr);
    CHECK("report removed", out[0] == '\0');
    CHECK("report sets LINES/COLUMNS", getenv("LINES") && !strcmp(getenv("LINES"), "27") &&
          getenv("COLUMNS") && !strcmp(getenv("COLUMNS"), "100"));
    CHECK("report raises SIGWINCH", s_winch == 1);
    CHECK("report alone reads as EINTR", eintr == 1);

    s_winch = 0;
    script(1, "x\x1b[48;10;40t" "y");
    drain(out, sizeof out, &eintr);
    CHECK("report between keys", !strcmp(out, "xy") && s_winch == 1 && !strcmp(getenv("LINES"), "10"));

    s_winch = 0;
    script(2, "k\x1b[48;12;", "60;0;0tz");
    drain(out, sizeof out, &eintr);
    CHECK("report split across reads", !strcmp(out, "kz") && s_winch == 1 &&
          !strcmp(getenv("COLUMNS"), "60"));

    s_winch = 0;
    script(1, "\x1b[48;x;1t" "\x1b[48;5t" "\x1b[48;3;3;3;3;3t");
    drain(out, sizeof out, &eintr);
    CHECK("malformed reports pass through", !strcmp(out, "\x1b[48;x;1t" "\x1b[48;5t" "\x1b[48;3;3;3;3;3t")
          && s_winch == 0);

    s_winch = 0;
    script(1, "\x1b[48;1;1t");
    drain(out, sizeof out, &eintr);
    CHECK("absurd size ignored", out[0] == '\0' && s_winch == 0 && !strcmp(getenv("LINES"), "12"));

    // poll/select see what nv_tty holds: an arrow key read one byte at a time
    script(1, "\x1b[B");
    char c;
    __wrap_read(0, &c, 1);
    struct pollfd pf = {0, POLLIN, 0};
    CHECK("poll sees buffered bytes", c == 0x1b && __wrap_poll(&pf, 1, 1000) == 1 && (pf.revents & POLLIN));
    fd_set r;
    FD_ZERO(&r);
    FD_SET(0, &r);
    struct timeval tv = {1, 0};
    CHECK("select sees buffered bytes", __wrap_select(1, &r, NULL, NULL, &tv) == 1 && FD_ISSET(0, &r));
    drain(out, sizeof out, &eintr);
    CHECK("rest of the key follows", !strcmp(out, "[B"));
    pf.revents = 0;
    CHECK("poll empty when nothing left", __wrap_poll(&pf, 1, 0) == 0);

    struct winsize ws = {0, 0, 0, 0};
    setenv("LINES", "31", 1);
    setenv("COLUMNS", "120", 1);
    CHECK("TIOCGWINSZ answers", __wrap_ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_row == 31 && ws.ws_col == 120);
    CHECK("TIOCGWINSZ not a tty", __wrap_ioctl(5, TIOCGWINSZ, &ws) == -1 && errno == ENOTTY);

    printf(s_fail ? "nv_tty: FAILED\n" : "nv_tty: all passed\n");
    return s_fail;
}
