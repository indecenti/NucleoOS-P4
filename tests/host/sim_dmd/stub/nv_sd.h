#pragma once
// The simulator's card is a plain folder.
#include <stdio.h>
static inline FILE *nv_sd_fopen(const char *path, const char *mode) { return fopen(path, mode); }
static inline int nv_sd_fclose(FILE *f) { return f ? fclose(f) : 0; }
