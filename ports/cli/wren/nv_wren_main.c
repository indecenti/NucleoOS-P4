// nv_wren_main.c — a small Wren front end for the NucleoOS Terminal (WASI).
//
// The official wren_cli needs libuv; this one is only the Wren VM (+ the optional meta and
// random modules) with:
//   wren                 interactive prompt: expressions print their value, statements run;
//                        a line with unbalanced (, [ or { continues on the next line
//   wren file.wren [a..] runs a script (module "main"); imports load "<name>.wren" from the
//                        script's folder
//   wren -e 'source'     runs a one-line program
// and a tiny "io" module compatible with wren_cli's names:
//   Stdin.readLine()        next line of input without the newline, or null at EOF
//   File.read(path)         the whole file as a String
//   File.write(path, text)  create/overwrite a file
//   File.exists(path)       Bool
//   Process.arguments       the script arguments (List of String)
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "wren.h"
#include "wren_vm.h"   // REPL: roll back module variables a failed compile declared

static bool s_quiet;          // swallow compile errors (REPL expression attempt)
static char s_dir[256] = "";  // folder of the main script, for imports
static int s_argc;
static char **s_argv;

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    if (b) b[n] = '\0';
    fclose(f);
    return b;
}

static void write_fn(WrenVM *vm, const char *text) {
    (void)vm;
    fputs(text, stdout);
    fflush(stdout);
}

static void error_fn(WrenVM *vm, WrenErrorType type, const char *module, int line, const char *msg) {
    (void)vm;
    switch (type) {
    case WREN_ERROR_COMPILE:
        if (!s_quiet) fprintf(stderr, "[%s line %d] %s\n", module ? module : "?", line, msg);
        break;
    case WREN_ERROR_RUNTIME:
        fprintf(stderr, "Runtime error: %s\n", msg);
        break;
    case WREN_ERROR_STACK_TRACE:
        fprintf(stderr, "  [%s line %d] in %s\n", module ? module : "?", line, msg);
        break;
    }
}

// ---- io module -------------------------------------------------------------------------------

static const char *IO_SRC =
    "class Stdin {\n"
    "  foreign static readLine()\n"
    "}\n"
    "class File {\n"
    "  foreign static read(path)\n"
    "  foreign static write(path, text)\n"
    "  foreign static exists(path)\n"
    "}\n"
    "class Process {\n"
    "  foreign static arguments\n"
    "}\n";

static void io_readline(WrenVM *vm) {
    static char buf[4096];
    fflush(stdout);
    if (!fgets(buf, sizeof buf, stdin)) { wrenSetSlotNull(vm, 0); return; }
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
    wrenSetSlotString(vm, 0, buf);
}

