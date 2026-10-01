// MicroPython for NucleoOS: `python` (REPL), `python file.py [args]`, `python -c "code"`.
// The Terminal runs it as a WASI console program; /sdcard/home is "/".
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "genhdr/mpversion.h"
#include "py/builtin.h"
#include "py/compile.h"
#include "py/gc.h"
#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/repl.h"
#include "shared/readline/readline.h"
#include "py/runtime.h"
#include "py/stackctrl.h"
#include "extmod/vfs.h"
#include "extmod/vfs_posix.h"
#include "shared/runtime/gchelper.h"

#ifndef NV_HEAP_KB
#define NV_HEAP_KB 1024      // first heap chunk; more is added on demand (split heap)
#endif
#define NV_PYSTACK_KB 64

static char *stack_top;
static mp_obj_t pystack[NV_PYSTACK_KB * 1024 / sizeof(mp_obj_t)];

static void stderr_strn(void *env, const char *str, size_t len) {
    (void)env;
    fwrite(str, 1, len, stderr);
}
const mp_print_t mp_stderr_print = { NULL, stderr_strn };

// ---- run code ------------------------------------------------------------------------------

typedef struct {
    mp_lexer_t *lex;          // source, or NULL with path
    const char *path;
    mp_parse_input_kind_t kind;
    bool is_repl;
} run_ctx_t;

static void run_body(void *ud) {
    run_ctx_t *c = ud;
    if (c->path) {
        c->lex = mp_lexer_new_from_file(qstr_from_str(c->path));
    }
    qstr source_name = c->lex->source_name;
    #if MICROPY_PY___FILE__
    if (c->kind == MP_PARSE_FILE_INPUT) {
        mp_store_global(MP_QSTR___file__, MP_OBJ_NEW_QSTR(source_name));
    }
    #endif
    mp_parse_tree_t pt = mp_parse(c->lex, c->kind);
    mp_obj_t fn = mp_compile(&pt, source_name, c->is_repl);
    mp_call_function_0(fn);
}

// Returns 0, or 1 after printing an uncaught exception; SystemExit gives its code (| 0x100).
static int run(run_ctx_t *c) {
    nlr_buf_t nlr;
    if (nlr_nv_try(&nlr, run_body, c) == 0) {
        return 0;
    }
    mp_obj_t exc = MP_OBJ_FROM_PTR(nlr.ret_val);
    if (mp_obj_is_subclass_fast(MP_OBJ_FROM_PTR(mp_obj_get_type(exc)), MP_OBJ_FROM_PTR(&mp_type_SystemExit))) {
        mp_obj_t v = mp_obj_exception_get_value(exc);
        mp_int_t code = 0;
        if (v != mp_const_none && !mp_obj_get_int_maybe(v, &code)) {
            mp_obj_print_helper(&mp_stderr_print, v, PRINT_STR);
            mp_print_str(&mp_stderr_print, "\n");
            code = 1;
        }
        return 0x100 | (code & 0xff);
    }
    mp_obj_print_exception(&mp_stderr_print, exc);
    return 1;
}

static int run_str(const char *src, mp_parse_input_kind_t kind, bool is_repl) {
    run_ctx_t c = { mp_lexer_new_from_str_len(MP_QSTR__lt_stdin_gt_, src, strlen(src), 0), NULL, kind, is_repl };
    return run(&c);
}

// Collect garbage where no object pointer can be in a WebAssembly local: between statements.
static void collect_top_level(void);

// ---- REPL ----------------------------------------------------------------------------------

static bool read_line(vstr_t *line, const char *prompt) {
    fputs(prompt, stdout);
    fflush(stdout);
    int ch;
    while ((ch = getchar()) != EOF && ch != '\n') {
        if (ch != '\r') {
            vstr_add_byte(line, ch);
        }
    }
    return ch != EOF || line->len > 0;
}

