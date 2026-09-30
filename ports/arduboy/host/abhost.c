// abhost.c — runs the REAL apps/ab-<slug>/app.wasm (or an x86-64 AOT of it) on the PC under WAMR
// with the firmware's feature set (libiwasm.a of ports/host/build.sh); the "nv" imports are the
// harness stubs of harness.c (canvas, simulated clock and audio sink, scripted input). This is the
// verification harness of ports/arduboy: it checks the wasm32 build itself (AVR-style 16-bit pointer
// casts, PROGMEM tables, WASI reactor start-up, import signatures).
//
//   abhost <module> <fsdir> <presents> <out.ppm> [script]      (env AB_SNAP, AB_LANG as harness.c)
// Built by: bash ports/arduboy/build.sh (abtool.py). Part of ports/arduboy.
#define AB_WAMR_HOST 1
#include "harness.c"
#include "wasm_export.h"

static int32_t w_lang(wasm_exec_env_t e, char *b, uint32_t n) { (void)e; return nv_lang(b, n); }
static int32_t w_load(wasm_exec_env_t e, const char *nm, void *d, uint32_t n) { (void)e; return nv_load(nm, d, (int32_t)n); }
static int32_t w_save(wasm_exec_env_t e, const char *nm, const void *d, uint32_t n) { (void)e; return nv_save(nm, d, (int32_t)n); }
static void w_log(wasm_exec_env_t e, int32_t l, const char *m) { (void)e; nv_log(l, m); }
static void w_print(wasm_exec_env_t e, const char *m) { (void)e; nv_print(m); }
static int32_t w_millis(wasm_exec_env_t e) { (void)e; return nv_millis(); }
static int32_t w_rand(wasm_exec_env_t e) { (void)e; return nv_rand(); }
static int64_t w_time_unix(wasm_exec_env_t e) { (void)e; return nv_time_unix(); }
static void w_sleep(wasm_exec_env_t e, int32_t ms) { (void)e; nv_sleep_ms(ms); }
static void w_persist(wasm_exec_env_t e, int32_t on) { (void)e; nv_gfx_persist(on); }
static void w_clear(wasm_exec_env_t e, int32_t c) { (void)e; nv_gfx_clear(c); }
static void w_rect(wasm_exec_env_t e, int32_t x, int32_t y, int32_t w, int32_t h, int32_t c) { (void)e; nv_gfx_rect(x, y, w, h, c); }
static void w_circle(wasm_exec_env_t e, int32_t x, int32_t y, int32_t r, int32_t c) { (void)e; nv_gfx_circle(x, y, r, c); }
static void w_tri(wasm_exec_env_t e, int32_t a, int32_t b, int32_t c, int32_t d, int32_t f, int32_t g, int32_t col) {
    (void)e; nv_gfx_tri(a, b, c, d, f, g, col);
}
static void w_text(wasm_exec_env_t e, int32_t x, int32_t y, const char *s, int32_t c, int32_t sc) { (void)e; nv_gfx_text(x, y, s, c, sc); }
static int32_t w_text_width(wasm_exec_env_t e, const char *s, int32_t sc) { (void)e; return nv_gfx_text_width(s, sc); }
static void w_blit(wasm_exec_env_t e, const void *p, uint32_t len, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)e; nv_gfx_blit_raw(p, (int32_t)len, x, y, w, h);
}
static int32_t w_present(wasm_exec_env_t e) { (void)e; return nv_gfx_present(); }
static int32_t w_touch_count(wasm_exec_env_t e) { (void)e; return nv_gfx_touch_count(); }
static int32_t w_touch_point(wasm_exec_env_t e, int32_t i) { (void)e; return nv_gfx_touch_point_raw(i); }
static int32_t w_input(wasm_exec_env_t e) { (void)e; return nv_gfx_input_raw(); }
static int32_t w_pad(wasm_exec_env_t e) { (void)e; return nv_gfx_pad(); }
static int32_t w_back(wasm_exec_env_t e) { (void)e; return nv_gfx_back(); }
static int32_t w_pad_count(wasm_exec_env_t e) { (void)e; return nv_pad_count(); }
static int32_t w_pad_state(wasm_exec_env_t e, int32_t i, void *st, uint32_t n) { (void)e; return nv_pad_state(i, st, (int32_t)n); }
static int32_t w_audio_open(wasm_exec_env_t e, int32_t r, int32_t c) { (void)e; return nv_audio_open(r, c); }
static int32_t w_audio_write(wasm_exec_env_t e, const void *p, uint32_t n) { (void)e; return nv_audio_write(p, (int32_t)n); }
static int32_t w_audio_backlog(wasm_exec_env_t e) { (void)e; return nv_audio_backlog(); }
static void w_audio_close(wasm_exec_env_t e) { (void)e; nv_audio_close(); }

