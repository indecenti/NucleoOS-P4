// wasihello — WASI self-test for NucleoOS. Standard C (wasi-libc) end to end: stdio into the app
// panel, malloc, time and sleep, and files confined to the app's own data folder. Every check
// prints "ok" or "FAIL"; the exit code is the number of failures.
//
// Build: .\sdk\build_app.ps1 -AppDir apps\wasihello -Wasi
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "nucleo_sdk.h"

static int s_fail, s_total;

static void check(int ok, const char *what) {
    s_total++;
    if (ok) {
        printf("ok   %s\n", what);
    } else {
        s_fail++;
        printf("FAIL %s (errno %d: %s)\n", what, errno, strerror(errno));
    }
}

static int cmp_int(const void *a, const void *b) {
    const int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

static int write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    const int ok = fputs(text, f) >= 0;
    return fclose(f) == 0 && ok;
}

static int file_is(const char *path, const char *text) {
    char buf[64] = "";
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    const size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strcmp(buf, text) == 0;
}

static long elapsed_ms(const struct timespec *a, const struct timespec *b) {
    return (b->tv_sec - a->tv_sec) * 1000L + (b->tv_nsec - a->tv_nsec) / 1000000L;
}

int main(int argc, char **argv) {
    printf("WASI on NucleoOS: argv[0]=%s, NUCLEO_APP=%s\n", argc > 0 ? argv[0] : "?",
           getenv("NUCLEO_APP") ? getenv("NUCLEO_APP") : "(unset)");

    // --- libc basics ---
    char num[32];
    snprintf(num, sizeof num, "%.3f", 3.14159);
    check(strcmp(num, "3.142") == 0, "printf %f formatting");

    // The manifest ram_budget (1 MB) caps the whole linear memory: 256 KB fits, 2 MB must not.
    void *volatile big = malloc(2u << 20);   // volatile: else clang elides the unused malloc/free
    check(big == NULL, "ram_budget caps memory.grow (malloc 2 MB fails)");
    free(big);
    enum { N = 4096 };
    int *v = malloc(16 * N * sizeof *v);   // 256 KB: grows linear memory past its initial size
    check(v != NULL, "malloc 256 KB");
    if (v) {
        unsigned seed = 12345;
        for (int i = 0; i < N; i++) { seed = seed * 1103515245u + 12345u; v[i] = (int)(seed >> 8); }
        qsort(v, N, sizeof *v, cmp_int);
        int sorted = 1;
        for (int i = 1; i < N; i++) if (v[i - 1] > v[i]) { sorted = 0; break; }
        check(sorted, "qsort 4K ints");
        free(v);
    }

    // --- clocks ---
    time_t now = time(NULL);
    check(now > 1700000000, "time() is wall-clock (SNTP/RTC)");
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    usleep(50 * 1000);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    const long slept = elapsed_ms(&t0, &t1);
    printf("     usleep(50 ms) took %ld ms\n", slept);
    check(slept >= 45 && slept < 500, "usleep really sleeps");

    // --- files: "/" is /sdcard/apps/wasihello/data ---
    check(write_file("hello.txt", "ciao dal WASI\n"), "fopen/fputs relative path");
    check(file_is("/hello.txt", "ciao dal WASI\n"), "read back via absolute path");
    struct stat st;
    check(stat("hello.txt", &st) == 0 && st.st_size == 14, "stat size");
    check(mkdir("sub", 0777) == 0 || errno == EEXIST, "mkdir");
    check(write_file("sub/a.txt", "A"), "file in subdirectory");
    check(write_file("x.txt", "old"), "second file");
    check(rename("hello.txt", "x.txt") == 0, "rename over an existing file");
    check(file_is("x.txt", "ciao dal WASI\n"), "rename replaced the target");

    int seen = 0;
    DIR *d = opendir(".");
    check(d != NULL, "opendir");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (!strcmp(e->d_name, "x.txt") || !strcmp(e->d_name, "sub")) seen++;
        }
        closedir(d);
    }
    check(seen == 2, "readdir lists x.txt and sub");

    check(unlink("sub") != 0, "unlink refuses a directory");
    check(remove("sub/a.txt") == 0, "remove file");
    check(rmdir("sub") == 0, "rmdir");
    check(remove("x.txt") == 0, "remove renamed file");

    // --- sandbox: nothing outside the data folder is reachable ---
    check(fopen("../manifest.json", "r") == NULL, "no escape via ..");
    check(fopen("/../app.wasm", "r") == NULL, "no escape via /..");
    check(fopen("/sdcard/apps/wasihello/manifest.json", "r") == NULL, "no host absolute paths");

    printf("%d/%d checks passed\n", s_total - s_fail, s_total);
    fflush(stdout);   // before the nv import: a host without it (desktop iwasm) traps right here
    nv_toast(s_fail ? NV_TOAST_ERROR : NV_TOAST_OK, s_fail ? "WASI test: failures" : "WASI test OK");
    return s_fail;
}
