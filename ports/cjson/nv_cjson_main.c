// nv_cjson_main.c — Terminal entry point for the cJSON port: a small JSON tool.
//
//   cjson [-c] [-l] [-q PATH] [FILE]
//
//   -c        compact output (default: pretty, 2-space indent)
//   -l        JSON-Lines mode: FILE (or stdin) is one JSON value per line (e.g.
//             /sdcard/data/anima/memory.jsonl, conv/<id>.j) — each line is parsed and printed
//             on its own instead of the whole input being one JSON document
//   -q PATH   descend into the value before printing, e.g. -q .permissions or -q .items.0.name
//             (dot-separated; a segment that is all digits indexes an array)
//   FILE      read from this file; omitted or "-" reads stdin
//
// No file/query given: parses and validates, pretty-prints on success, reports a parse error on
// failure (exit 1) — a plain "is this JSON valid, and what does it look like" tool, e.g. for
// apps/<id>/manifest.json or any config file on the SD card.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/types.h>
#include "cJSON.h"

static void die(const char *msg) {
    fprintf(stderr, "cjson: %s\n", msg);
    exit(1);
}

// Read a whole stream into a NUL-terminated malloc'd buffer.
static char *slurp(FILE *f, size_t *out_len) {
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) die("out of memory");
    for (;;) {
        if (len + 1 >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); die("out of memory"); }
            buf = nb;
        }
        size_t n = fread(buf + len, 1, cap - len - 1, f);
        len += n;
        if (n == 0) break;
    }
    buf[len] = '\0';
    if (out_len) *out_len = len;
    return buf;
}

// Walk a dotted path ("permissions.0.name") into `root`. Returns NULL (and prints nothing) if
// any segment doesn't resolve.
static cJSON *walk_path(cJSON *root, const char *path) {
    if (!path || !*path) return root;
    if (*path == '.') path++;
    char *copy = strdup(path);
    if (!copy) die("out of memory");
    cJSON *cur = root;
    char *save = NULL;
    for (char *seg = strtok_r(copy, ".", &save); seg && cur; seg = strtok_r(NULL, ".", &save)) {
        bool digits = *seg != '\0';
        for (const char *p = seg; *p; p++) {
            if (*p < '0' || *p > '9') { digits = false; break; }
        }
        cur = digits ? cJSON_GetArrayItem(cur, atoi(seg)) : cJSON_GetObjectItemCaseSensitive(cur, seg);
    }
    free(copy);
    return cur;
}

// cJSON_Print's default pretty-printer indents with a raw tab per level, which jumps a full
// terminal tab-stop on a small monospace screen. Replace each tab with two spaces on the way out.
static void put_pretty(const char *s) {
    for (const char *p = s; *p; p++) {
        if (*p == '\t') fputs("  ", stdout);
        else fputc(*p, stdout);
    }
    fputc('\n', stdout);
}

static void print_value(cJSON *v, bool compact) {
    char *s = compact ? cJSON_PrintUnformatted(v) : cJSON_Print(v);
    if (!s) die("out of memory formatting output");
    if (compact) puts(s);
    else put_pretty(s);
    cJSON_free(s);
}

int main(int argc, char **argv) {
    bool compact = false, lines = false;
    const char *query = NULL, *path = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c")) compact = true;
        else if (!strcmp(argv[i], "-l")) lines = true;
        else if (!strcmp(argv[i], "-q") && i + 1 < argc) query = argv[++i];
        else if (!path) path = argv[i];
        else die("usage: cjson [-c] [-l] [-q PATH] [FILE]");
    }

    FILE *f = stdin;
    if (path && strcmp(path, "-") != 0) {
        f = fopen(path, "rb");
        if (!f) { fprintf(stderr, "cjson: can't open %s\n", path); return 1; }
    }

    int status = 0;

    if (lines) {
        char *line = NULL;
        size_t cap = 0;
        ssize_t n;
        int lineno = 0;
        while ((n = getline(&line, &cap, f)) >= 0) {
            lineno++;
            while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
            if (n == 0) continue;
            cJSON *doc = cJSON_Parse(line);
            if (!doc) {
                fprintf(stderr, "cjson: line %d: parse error near: %.40s\n", lineno,
                        cJSON_GetErrorPtr());
                status = 1;
                continue;
            }
            cJSON *v = query ? walk_path(doc, query) : doc;
            if (v) print_value(v, compact);
            else fprintf(stderr, "cjson: line %d: path not found\n", lineno);
            cJSON_Delete(doc);
        }
        free(line);
    } else {
        char *text = slurp(f, NULL);
        cJSON *doc = cJSON_Parse(text);
        free(text);
        if (!doc) {
            fprintf(stderr, "cjson: parse error near: %.40s\n", cJSON_GetErrorPtr());
            status = 1;
        } else {
            cJSON *v = query ? walk_path(doc, query) : doc;
            if (v) print_value(v, compact);
            else { fprintf(stderr, "cjson: path not found\n"); status = 1; }
            cJSON_Delete(doc);
        }
    }

    if (f != stdin) fclose(f);
    return status;
}
