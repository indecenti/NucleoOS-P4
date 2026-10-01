// nv_units_port.h — force-included into units.c (ports/cli/build.sh): wasi-libc declares no
// popen/pclose; nv_units_shim.c provides stubs that always fail (no pager on the device).
#pragma once
#include <stdio.h>
FILE *popen(const char *cmd, const char *mode);
int pclose(FILE *f);
