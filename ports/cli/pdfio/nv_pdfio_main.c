// nv_pdfio_main.c — PDFio's example tools for the NucleoOS Terminal (WASI), one program:
//   pdftotext FILE.pdf     the text of every page (examples/pdf2text.c)
//   pdfinfo FILE.pdf       title, author, pages, sizes (examples/pdfioinfo.c)
//   pdfmerge -o OUT.pdf IN.. joins PDF files (examples/pdfiomerge.c)
// The shell runs "pdftotext ARGS" as the app pdfio with "pdftotext ARGS" (term_sh kMultiCall).
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

// getrandom() for pdfio-crypto.c (encrypted PDFs): WASI's getentropy, at most 256 bytes a call
ssize_t getrandom(void *buf, size_t len, unsigned flags) {
    (void)flags;
    for (size_t done = 0; done < len; ) {
        size_t n = len - done > 256 ? 256 : len - done;
        if (getentropy((char *)buf + done, n) != 0) return -1;
        done += n;
    }
    return (ssize_t)len;
}

int pdf2text_main(int, char **);
int pdfioinfo_main(int, char **);
int pdfiomerge_main(int, char **);

int main(int argc, char **argv) {
    if (argc >= 2) {
        if (!strcmp(argv[1], "pdftotext") || !strcmp(argv[1], "pdf2text")) return pdf2text_main(argc - 1, argv + 1);
        if (!strcmp(argv[1], "pdfinfo")) return pdfioinfo_main(argc - 1, argv + 1);
        if (!strcmp(argv[1], "pdfmerge")) return pdfiomerge_main(argc - 1, argv + 1);
    }
    puts("PDFio 1.6.5 tools:\n"
         "  pdftotext FILE.pdf        text of every page\n"
         "  pdfinfo FILE.pdf          title, author, pages, page sizes\n"
         "  pdfmerge -o OUT.pdf IN.pdf... join PDF files");
    return argc >= 2 ? 1 : 0;
}