static int repl(void) {
    printf("MicroPython " MICROPY_GIT_TAG " on NucleoOS; type exit() or Ctrl-D to leave\n");
    vstr_t line;
    vstr_init(&line, 64);
    for (;;) {
        collect_top_level();
        vstr_reset(&line);
        if (!read_line(&line, ">>> ")) {
            break;
        }
        if (line.len == 0) {
            continue;
        }
        while (mp_repl_continue_with_input(vstr_null_terminated_str(&line))) {
            vstr_t more;
            vstr_init(&more, 32);
            if (!read_line(&more, "... ")) {
                vstr_clear(&more);
                break;
            }
            vstr_add_byte(&line, '\n');
            vstr_add_strn(&line, more.buf, more.len);
            vstr_clear(&more);
        }
        int r = run_str(vstr_null_terminated_str(&line), MP_PARSE_SINGLE_INPUT, true);
        if (r & 0x100) {
            vstr_clear(&line);
            return r & 0xff;
        }
    }
    putchar('\n');
    vstr_clear(&line);
    return 0;
}

// ---- main ----------------------------------------------------------------------------------

static void mount_root(void) {
    mp_obj_t args[2] = {
        MP_OBJ_TYPE_GET_SLOT(&mp_type_vfs_posix, make_new)(&mp_type_vfs_posix, 0, 0, NULL),
        MP_OBJ_NEW_QSTR(MP_QSTR__slash_),
    };
    mp_vfs_mount(2, args, (mp_map_t *)&mp_const_empty_map);
    MP_STATE_VM(vfs_cur) = MP_STATE_VM(vfs_mount_table);
}

static void mount_body(void *ud) {
    (void)ud;
    mount_root();
}

static void usage(void) {
    puts("usage: python [file.py [args...] | -c code | -m module]\n"
        "  no argument: interactive prompt. Files are under /sdcard/home (seen as /).");
}

int main(int argc, char **argv) {
    int stack_dummy;
    stack_top = (char *)&stack_dummy;
    mp_stack_ctrl_init();
    mp_stack_set_limit(200 * 1024);   // of the 256 KB C stack (ports/build.sh)
    mp_pystack_init(pystack, &pystack[MP_ARRAY_SIZE(pystack)]);
    char *heap = malloc(NV_HEAP_KB * 1024);
    if (!heap) {
        fputs("python: out of memory\n", stderr);
        return 1;
    }
    gc_init(heap, heap + NV_HEAP_KB * 1024);
    mp_init();
    #ifdef NV_GC_STRESS
    MP_STATE_MEM(gc_alloc_threshold) = NV_GC_STRESS;   // test builds: collect every N blocks
    #endif

    nlr_buf_t nlr;
    if (nlr_nv_try(&nlr, mount_body, NULL) != 0) {
        mp_obj_print_exception(&mp_stderr_print, MP_OBJ_FROM_PTR(nlr.ret_val));
    }
    mp_sys_path = mp_obj_new_list(0, NULL);
    mp_obj_list_append(mp_sys_path, MP_OBJ_NEW_QSTR(MP_QSTR_));
    mp_obj_list_append(mp_sys_path, MP_OBJ_NEW_QSTR(qstr_from_str("/lib")));
    mp_obj_list_append(mp_sys_path, MP_OBJ_NEW_QSTR(qstr_from_str("/py")));
    mp_obj_list_init(MP_OBJ_TO_PTR(mp_sys_argv), 0);

    // -X options of the unix port (emit=..., heapsize=...) are accepted and ignored
    while (argc >= 3 && !strcmp(argv[1], "-X")) {
        argv += 2;
        argc -= 2;
    }
    int ret = 0;
    if (argc < 2) {
        mp_obj_list_append(mp_sys_argv, MP_OBJ_NEW_QSTR(MP_QSTR_));
        ret = repl();
    } else if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        usage();
    } else if (!strcmp(argv[1], "-c") || !strcmp(argv[1], "-m")) {
        if (argc < 3) {
            usage();
            ret = 2;
        } else {
            mp_obj_list_append(mp_sys_argv, MP_OBJ_NEW_QSTR(qstr_from_str(argv[1])));
            for (int i = 3; i < argc; i++) {
                mp_obj_list_append(mp_sys_argv, MP_OBJ_NEW_QSTR(qstr_from_str(argv[i])));
            }
            if (argv[1][1] == 'c') {
                ret = run_str(argv[2], MP_PARSE_FILE_INPUT, false);
            } else {
                char code[160];
                snprintf(code, sizeof code, "import %s", argv[2]);
                ret = run_str(code, MP_PARSE_FILE_INPUT, false);
            }
        }
    } else {
        for (int i = 1; i < argc; i++) {
            mp_obj_list_append(mp_sys_argv, MP_OBJ_NEW_QSTR(qstr_from_str(argv[i])));
        }
        // the script's directory first on sys.path, like CPython
        const char *slash = strrchr(argv[1], '/');
        if (slash) {
            size_t dir_len = slash == argv[1] ? 1 : (size_t)(slash - argv[1]);   // "/x.py" -> "/"
            mp_obj_list_store(mp_sys_path, MP_OBJ_NEW_SMALL_INT(0), mp_obj_new_str(argv[1], dir_len));
        }
        run_ctx_t c = { NULL, argv[1], MP_PARSE_FILE_INPUT, false };
        ret = run(&c);
    }
    fflush(stdout);
    mp_deinit();
    free(heap);
    return ret & 0xff;
}

