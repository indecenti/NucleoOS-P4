/* wamr_host.c — runs the real apps/openhasp/app.wasm (or an x86_64 AOT of it) in WAMR the way the
 * device does: WASI with the app's data folder as "/", no host-managed heap, linear memory capped
 * at the manifest ram_budget (8 MB), the "nv" imports the app uses, and run() called on a pthread
 * with the device worker's 48 KB native stack. The imports and the script engine are nv_sim.c's
 * (same scripts as the native harness).
 *
 *   openhasp_wamr <module> <data dir> <script> <out dir>
 */
#define NV_SIM_NO_MAIN
#include "nv_sim.c"

#include <pthread.h>
#include <time.h>
#include "wasm_export.h"

#define RAM_MB 8
#define WASM_STACK_KB 64   // manifest stack_kb
#define NATIVE_STACK_KB 48 // kWorkerNativeStk in components/nv_wasm

static int32_t w_millis(wasm_exec_env_t e) { (void)e; return nv_millis(); }
static void w_sleep(wasm_exec_env_t e, int32_t ms) { (void)e; nv_sleep_ms(ms); }
static int32_t w_rand(wasm_exec_env_t e) { (void)e; return nv_rand(); }
static int32_t w_lang(wasm_exec_env_t e, char* b, uint32_t n) { (void)e; return nv_lang(b, n); }
static void w_backlight(wasm_exec_env_t e, int32_t l) { (void)e; nv_backlight(l); }
static void w_persist(wasm_exec_env_t e, int32_t on) { (void)e; nv_gfx_persist(on); }
static int32_t w_back(wasm_exec_env_t e) { (void)e; return nv_gfx_back(); }
static int32_t w_input(wasm_exec_env_t e) { (void)e; return nv_gfx_input_raw(); }
static int32_t w_load(wasm_exec_env_t e, const char* n, void* d, uint32_t l) { (void)e; return nv_load(n, d, (int32_t)l); }
static int32_t w_save(wasm_exec_env_t e, const char* n, const void* d, uint32_t l) { (void)e; return nv_save(n, d, (int32_t)l); }
static void w_blit(wasm_exec_env_t e, const void* px, uint32_t len, int32_t x, int32_t y, int32_t w, int32_t h)
{
    (void)e;
    nv_gfx_blit_raw(px, (int32_t)len, x, y, w, h);
}
static int32_t w_sub(wasm_exec_env_t e, const char* f) { (void)e; return nv_mqtt_sub(f); }
static int32_t w_pub(wasm_exec_env_t e, const char* t, const void* p, uint32_t l, int32_t r)
{
    (void)e;
    return nv_mqtt_pub(t, p, l, r);
}
static int32_t w_recv(wasm_exec_env_t e, char* t, uint32_t tc, void* p, uint32_t pc)
{
    (void)e;
    return nv_mqtt_recv(t, tc, p, pc);
}
/* In the native build run() calls nv_sim_step() itself; here the wasm run() only presents. */
static int32_t w_present(wasm_exec_env_t e)
{
    (void)e;
    if(!nv_gfx_present()) return 0;
    nv_sim_step();
    return g_running;
}

static NativeSymbol natives[] = {
    {"millis", (void*)w_millis, "()i", NULL},        {"sleep_ms", (void*)w_sleep, "(i)", NULL},
    {"rand", (void*)w_rand, "()i", NULL},            {"lang", (void*)w_lang, "(*~)i", NULL},
    {"backlight", (void*)w_backlight, "(i)", NULL},  {"gfx_persist", (void*)w_persist, "(i)", NULL},
    {"gfx_back", (void*)w_back, "()i", NULL},        {"gfx_input", (void*)w_input, "()i", NULL},
    {"load", (void*)w_load, "($*~)i", NULL},         {"save", (void*)w_save, "($*~)i", NULL},
    {"gfx_blit", (void*)w_blit, "(*~iiii)", NULL},   {"mqtt_sub", (void*)w_sub, "($)i", NULL},
    {"mqtt_pub", (void*)w_pub, "($*~i)i", NULL},     {"mqtt_recv", (void*)w_recv, "(*~*~)i", NULL},
    {"gfx_present", (void*)w_present, "()i", NULL},
};

static wasm_module_inst_t inst;
static int ok;

static void* worker(void* arg)
{
    (void)arg;
    wasm_runtime_init_thread_env();
    wasm_exec_env_t env    = wasm_runtime_create_exec_env(inst, WASM_STACK_KB * 1024);
    wasm_function_inst_t f = wasm_runtime_lookup_function(inst, "run");
    if(!env || !f) {
        fprintf(stderr, "no exec env / no run export\n");
        return NULL;
    }
    ok = wasm_runtime_call_wasm(env, f, 0, NULL);
    if(!ok) printf("TRAP: %s\n", wasm_runtime_get_exception(inst));
    wasm_runtime_destroy_exec_env(env);
    wasm_runtime_destroy_thread_env();
    return NULL;
}

int main(int argc, char** argv)
{
    if(argc < 5) {
        fprintf(stderr, "usage: %s <module> <data dir> <script> <out dir>\n", argv[0]);
        return 2;
    }
    g_script = fopen(argv[3], "r");
    g_out    = realpath(argv[4], NULL);
    if(!g_script || !g_out) return 2;
    char log[512];
    snprintf(log, sizeof log, "%s/mqtt.log", g_out);
    g_mqttlog = fopen(log, "w");
    if(chdir(argv[2])) return 2; // nv_save/nv_load files, like the device's app folder
    srand(1234);

    if(!wasm_runtime_init()) return 1;
    wasm_runtime_register_natives("nv", natives, sizeof natives / sizeof natives[0]);
    FILE* f = fopen(argv[1], "rb");
    if(!f) {
        perror(argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* buf = malloc(sz);
    if(fread(buf, 1, sz, f) != (size_t)sz) return 1;
    fclose(f);
    char err[128], map[600];
    wasm_module_t mod = wasm_runtime_load(buf, (uint32_t)sz, err, sizeof err);
    if(!mod) {
        fprintf(stderr, "load: %s\n", err);
        return 1;
    }
    snprintf(map, sizeof map, "/::%s", argv[2]);
    const char* maps[1] = {map};
    wasm_runtime_set_wasi_args_ex(mod, NULL, 0, maps, 1, NULL, 0, NULL, 0, 0, 1, 2);
    InstantiationArgs ia;
    memset(&ia, 0, sizeof ia);
    ia.default_stack_size     = WASM_STACK_KB * 1024;
    ia.host_managed_heap_size = 0;
    ia.max_memory_pages       = RAM_MB * 16;
    inst = wasm_runtime_instantiate_ex(mod, &ia, err, sizeof err);
    if(!inst) {
        fprintf(stderr, "instantiate: %s\n", err);
        return 1;
    }
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, NATIVE_STACK_KB * 1024);
    pthread_t th;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pthread_create(&th, &at, worker, NULL);
    pthread_join(th, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    fprintf(g_mqttlog, "EXIT\n");
    fclose(g_mqttlog);
    printf("%s: %s, sim t=%u ms, %d blits (%d px), %.1f s wall, linear memory %u KB\n", argv[1],
           ok ? "returned" : "TRAPPED", g_ms, g_blits, g_blit_px, (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9,
           (unsigned)(wasm_memory_get_cur_page_count(wasm_runtime_get_default_memory(inst)) * 64));
    wasm_runtime_deinstantiate(inst);
    wasm_runtime_unload(mod);
    wasm_runtime_destroy();
    return ok ? 0 : 1;
}
