// nv_posix.c — the POSIX calls WASI leaves out, for the NucleoOS full-screen ports.
//
// A WASI program has no processes, no shell, no user database and no dynamic loading: those
// calls fail with the errno a POSIX system gives when the facility is missing, so the programs
// take their own "not available" paths (sc-im's clipboard and `!` commands report an error
// instead of crashing). Temporary files are real: mkstemp/mkdtemp/tmpfile create them (tmpfile
// in /tmp, the card's tmp folder for "home" programs).
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "pwd.h"
#include "sys/wait.h"

int system(const char *cmd) {
    if (!cmd) return 0;   // "is a shell available?": no
    errno = ENOSYS;
    return -1;
}

FILE *popen(const char *cmd, const char *mode) { (void)cmd; (void)mode; errno = ENOSYS; return NULL; }
int pclose(FILE *f) { (void)f; errno = ECHILD; return -1; }

pid_t fork(void) { errno = ENOSYS; return -1; }
int execv(const char *p, char *const a[])  { (void)p; (void)a; errno = ENOSYS; return -1; }
int execvp(const char *p, char *const a[]) { (void)p; (void)a; errno = ENOSYS; return -1; }
int execl(const char *p, const char *a, ...)  { (void)p; (void)a; errno = ENOSYS; return -1; }
int execlp(const char *p, const char *a, ...) { (void)p; (void)a; errno = ENOSYS; return -1; }
pid_t wait(int *s) { (void)s; errno = ECHILD; return -1; }
pid_t waitpid(pid_t p, int *s, int o) { (void)p; (void)s; (void)o; errno = ECHILD; return -1; }

// Job control does not exist (the Terminal sends Ctrl-Z as a key): stopping and continuing
// oneself is a no-op, anything else has no process to reach.
int kill(pid_t pid, int sig) {
    if (pid == getpid() || pid == 0) {
        if (sig == SIGTSTP || sig == SIGSTOP || sig == SIGCONT || sig == 0) return 0;
        return raise(sig);
    }
    errno = ESRCH;
    return -1;
}

struct passwd *getpwnam(const char *name) { (void)name; return NULL; }
struct passwd *getpwuid(uid_t uid) { (void)uid; return NULL; }

char *ttyname(int fd) { return isatty(fd) ? "/dev/tty" : NULL; }
mode_t umask(mode_t m) { (void)m; return 022; }
int mkfifo(const char *p, mode_t m) { (void)p; (void)m; errno = ENOSYS; return -1; }

void *dlopen(const char *f, int flags) { (void)f; (void)flags; return NULL; }
void *dlsym(void *h, const char *s) { (void)h; (void)s; return NULL; }
int dlclose(void *h) { (void)h; return 0; }
char *dlerror(void) { return "dynamic loading is not available"; }

int mkstemp(char *tpl) {
    static const char set[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    const size_t n = strlen(tpl);
    if (n < 6 || strcmp(tpl + n - 6, "XXXXXX")) { errno = EINVAL; return -1; }
    for (int tries = 0; tries < 100; tries++) {
        unsigned char r[6];
        if (getentropy(r, sizeof r) != 0) for (int i = 0; i < 6; i++) r[i] = (unsigned char)rand();
        for (int i = 0; i < 6; i++) tpl[n - 6 + i] = set[r[i] % (sizeof set - 1)];
        const int fd = open(tpl, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0 || errno != EEXIST) return fd;
    }
    errno = EEXIST;
    return -1;
}

char *mkdtemp(char *tpl) {
    static const char set[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    const size_t n = strlen(tpl);
    if (n < 6 || strcmp(tpl + n - 6, "XXXXXX")) { errno = EINVAL; return NULL; }
    for (int tries = 0; tries < 100; tries++) {
        unsigned char r[6];
        if (getentropy(r, sizeof r) != 0) for (int i = 0; i < 6; i++) r[i] = (unsigned char)rand();
        for (int i = 0; i < 6; i++) tpl[n - 6 + i] = set[r[i] % (sizeof set - 1)];
        if (mkdir(tpl, 0700) == 0) return tpl;
        if (errno != EEXIST) return NULL;
    }
    errno = EEXIST;
    return NULL;
}

FILE *tmpfile(void) {
    char p[] = "/tmp/tmpXXXXXX";
    const int fd = mkstemp(p);
    if (fd < 0) return NULL;
    FILE *f = fdopen(fd, "w+");
    if (!f) { close(fd); unlink(p); return NULL; }
    unlink(p);   // gone when closed, where the file system allows it
    return f;
}
