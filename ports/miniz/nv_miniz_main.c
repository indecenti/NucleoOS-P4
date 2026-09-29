// nv_miniz_main.c — Terminal entry point for the miniz port: a small zip tool.
//
//   zip <archive.zip> <file...>      create archive.zip containing the given files (stored by
//                                    basename, no directory structure — Terminal's cwd is a flat
//                                    sandbox root already)
//   zip -l <archive.zip>             list entries: name, size, compressed size
//   zip -x <archive.zip> [outdir]    extract every entry into outdir (default: ".")
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "miniz.h"

static void die(const char *msg) {
    fprintf(stderr, "zip: %s\n", msg);
    exit(1);
}

static void usage(void) {
    fprintf(stderr, "usage: zip archive.zip file...\n"
                     "       zip -l archive.zip\n"
                     "       zip -x archive.zip [outdir]\n");
    exit(1);
}

// Create every directory named by a "/"-separated prefix of `path` (the trailing component is
// left alone — it's the file itself). Best-effort: mkdir() failing because a segment already
// exists is fine, anything else is reported by the caller's next fopen/extract failing anyway.
static void mkdirs_for(char *path) {
    for (char *p = path + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(path, 0777);
            *p = '/';
        }
    }
}

static int cmd_list(const char *archive) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, archive, 0)) die("can't open archive");

    mz_uint n = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        printf("%10llu  %10llu  %s\n", (unsigned long long)st.m_uncomp_size,
               (unsigned long long)st.m_comp_size, st.m_filename);
    }
    mz_zip_reader_end(&zip);
    return 0;
}

static int cmd_extract(const char *archive, const char *outdir) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, archive, 0)) die("can't open archive");

    mz_uint n = mz_zip_reader_get_num_files(&zip);
    char dst[512];
    for (mz_uint i = 0; i < n; i++) {
        if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;
        char name[256];
        mz_zip_reader_get_filename(&zip, i, name, sizeof(name));
        int len = snprintf(dst, sizeof(dst), "%s/%s", outdir, name);
        if (len < 0 || (size_t)len >= sizeof(dst)) {
            fprintf(stderr, "zip: skip (path too long): %s\n", name);
            continue;
        }
        mkdirs_for(dst);
        if (mz_zip_reader_extract_to_file(&zip, i, dst, 0)) printf("%s\n", name);
        else fprintf(stderr, "zip: failed to extract %s\n", name);
    }
    mz_zip_reader_end(&zip);
    return 0;
}

static int cmd_create(const char *archive, char **files, int nfiles) {
    remove(archive);   // mz_zip_writer_init_file appends if the file exists; start fresh
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_writer_init_file(&zip, archive, 0)) die("can't create archive");

    int status = 0;
    for (int i = 0; i < nfiles; i++) {
        const char *base = strrchr(files[i], '/');
        base = base ? base + 1 : files[i];
        if (!mz_zip_writer_add_file(&zip, base, files[i], NULL, 0, MZ_DEFAULT_COMPRESSION)) {
            fprintf(stderr, "zip: failed to add %s\n", files[i]);
            status = 1;
            continue;
        }
        printf("%s\n", base);
    }
    if (!mz_zip_writer_finalize_archive(&zip)) { fprintf(stderr, "zip: finalize failed\n"); status = 1; }
    mz_zip_writer_end(&zip);
    return status;
}

int main(int argc, char **argv) {
    if (argc < 2) usage();

    if (!strcmp(argv[1], "-l")) {
        if (argc != 3) usage();
        return cmd_list(argv[2]);
    }
    if (!strcmp(argv[1], "-x")) {
        if (argc < 3 || argc > 4) usage();
        return cmd_extract(argv[2], argc == 4 ? argv[3] : ".");
    }
    if (argc < 3) usage();
    return cmd_create(argv[1], argv + 2, argc - 2);
}
