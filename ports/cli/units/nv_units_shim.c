// nv_units_shim.c — GNU units for the NucleoOS Terminal (WASI).
//
// Store packages carry no data files, so the unit database (definitions.units and the files it
// !includes: currency, cpi, elements) and locale_map.txt are compiled in (units_data.h, generated
// by ports/cli/build.sh). units.c is built with UNITSFILE/LOCALEMAP = /units/... and
// -Dfopen=nv_units_fopen: a real file wins (e.g. your own /units/currency.units in the SD card's
// home folder), otherwise a path in /units/ is served from memory. Your own definitions go in
// /.units (HOME is "/").
//
// No pager and no readline: the Terminal hands over whole lines, so isatty() answers "no"
// (units then never opens a pager through popen) and the pager-based `help` reports it cannot.
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "units_data.h"   // struct nv_udata nv_udata[]; NV_UDATA_COUNT

#define DATADIR_ "/units/"

FILE *nv_units_fopen(const char *path, const char *mode) {
    FILE *f = fopen(path, mode);
    if (f || mode[0] != 'r' || strncmp(path, DATADIR_, strlen(DATADIR_)) != 0) return f;
    for (int i = 0; i < NV_UDATA_COUNT; i++)
        if (!strcmp(path + strlen(DATADIR_), nv_udata[i].file))
            return fmemopen((void *)nv_udata[i].data, nv_udata[i].len, "rb");
    return NULL;
}

int nv_units_isatty(int fd) { (void)fd; return 0; }

FILE *popen(const char *cmd, const char *mode) { (void)cmd; (void)mode; errno = ENOSYS; return NULL; }
int pclose(FILE *f) { (void)f; return -1; }
int system(const char *cmd) { (void)cmd; errno = ENOSYS; return -1; }
