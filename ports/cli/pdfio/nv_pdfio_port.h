// nv_pdfio_port.h — force-included into PDFio's sources (build_wsl.sh): wasi-libc has
// <sys/random.h> with getentropy() but no getrandom(); nv_pdfio_main.c supplies it.
#pragma once
#include <sys/types.h>
ssize_t getrandom(void *buf, size_t len, unsigned flags);