static void io_read(WrenVM *vm) {
    const char *p = wrenGetSlotString(vm, 1);
    FILE *f = p ? fopen(p, "rb") : NULL;
    if (!f) {
        wrenSetSlotString(vm, 0, "Cannot open file.");
        wrenAbortFiber(vm, 0);
        return;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    size_t got = b ? fread(b, 1, (size_t)n, f) : 0;
    fclose(f);
    if (!b) { wrenSetSlotString(vm, 0, "Out of memory."); wrenAbortFiber(vm, 0); return; }
    wrenSetSlotBytes(vm, 0, b, got);
    free(b);
}

static void io_write(WrenVM *vm) {
    const char *p = wrenGetSlotString(vm, 1);
    if (wrenGetSlotType(vm, 2) != WREN_TYPE_STRING) {
        wrenSetSlotString(vm, 0, "Text must be a string.");
        wrenAbortFiber(vm, 0);
        return;
    }
    int len = 0;
    const char *t = wrenGetSlotBytes(vm, 2, &len);
    FILE *f = p ? fopen(p, "wb") : NULL;
    if (!f || fwrite(t, 1, (size_t)len, f) != (size_t)len) {
        if (f) fclose(f);
        wrenSetSlotString(vm, 0, "Cannot write file.");
        wrenAbortFiber(vm, 0);
        return;
    }
    fclose(f);
    wrenSetSlotDouble(vm, 0, len);
}

static void io_exists(WrenVM *vm) {
    struct stat st;
    const char *p = wrenGetSlotString(vm, 1);
    wrenSetSlotBool(vm, 0, p && stat(p, &st) == 0);
}

static void io_arguments(WrenVM *vm) {
    wrenEnsureSlots(vm, 2);
    wrenSetSlotNewList(vm, 0);
    for (int i = 0; i < s_argc; i++) {
        wrenSetSlotString(vm, 1, s_argv[i]);
        wrenInsertInList(vm, 0, -1, 1);
    }
}

static WrenForeignMethodFn bind_foreign(WrenVM *vm, const char *module, const char *cls,
                                        bool is_static, const char *sig) {
    (void)vm;
    if (strcmp(module, "io") != 0 || !is_static) return NULL;
    if (!strcmp(cls, "Stdin") && !strcmp(sig, "readLine()")) return io_readline;
    if (!strcmp(cls, "File") && !strcmp(sig, "read(_)")) return io_read;
    if (!strcmp(cls, "File") && !strcmp(sig, "write(_,_)")) return io_write;
    if (!strcmp(cls, "File") && !strcmp(sig, "exists(_)")) return io_exists;
    if (!strcmp(cls, "Process") && !strcmp(sig, "arguments")) return io_arguments;
    return NULL;
}

static void free_source(WrenVM *vm, const char *name, WrenLoadModuleResult r) {
    (void)vm; (void)name;
    free((void *)r.source);
}

static WrenLoadModuleResult load_module(WrenVM *vm, const char *name) {
    (void)vm;
    WrenLoadModuleResult r = { 0 };
    if (!strcmp(name, "io")) { r.source = IO_SRC; return r; }
    if (!strcmp(name, "meta") || !strcmp(name, "random")) return r;   // built-in optional modules
    char path[512];
    snprintf(path, sizeof path, "%s%s.wren", s_dir, name);
    r.source = read_file(path);
    if (r.source) r.onComplete = free_source;
    return r;
}

// ---- REPL --------------------------------------------------------------------------------------

// Nesting depth of (, [, { outside strings and comments (>0: the input continues).
static int depth_of(const char *s) {
    int d = 0;
    bool str = false;
    for (; *s; s++) {
        if (str) {
            if (*s == '\\' && s[1]) s++;
            else if (*s == '"') str = false;
            continue;
        }
        if (*s == '"') str = true;
        else if (s[0] == '/' && s[1] == '/') { while (*s && *s != '\n') s++; if (!*s) break; }
        else if (*s == '(' || *s == '[' || *s == '{') d++;
        else if (*s == ')' || *s == ']' || *s == '}') d--;
    }
    return d;
}

static bool starts_with_word(const char *s, const char *w) {
    size_t n = strlen(w);
    return !strncmp(s, w, n) && !(s[n] == '_' || (s[n] >= 'a' && s[n] <= 'z') ||
                                  (s[n] >= 'A' && s[n] <= 'Z') || (s[n] >= '0' && s[n] <= '9'));
}

static bool is_statement(const char *s) {
    static const char *kw[] = { "var", "class", "import", "for", "while", "if", "return", "foreign",
                                "break", "continue", "static", "construct", NULL };
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '{' || *s == '\0' || (s[0] == '/' && (s[1] == '/' || s[1] == '*'))) return true;
    // System.print(x) returns x: as an expression it would show it twice
    if (!strncmp(s, "System.print", 12) || !strncmp(s, "System.write", 12)) return true;
    for (int i = 0; kw[i]; i++)
        if (starts_with_word(s, kw[i])) return true;
    return false;
}

static ObjModule *main_module(WrenVM *vm) {
    Value name = CONST_STRING(vm, "main");
    wrenPushRoot(vm, AS_OBJ(name));
    Value m = wrenMapGet(vm->modules, name);
    wrenPopRoot(vm);
    return IS_UNDEFINED(m) ? NULL : AS_MODULE(m);
}

