// nv_if.h — force-included (-include) into every interpreter source of the IF catalog apps.
// fopen() of the story name resolves to the story bytes embedded in the module (fmemopen), so a
// single app.wasm carries interpreter + game; everything else (saves, transcripts) goes to the
// real file system, i.e. the app's data folder (manifest permission "fs" makes it "/").
#pragma once
#include <stdio.h>
FILE *nv_if_fopen(const char *name, const char *mode);
#define fopen(n, m) nv_if_fopen((n), (m))
int mkstemp(char *tmpl);
