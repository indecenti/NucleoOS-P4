// pwd.h — WASI has no user database. NucleoOS has one user: lookups find nobody (nv_posix.c), so
// programs fall back to $HOME (set to "/" by the Terminal).
#pragma once
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct passwd {
    char *pw_name, *pw_passwd;
    uid_t pw_uid;
    gid_t pw_gid;
    char *pw_gecos, *pw_dir, *pw_shell;
};

struct passwd *getpwnam(const char *name);
struct passwd *getpwuid(uid_t uid);

#ifdef __cplusplus
}
#endif
