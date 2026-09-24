// nvhost.c — minimal PC host for testing NucleoOS WASI ports in WSL: WAMR + WASI + the "nv"
// imports the device provides to console programs (try_call/throw). Mirrors the firmware's
// instantiation (no host-managed heap, stack from the manifest), so what runs here runs there.
//
//   nvhost [--dir=host::guest]... [--stack=KB] [--mem=MB] module.wasm|module.aot [args...]
//
// Build: bash ports/host/build.sh (inside WSL; links the libiwasm.a from WAMR_LIB).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wasm_export.h"

#define NV_THROW_MARK "nv: throw"

static int32_t nvi_try_call(wasm_exec_env_t env, int32_t fn, int32_t ud) {
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(env);
    uint32_t argv[1] = { (uint32_t)ud };
    if (getenv("NVHOST_DEBUG")) fprintf(stderr, "[nvhost] try_call enter fn=%d\n", fn);
    const bool ok = wasm_runtime_call_indirect(env, (uint32_t)fn, 1, argv);
    const char *ex = wasm_runtime_get_exception(inst);
    if (getenv("NVHOST_DEBUG")) fprintf(stderr, "[nvhost] try_call fn=%d ok=%d ex=%s\n", fn, ok, ex ? ex : "-");
    if (ok) return 0;
    if (ex && strstr(ex, NV_THROW_MARK)) wasm_runtime_clear_exception(inst);
    return 1;   // a foreign trap stays set and keeps unwinding once we return
}

static void nvi_throw(wasm_exec_env_t env) {
    if (getenv("NVHOST_DEBUG")) fprintf(stderr, "[nvhost] throw\n");
    wasm_runtime_set_exception(wasm_runtime_get_module_inst(env), NV_THROW_MARK);
}

static NativeSymbol s_nv[] = {
    { "try_call", (void *)nvi_try_call, "(ii)i", NULL },
    { "throw",    (void *)nvi_throw,    "()",    NULL },
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
    const char *maps[8];
    int nmap = 0, stack_kb = 64, mem_mb = 8, i = 1;
    for (; i < argc && !strncmp(argv[i], "--", 2); i++) {
        if (!strncmp(argv[i], "--dir=", 6) && nmap < 8) {
            // host::guest (iwasm takes guest::host; keep the device's "guest::host" order)
            maps[nmap++] = argv[i] + 6;
        } else if (!strncmp(argv[i], "--stack=", 8)) {
            stack_kb = atoi(argv[i] + 8);
        } else if (!strncmp(argv[i], "--mem=", 6)) {
            mem_mb = atoi(argv[i] + 6);
        }
    }
    if (i >= argc) {
        fprintf(stderr, "usage: nvhost [--dir=guest::host] [--stack=KB] [--mem=MB] module [args]\n");
        return 2;
    }
    if (!wasm_runtime_init()) { fprintf(stderr, "runtime init failed\n"); return 1; }
    wasm_runtime_register_natives("nv", s_nv, 2);

    uint32_t size;
    unsigned char *buf = read_file(argv[i], &size);
    if (!buf) { fprintf(stderr, "cannot read %s\n", argv[i]); return 1; }
    char err[128];
    wasm_module_t mod = wasm_runtime_load(buf, size, err, sizeof err);
    if (!mod) { fprintf(stderr, "load: %s\n", err); return 1; }
    const char *env[] = { "TERM=dumb" };
    wasm_runtime_set_wasi_args_ex(mod, NULL, 0, maps, (uint32_t)nmap, env, 1, argv + i,
                                  argc - i, 0, 1, 2);
    InstantiationArgs ia;
    memset(&ia, 0, sizeof ia);
    ia.default_stack_size = (uint32_t)stack_kb * 1024;
    ia.host_managed_heap_size = 0;
    ia.max_memory_pages = (uint32_t)mem_mb * 16;
    wasm_module_inst_t inst = wasm_runtime_instantiate_ex(mod, &ia, err, sizeof err);
    if (!inst) { fprintf(stderr, "instantiate: %s\n", err); return 1; }
    int rc = 0;
    const bool ran = wasm_application_execute_main(inst, 0, NULL);
    if (getenv("NVHOST_DEBUG")) {
        const char *ex = wasm_runtime_get_exception(inst);
        fprintf(stderr, "[nvhost] main=%d exception=%s exit=%u\n", ran, ex ? ex : "-",
                wasm_runtime_get_wasi_exit_code(inst));
    }
    if (!ran) {
        const char *ex = wasm_runtime_get_exception(inst);
        if (ex && strstr(ex, "wasi proc exit")) {
            rc = (int)wasm_runtime_get_wasi_exit_code(inst);
        } else {
            fprintf(stderr, "trap: %s\n", ex ? ex : "?");
            rc = 1;
        }
    } else {
        rc = (int)wasm_runtime_get_wasi_exit_code(inst);
    }
    wasm_runtime_deinstantiate(inst);
    wasm_runtime_unload(mod);
    wasm_runtime_destroy();
    free(buf);
    return rc;
}
