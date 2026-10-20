// nv_posix.h — declarations of the POSIX calls wasi-libc leaves out, implemented (as "not
// available", or for real where WASI allows) by nv_posix.c. Force-included (-include) into the
// full-screen ports so their sources compile unchanged.
#pragma once
#include <stdio.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

int   system(const char *cmd);
FILE *popen(const char *cmd, const char *mode);
int   pclose(FILE *f);
pid_t fork(void);
int   execv(const char *path, char *const argv[]);
int   execvp(const char *file, char *const argv[]);
int   execl(const char *path, const char *arg, ...);
int   execlp(const char *file, const char *arg, ...);
int   kill(pid_t pid, int sig);
char *ttyname(int fd);
mode_t umask(mode_t mask);
int   mkfifo(const char *path, mode_t mode);
int   mkstemp(char *tpl);
char *mkdtemp(char *tpl);
FILE *tmpfile(void);

#ifdef __cplusplus
}
#endif
