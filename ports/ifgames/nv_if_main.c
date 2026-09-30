// nv_if_main.c — the part every IF catalog app shares: embedded story + the fopen() redirect +
// main(). #included at the end of the generated per-game file (ports/_src/ifgames_gen/<slug>.c),
// which defines before it:
//
//   NV_IF_TITLE        "Title"                  banner line 1
//   NV_IF_CREDIT       "by X (year), licence"   banner line 2
//   NV_IF_STORY_NAME   "<slug>.z5" / ".ulx"     name the interpreter opens (-> embedded bytes)
//   NV_IF_STORY_SIZE   unpacked size
//   nv_if_story_z[]    the story file, raw deflate (#embed)
//   NV_IF_FROTZ or NV_IF_GLULXE
//
// Interpreters: Frotz 2.55 "dumb" interface (Z-machine v1-v8, GPL-2.0-or-later) and Glulxe +
// CheapGlk (Glulx, MIT). Both are compiled unmodified with -Dmain=nv_upstream_main and
// -include nv_if.h (fopen -> nv_if_fopen). Saves go to the app's data folder ("/").
#undef fopen
#undef main
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

// The story is embedded raw-deflated (gen.py) and inflated once, on first open, with miniz's tinfl.
#define MINIZ_NO_TIME
#include "miniz_tinfl.c"

static unsigned char *s_story;

FILE *nv_if_fopen(const char *name, const char *mode)
{
    if (name) {
        const char *base = strrchr(name, '/');
        base = base ? base + 1 : name;
        if (!strcmp(base, NV_IF_STORY_NAME) && mode && mode[0] == 'r') {
            if (!s_story) {
                size_t n = 0;
                s_story = tinfl_decompress_mem_to_heap(nv_if_story_z, sizeof nv_if_story_z, &n, 0);
                if (!s_story || n != NV_IF_STORY_SIZE) {
                    fprintf(stderr, "cannot unpack the story (out of memory?)\n");
                    free(s_story); s_story = NULL;
                    errno = ENOMEM;
                    return NULL;
                }
            }
            return fmemopen(s_story, NV_IF_STORY_SIZE, "rb");
        }
    }
    return fopen(name, mode);
}

// CheapGlk creates temporary files with mkstemp(), which wasi-libc lacks (same shim as
// ports/glulxe/nv_glulxe_shim.c). Unused by Frotz but harmless.
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

int nv_upstream_main(int argc, char **argv);

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("%s\n%s\n", NV_IF_TITLE, NV_IF_CREDIT);
#ifdef NV_IF_FROTZ
    printf("Interpreter: Frotz 2.55 (GPL-2.0-or-later). SAVE / RESTORE use this app's folder.\n\n");
    fflush(stdout);
    // -m: no [MORE] prompts (the Terminal scrolls); -q: no startup banner; -w: wrap width.
    static char *args[] = {"dfrotz", "-m", "-q", "-w", "72", "-h", "250", NV_IF_STORY_NAME, NULL};
#else
    printf("Interpreter: Glulxe + CheapGlk (MIT). SAVE / RESTORE use this app's folder.\n\n");
    fflush(stdout);
    // -u: UTF-8 output (CheapGlk defaults to Latin-1; the Terminal renders UTF-8).
    static char *args[] = {"glulxe", "-q", "-u", "-w", "72", NV_IF_STORY_NAME, NULL};
#endif
    return nv_upstream_main((int)(sizeof args / sizeof args[0]) - 1, args);
}