static NativeSymbol s_nv[] = {
    {"lang", (void *)w_lang, "(*~)i", NULL},
    {"load", (void *)w_load, "($*~)i", NULL},
    {"save", (void *)w_save, "($*~)i", NULL},
    {"log", (void *)w_log, "(i$)", NULL},
    {"print", (void *)w_print, "($)", NULL},
    {"millis", (void *)w_millis, "()i", NULL},
    {"rand", (void *)w_rand, "()i", NULL},
    {"time_unix", (void *)w_time_unix, "()I", NULL},
    {"sleep_ms", (void *)w_sleep, "(i)", NULL},
    {"gfx_persist", (void *)w_persist, "(i)", NULL},
    {"gfx_clear", (void *)w_clear, "(i)", NULL},
    {"gfx_rect", (void *)w_rect, "(iiiii)", NULL},
    {"gfx_circle", (void *)w_circle, "(iiii)", NULL},
    {"gfx_tri", (void *)w_tri, "(iiiiiii)", NULL},
    {"gfx_text", (void *)w_text, "(ii$ii)", NULL},
    {"gfx_text_width", (void *)w_text_width, "($i)i", NULL},
    {"gfx_blit", (void *)w_blit, "(*~iiii)", NULL},
    {"gfx_present", (void *)w_present, "()i", NULL},
    {"gfx_touch_count", (void *)w_touch_count, "()i", NULL},
    {"gfx_touch_point", (void *)w_touch_point, "(i)i", NULL},
    {"gfx_input", (void *)w_input, "()i", NULL},
    {"gfx_pad", (void *)w_pad, "()i", NULL},
    {"gfx_back", (void *)w_back, "()i", NULL},
    {"pad_count", (void *)w_pad_count, "()i", NULL},
    {"pad_state", (void *)w_pad_state, "(i*~)i", NULL},
    {"audio_open", (void *)w_audio_open, "(ii)i", NULL},
    {"audio_write", (void *)w_audio_write, "(*~)i", NULL},
    {"audio_backlog", (void *)w_audio_backlog, "()i", NULL},
    {"audio_close", (void *)w_audio_close, "()", NULL},
};

static unsigned char *read_file(const char *p, uint32_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)sz);
    if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); b = NULL; }
    fclose(f);
    *n = (uint32_t)sz;
    return b;
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: abhost <module> <fsdir> <presents> <out.ppm> [script]\n"); return 2; }
    setenv("AB_FS", argv[2], 1);
    if (hs_setup(argc - 2, argv + 2)) return 2;
    alarm(900);   // wall clock: generous, parallel builds compete for the CPU
    if (!wasm_runtime_init()) { fprintf(stderr, "runtime init failed\n"); return 1; }
    wasm_runtime_register_natives("nv", s_nv, sizeof s_nv / sizeof s_nv[0]);
    uint32_t size;
    unsigned char *buf = read_file(argv[1], &size);
    if (!buf) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    char err[128];
    wasm_module_t mod = wasm_runtime_load(buf, size, err, sizeof err);
    if (!mod) { fprintf(stderr, "load: %s\n", err); return 1; }
    char map[512];
    snprintf(map, sizeof map, "/::%s", argv[2]);
    const char *maps[1] = {map};
    const char *env[] = {"HOME=/"};
    wasm_runtime_set_wasi_args_ex(mod, NULL, 0, maps, 1, env, 1, NULL, 0, 0, 1, 2);
    InstantiationArgs ia;
    memset(&ia, 0, sizeof ia);
    ia.default_stack_size = 64 * 1024;
    ia.host_managed_heap_size = 0;
    ia.max_memory_pages = 3 * 16;   // manifest ram_budget: 3 MB
    wasm_module_inst_t inst = wasm_runtime_instantiate_ex(mod, &ia, err, sizeof err);
    if (!inst) { fprintf(stderr, "instantiate: %s\n", err); return 1; }
    wasm_exec_env_t ex = wasm_runtime_create_exec_env(inst, 64 * 1024);
    wasm_function_inst_t f = wasm_runtime_lookup_function(inst, "run");
    int ok = 0;
    if (!f) fprintf(stderr, "no export run\n");
    else if (wasm_runtime_call_wasm(ex, f, 0, NULL)) ok = 1;
    else {
        const char *exc = wasm_runtime_get_exception(inst);
        if (exc && strstr(exc, "wasi proc exit")) {
            const uint32_t code = wasm_runtime_get_wasi_exit_code(inst);
            ok = code == 0;
            if (!ok) printf("exit code %u\n", code);
        } else {
            printf("TRAP: %s\n", exc ? exc : "?");
        }
    }
    hs_finish();
    wasm_runtime_destroy_exec_env(ex);
    wasm_runtime_deinstantiate(inst);
    wasm_runtime_unload(mod);
    wasm_runtime_destroy();
    free(buf);
    return ok ? 0 : 1;
}