// ---- GC ------------------------------------------------------------------------------------
// WebAssembly keeps most values in locals the collector cannot see. The build runs Binaryen's
// --spill-pointers pass, which copies every pointer live across a call to the C shadow stack, so
// scanning that stack (as on any MCU port) finds them. Without it (NV_GC_TOP_LEVEL_ONLY), the
// heap only grows during a statement and is collected between REPL statements.

#if NV_GC_TOP_LEVEL_ONLY
static bool gc_pending;
size_t gc_get_max_new_split(void) {
    return 4 * 1024 * 1024;   // per added chunk; the app's memory budget caps the total
}
void gc_collect(void) {
    gc_pending = true;
}
static void collect_top_level(void) {
    if (gc_pending) {
        gc_pending = false;
        gc_collect_start();
        gc_collect_end();
    }
}
#else
size_t gc_get_max_new_split(void) {
    return 4 * 1024 * 1024;   // per added chunk; the app's memory budget caps the total
}
void gc_collect(void) {
    gc_collect_start();
    #ifdef __wasm__
    volatile void *dummy = NULL;   // the bottom of the shadow stack (it grows down)
    gc_collect_root((void **)&dummy, ((uintptr_t)stack_top - (uintptr_t)&dummy) / sizeof(void *));
    #else
    gc_helper_collect_regs_and_stack();   // native test build
    #endif
    gc_collect_end();
}
static void collect_top_level(void) {
}
#endif

// ---- HAL -----------------------------------------------------------------------------------

void nlr_jump_fail(void *val) {
    fprintf(stderr, "python: uncaught exception %p\n", val);
    exit(1);
}

#ifndef NDEBUG
void MP_WEAK __assert_func(const char *file, int line, const char *func, const char *expr) {
    fprintf(stderr, "python: assertion '%s' failed, %s:%d\n", expr, file, line);
    exit(1);
}
#endif

int mp_hal_stdin_rx_chr(void) {
    int c = getchar();
    return c == EOF ? 4 : c;   // EOF -> Ctrl-D
}

mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len) {
    return fwrite(str, 1, len, stdout);
}

static uint64_t now_ns(clockid_t id) {
    struct timespec ts;
    clock_gettime(id, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}
mp_uint_t mp_hal_ticks_ms(void) {
    return now_ns(CLOCK_MONOTONIC) / 1000000;
}
mp_uint_t mp_hal_ticks_us(void) {
    return now_ns(CLOCK_MONOTONIC) / 1000;
}
uint64_t mp_hal_time_ns(void) {
    return now_ns(CLOCK_REALTIME);
}
void mp_hal_delay_ms(mp_uint_t ms) {
    usleep(ms * 1000);
}
void mp_hal_delay_us(mp_uint_t us) {
    usleep(us);
}

void mp_hal_get_random(size_t n, void *buf) {
    while (n) {
        size_t k = n > 256 ? 256 : n;
        if (getentropy(buf, k) != 0) {
            memset(buf, 0, k);
        }
        buf = (char *)buf + k;
        n -= k;
    }
}
uint32_t mp_nv_random_seed(void) {
    uint32_t r;
    mp_hal_get_random(sizeof r, &r);
    return r;
}

uintptr_t mp_hal_stdio_poll(uintptr_t poll_flags) {
    (void)poll_flags;
    return 0;
}

// input(): the Terminal already edits the line, so read it whole.
int readline(vstr_t *line, const char *prompt) {
    return read_line(line, prompt) ? 0 : 4;   // CHAR_CTRL_D at EOF
}