// wrenInterpret on the REPL's module. A failed compile leaves the names it used implicitly
// declared (as line numbers) and the compiler only complains about undefined names it added
// itself, so later lines would see them as numbers: drop them again.
static WrenInterpretResult run(WrenVM *vm, const char *src) {
    ObjModule *mod = main_module(vm);
    int nv = mod ? mod->variables.count : 0, nn = mod ? mod->variableNames.count : 0;
    WrenInterpretResult r = wrenInterpret(vm, "main", src);
    if (r == WREN_RESULT_COMPILE_ERROR && mod) {
        mod->variables.count = nv;
        mod->variableNames.count = nn;
    }
    return r;
}

static void repl(WrenVM *vm) {
    wrenInterpret(vm, "main", "");   // create the module now, so run() always finds it
    printf("Wren %s - type an expression or a statement; EOF quits.\n", WREN_VERSION_STRING);
    size_t cap = 4096, len = 0;
    char *src = malloc(cap);
    char line[1024];
    for (;;) {
        fputs(len ? "... " : "> ", stdout);
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) break;
        size_t n = strlen(line);
        if (len + n + 1 > cap) {
            cap = (len + n + 1) * 2;
            src = realloc(src, cap);
        }
        memcpy(src + len, line, n + 1);
        len += n;
        if (depth_of(src) > 0) continue;
        if (!is_statement(src)) {
            // Try it as an expression first and print its value (unless null).
            size_t m = len + 64;
            char *e = malloc(m);
            size_t k = len;
            while (k && (src[k - 1] == '\n' || src[k - 1] == '\r' || src[k - 1] == ' ')) k--;
            snprintf(e, m, "{\n  var nv_r_ = (%.*s)\n  if (nv_r_ != null) System.print(nv_r_)\n}\n",
                     (int)k, src);
            s_quiet = true;
            WrenInterpretResult r = run(vm, e);
            s_quiet = false;
            free(e);
            if (r != WREN_RESULT_COMPILE_ERROR) { len = 0; continue; }
        }
        run(vm, src);
        len = 0;
    }
    putchar('\n');
    free(src);
}

int main(int argc, char **argv) {
    WrenConfiguration cfg;
    wrenInitConfiguration(&cfg);
    cfg.writeFn = write_fn;
    cfg.errorFn = error_fn;
    cfg.loadModuleFn = load_module;
    cfg.bindForeignMethodFn = bind_foreign;
    WrenVM *vm = wrenNewVM(&cfg);
    int rc = 0;

    if (argc >= 3 && !strcmp(argv[1], "-e")) {
        s_argc = argc - 3;
        s_argv = argv + 3;
        rc = wrenInterpret(vm, "main", argv[2]) == WREN_RESULT_SUCCESS ? 0 : 70;
    } else if (argc >= 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        puts("usage: wren                interactive prompt\n"
             "       wren file.wren [args] run a script\n"
             "       wren -e 'source'      run a one-line program\n"
             "       wren -v               version");
    } else if (argc >= 2 && (!strcmp(argv[1], "-v") || !strcmp(argv[1], "--version"))) {
        printf("wren %s\n", WREN_VERSION_STRING);
    } else if (argc >= 2) {
        char *src = read_file(argv[1]);
        if (!src) {
            fprintf(stderr, "wren: cannot open %s\n", argv[1]);
            rc = 66;
        } else {
            const char *slash = strrchr(argv[1], '/');
            if (slash && (size_t)(slash - argv[1] + 1) < sizeof s_dir) {
                memcpy(s_dir, argv[1], (size_t)(slash - argv[1] + 1));
                s_dir[slash - argv[1] + 1] = '\0';
            }
            s_argc = argc - 2;
            s_argv = argv + 2;
            WrenInterpretResult r = wrenInterpret(vm, "main", src);
            rc = r == WREN_RESULT_SUCCESS ? 0 : r == WREN_RESULT_COMPILE_ERROR ? 65 : 70;
            free(src);
        }
    } else {
        repl(vm);
    }
    wrenFreeVM(vm);
    return rc;
}
