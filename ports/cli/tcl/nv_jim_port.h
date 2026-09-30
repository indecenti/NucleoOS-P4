// nv_jim_port.h — force-included into every Jim Tcl source for NucleoOS (WASI).
#pragma once
char *mktemp(char *tmpl);   // wasi-libc lacks it (file tempfile / aio): nv_jim_shim.c
