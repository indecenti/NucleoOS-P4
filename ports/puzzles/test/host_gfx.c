// host_gfx.c — PC host (WSL) that runs the real apps/puzzles/app.wasm (or an x86_64 AOT of it) in
// WAMR the way the device does: WASI with the app's data folder as "/", no host-managed heap, memory
// capped by the manifest's ram_budget, the "nv" gfx imports the app uses, and the call made on a
// pthread with the device worker's 48 KB native stack. Input comes from script.c; screenshots as PPM.
//
//   pz_host <module> <out dir> <data dir> [game index]
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "wasm_export.h"
#include "script.h"

#define CW 1024
#define CH 600
#define RAM_MB 8
#define WASM_STACK_KB 64          // manifest stack_kb (interpreter operand stack)
#define NATIVE_STACK_KB 48        // kWorkerNativeStk in components/nv_wasm

static uint16_t canvas[CW * CH];
static int frame, cur_down, cur_x, cur_y, pend_back, blits, persist;
static const char *outdir;

static void write_ppm(const char *name) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s.ppm", outdir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", CW, CH);
    static unsigned char rgb[CW * CH * 3];   // one write: the output dir may be a slow /mnt share
    for (int i = 0; i < CW * CH; i++) {
        uint16_t p = canvas[i];
        rgb[3 * i] = (unsigned char)((p >> 11) << 3);
        rgb[3 * i + 1] = (unsigned char)(((p >> 5) & 63) << 2);
        rgb[3 * i + 2] = (unsigned char)((p & 31) << 3);
    }
    fwrite(rgb, 1, sizeof rgb, f);
    fclose(f);
}

static int32_t n_width(wasm_exec_env_t e) { (void)e; return CW; }
static int32_t n_height(wasm_exec_env_t e) { (void)e; return CH; }
static void n_persist(wasm_exec_env_t e, int32_t on) { (void)e; persist = on; }
static void n_tone(wasm_exec_env_t e, int32_t f, int32_t ms) { (void)e; (void)f; (void)ms; }
static int32_t n_pad(wasm_exec_env_t e) { (void)e; return 0; }
static int32_t n_millis(wasm_exec_env_t e) { (void)e; return frame * 16; }
static int32_t n_rand(wasm_exec_env_t e) { (void)e; return rand(); }
static int64_t n_time(wasm_exec_env_t e) { (void)e; return 1759000000; }
static int32_t n_lang(wasm_exec_env_t e, char *buf, uint32_t len) {
    (void)e;
    if (len >= 3) { memcpy(buf, "en", 3); return 2; }
    return 0;
}
static void n_blit(wasm_exec_env_t e, void *px, uint32_t len, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)e;
    if (!px || w <= 0 || h <= 0 || (int64_t)w * h * 2 > len) { fprintf(stderr, "bad blit\n"); exit(3); }
    const uint16_t *s = px;
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++) {
            int X = x + c, Y = y + r;
            if (X >= 0 && Y >= 0 && X < CW && Y < CH) canvas[Y * CW + X] = s[r * w + c];
        }
    blits++;
}
static int32_t n_input(wasm_exec_env_t e) { (void)e; return (cur_down << 24) | (cur_y << 12) | cur_x; }
static int32_t n_back(wasm_exec_env_t e) { (void)e; int b = pend_back; pend_back = 0; return b; }
static int32_t n_present(wasm_exec_env_t e) {
    (void)e;
    const script_frame *f = script_at(frame++);
    if (!f) return 0;
    cur_down = f->down; cur_x = f->x; cur_y = f->y;
    if (f->back) pend_back = 1;
    if (f->shot[0]) write_ppm(f->shot);
    return 1;
}

static NativeSymbol natives[] = {
    { "gfx_width", (void *)n_width, "()i", NULL },     { "gfx_height", (void *)n_height, "()i", NULL },
    { "gfx_persist", (void *)n_persist, "(i)", NULL }, { "gfx_tone", (void *)n_tone, "(ii)", NULL },
    { "gfx_pad", (void *)n_pad, "()i", NULL },         { "millis", (void *)n_millis, "()i", NULL },
    { "rand", (void *)n_rand, "()i", NULL },           { "time_unix", (void *)n_time, "()I", NULL },
    { "lang", (void *)n_lang, "(*~)i", NULL },         { "gfx_blit", (void *)n_blit, "(*~iiii)", NULL },
    { "gfx_input", (void *)n_input, "()i", NULL },     { "gfx_back", (void *)n_back, "()i", NULL },
    { "gfx_present", (void *)n_present, "()i", NULL },
};

static wasm_module_inst_t inst;
static int ok;
static void *worker(void *arg) {
    (void)arg;
    wasm_runtime_init_thread_env();
    wasm_exec_env_t env = wasm_runtime_create_exec_env(inst, WASM_STACK_KB * 1024);
    wasm_function_inst_t fn = wasm_runtime_lookup_function(inst, "run");
    if (!env || !fn) { fprintf(stderr, "no exec env / no run export\n"); return NULL; }
    ok = wasm_runtime_call_wasm(env, fn, 0, NULL);
    if (!ok) fprintf(stderr, "trap: %s\n", wasm_runtime_get_exception(inst));
    wasm_runtime_destroy_exec_env(env);
    wasm_runtime_destroy_thread_env();
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: pz_host module outdir datadir [game]\n"); return 2; }
    outdir = argv[2];
    srand(1234);
    int n = script_build(40, argc > 4 ? argv[4] : NULL);
    if (!wasm_runtime_init()) return 1;
    wasm_runtime_register_natives("nv", natives, sizeof natives / sizeof natives[0]);
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc(sz);
    if (fread(buf, 1, sz, f) != (size_t)sz) return 1;
    fclose(f);
    char err[128], map[600];
    wasm_module_t mod = wasm_runtime_load(buf, (uint32_t)sz, err, sizeof err);
    if (!mod) { fprintf(stderr, "load: %s\n", err); return 1; }
    snprintf(map, sizeof map, "/::%s", argv[3]);
    const char *maps[1] = { map };
    wasm_runtime_set_wasi_args_ex(mod, NULL, 0, maps, 1, NULL, 0, NULL, 0, 0, 1, 2);
    InstantiationArgs ia;
    memset(&ia, 0, sizeof ia);
    ia.default_stack_size = WASM_STACK_KB * 1024;
    ia.host_managed_heap_size = 0;
    ia.max_memory_pages = RAM_MB * 16;
    inst = wasm_runtime_instantiate_ex(mod, &ia, err, sizeof err);
    if (!inst) { fprintf(stderr, "instantiate: %s\n", err); return 1; }
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, NATIVE_STACK_KB * 1024);
    pthread_t th;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pthread_create(&th, &at, worker, NULL);
    pthread_join(th, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("%s: %s, %d/%d frames, %d blits, persist=%d, %.1f s\n", argv[1], ok ? "returned" : "TRAPPED",
           frame, n, blits, persist, (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9);
    wasm_runtime_deinstantiate(inst);
    wasm_runtime_unload(mod);
    wasm_runtime_destroy();
    return ok && frame >= n - 25 ? 0 : 1;
}
