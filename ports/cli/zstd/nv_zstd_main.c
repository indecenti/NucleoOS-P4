// nv_zstd_main.c — zstd for the NucleoOS Terminal (WASI), one program for nine commands.
//
// The shell runs "gunzip FILE" as the app zstd with "gunzip FILE" (term_sh kMultiCall); zstd's
// own CLI picks its behaviour from argv[0] (zstd, unzstd, zstdcat, gzip, gunzip, zcat, xz, unxz,
// xzcat), so the first argument becomes argv[0]. Plain "zstd ARGS" is passed through unchanged.
#include <string.h>

int zstd_cli_main(int argc, char **argv);

static const char *const kNames[] = { "zstd", "unzstd", "zstdcat", "zcat", "gzip", "gunzip",
                                      "xz", "unxz", "xzcat", "lzma", "unlzma" };

int main(int argc, char **argv) {
    if (argc >= 2)
        for (size_t i = 0; i < sizeof kNames / sizeof kNames[0]; i++)
            if (!strcmp(argv[1], kNames[i])) {
                if (!strcmp(argv[1], "xzcat")) argv[1] = (char *)"zcat";   // zcat reads every format
                return zstd_cli_main(argc - 1, argv + 1);
            }
    return zstd_cli_main(argc, argv);
}
