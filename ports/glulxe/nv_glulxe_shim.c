// nv_glulxe_shim.c — the one POSIX call CheapGlk needs that wasi-libc lacks.
// mkstemp(): CheapGlk creates temporary files (glk_fileref_create_temp) this way. Under WASI the
// program's "/" is its data folder (manifest permission "home" -> /sdcard/home), so a counter-based
// name with O_CREAT|O_EXCL is enough.
#include <fcntl.h>
#include <string.h>
#include <errno.h>

int mkstemp(char *tmpl)
{
    static unsigned counter = 0;
    size_t n = tmpl ? strlen(tmpl) : 0;
    if (n < 6 || strcmp(tmpl + n - 6, "XXXXXX") != 0) { errno = EINVAL; return -1; }
    for (int tries = 0; tries < 1000; tries++) {
        unsigned v = ++counter * 2654435761u;
        for (int i = 0; i < 6; i++) { tmpl[n - 6 + i] = "abcdefghijklmnopqrstuvwxyz0123456789"[v % 36]; v /= 36; }
        int fd = open(tmpl, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0 || errno != EEXIST) return fd;
    }
    errno = EEXIST;
    return -1;
}
