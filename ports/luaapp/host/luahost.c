// luahost.c — runs the REAL apps/luaapp/app.wasm (or an x86_64 AOT of it) on the PC under WAMR with
// the firmware's feature set, the "nv" imports stubbed: an in-memory canvas, a simulated 60 Hz clock,
// scripted touches/keys/back, HTTP answered from files, MQTT messages from a file.
//
//   luahost <module> <app_id> <fsdir> <frames> <out.ppm> [script] [W H]
//     fsdir/       the package's private folder ("/"), fsdir/engine/ = "/engine"
//     fsdir/net/   HTTP answers: the URL with every non-alphanumeric char replaced by '_'
//     fsdir/mqtt.txt  "topic|payload" lines delivered after the first subscribe
//   script: comma-separated "F:t=X/Y" (tap: down at F, up at F+3), "F-G:t=X/Y" (held), "F:back",
//           "F:key=USAGE" (HID usage held 3 frames), "F:shot" (also writes <out>.<F>.ppm),
//           "F:args=..." is not a thing: the bundle hash comes from LUAHOST_ARGS (space separated)
// Built by: bash ports/luaapp/build.sh test
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "wasm_export.h"

static int CW = 1024, CH = 600;
static uint16_t *canvas;
static int frame, frames_max;
static const char *out_ppm, *fsdir;
static char script[4096];
static int back_pending;
static int pending_shots[64], nshots;

static void write_ppm(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", CW, CH);
    for (int i = 0; i < CW * CH; i++) {
        uint16_t p = canvas[i];
        uint8_t rgb[3] = { (uint8_t)((p >> 11) << 3), (uint8_t)(((p >> 5) & 63) << 2), (uint8_t)((p & 31) << 3) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

// ---- script ---------------------------------------------------------------------------------------
static int touch_x = -1, touch_y = -1, key_usage;
static void script_step(void) {
    touch_x = touch_y = -1;
    key_usage = 0;
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", script);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        int a = 0, b = -1;
        char *colon = strchr(tok, ':');
        if (!colon) continue;
        if (sscanf(tok, "%d-%d", &a, &b) < 2) b = -1;
        const char *what = colon + 1;
        if (!strncmp(what, "t=", 2)) {
            int x, y;
            if (sscanf(what + 2, "%d/%d", &x, &y) != 2) continue;
            int end = b >= 0 ? b : a + 3;
            if (frame >= a && frame < end) { touch_x = x; touch_y = y; }
        } else if (!strcmp(what, "back")) {
            if (frame == a) back_pending++;
        } else if (!strncmp(what, "key=", 4)) {
            if (frame >= a && frame < a + 3) key_usage = atoi(what + 4);
        } else if (!strcmp(what, "shot")) {
            if (frame == a) {
                char p[512];
                snprintf(p, sizeof p, "%s.%d.ppm", out_ppm, a);
                write_ppm(p);
            }
        }
    }
}

// ---- nv imports ---------------------------------------------------------------------------------
#define NV_THROW_MARK "nv: throw"
static int32_t w_try_call(wasm_exec_env_t env, int32_t fn, int32_t ud) {
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(env);
    uint32_t argv[1] = { (uint32_t)ud };
    const bool ok = wasm_runtime_call_indirect(env, (uint32_t)fn, 1, argv);
    const char *ex = wasm_runtime_get_exception(inst);
    if (ok) return 0;
    if (ex && strstr(ex, NV_THROW_MARK)) wasm_runtime_clear_exception(inst);
    return 1;
}
static void w_throw(wasm_exec_env_t env) { wasm_runtime_set_exception(wasm_runtime_get_module_inst(env), NV_THROW_MARK); }
static int32_t w_width(wasm_exec_env_t e) { (void)e; return CW; }
static int32_t w_height(wasm_exec_env_t e) { (void)e; return CH; }
static void w_persist(wasm_exec_env_t e, int32_t on) { (void)e; (void)on; }
static void w_blit(wasm_exec_env_t e, const void *p, uint32_t len, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)e;
    if ((int64_t)w * h * 2 > len) return;
    const uint16_t *s = p;
    for (int r = 0; r < h; r++) {
        if (y + r < 0 || y + r >= CH) continue;
        for (int c = 0; c < w; c++) if (x + c >= 0 && x + c < CW) canvas[(y + r) * CW + x + c] = s[r * w + c];
    }
}
static int32_t w_present(wasm_exec_env_t e) {
    (void)e;
    frame++;
    script_step();
    return frame < frames_max;
}
static int32_t w_touch_count(wasm_exec_env_t e) { (void)e; return touch_x >= 0; }
static int32_t w_touch_point(wasm_exec_env_t e, int32_t i) {
    (void)e;
    if (i != 0 || touch_x < 0) return 0;
    return (1 << 24) | (touch_y << 12) | touch_x;
}
static int32_t w_input(wasm_exec_env_t e) { (void)e; return touch_x >= 0 ? (1 << 24) | (touch_y << 12) | touch_x : 0; }
static int32_t w_back(wasm_exec_env_t e) { (void)e; int n = back_pending; back_pending = 0; return n; }
static int32_t w_pad(wasm_exec_env_t e) { (void)e; return 0; }
static int32_t w_pad_count(wasm_exec_env_t e) { (void)e; return 0; }
static int32_t w_pad_state(wasm_exec_env_t e, int32_t i, void *st, uint32_t n) { (void)e; (void)i; (void)st; (void)n; return 0; }
static int32_t w_kbd(wasm_exec_env_t e, uint8_t *b, uint32_t n) {
    (void)e;
    if (n < 2) return -1;
    memset(b, 0, n);
    if (!key_usage) return 0;
    b[1] = (uint8_t)key_usage;
    return 1;
}
static int32_t w_mouse(wasm_exec_env_t e, void *m, uint32_t n) { (void)e; (void)m; (void)n; return 0; }
static int32_t w_lang(wasm_exec_env_t e, char *b, uint32_t n) { (void)e; const char *l = getenv("LUAHOST_LANG"); snprintf(b, n, "%s", l ? l : "en"); return (int32_t)strlen(b); }
static int32_t w_millis(wasm_exec_env_t e) { (void)e; return frame * 16; }
static int64_t w_time(wasm_exec_env_t e) { (void)e; return 1790000000LL + frame / 60; }
static int32_t w_rand(wasm_exec_env_t e) { (void)e; return rand(); }
static void w_toast(wasm_exec_env_t e, int32_t k, const char *m) { (void)e; fprintf(stderr, "[toast %d] %s\n", k, m); }
static void w_log(wasm_exec_env_t e, int32_t l, const char *m) { (void)e; fprintf(stderr, "[log%d] %s\n", l, m); }
static void w_print(wasm_exec_env_t e, const char *m) { (void)e; fprintf(stderr, "[print] %s\n", m); }
static void w_tone(wasm_exec_env_t e, int32_t f, int32_t ms) { (void)e; (void)f; (void)ms; }
static void w_sound(wasm_exec_env_t e, const char *n) { (void)e; fprintf(stderr, "[sound] %s\n", n); }
static void w_speak(wasm_exec_env_t e, const char *t, const char *l) { (void)e; fprintf(stderr, "[speak %s] %s\n", l, t); }
static void w_backlight(wasm_exec_env_t e, int32_t l) { (void)e; (void)l; }

// HTTP from files
typedef struct { int used, polls; char *body; size_t n, pos; int status, err; } hh_t;
static hh_t hh[4];
static int open_file_handle(const char *key) {
    int id = -1;
    for (int i = 0; i < 4; i++) if (!hh[i].used) { id = i; break; }
    if (id < 0) return -3;
    char name[400], path[1024];
    size_t k = 0;
    for (const char *p = key; *p && k < sizeof name - 1; p++) name[k++] = isalnum((unsigned char)*p) ? *p : '_';
    name[k] = 0;
    if (k > 200) memmove(name, name + k - 200, 201);
    snprintf(path, sizeof path, "%s/net/%s", fsdir, name);
    hh_t *h = &hh[id];
    memset(h, 0, sizeof *h);
    h->used = 1;
    FILE *f = fopen(path, "rb");
    fprintf(stderr, "[http] %s -> %s\n", key, f ? "file" : "MISSING");
    if (!f) { h->err = -5; return id; }
    fseek(f, 0, SEEK_END);
    h->n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    h->body = malloc(h->n + 1);
    if (fread(h->body, 1, h->n, f) != h->n) h->n = 0;
    fclose(f);
    h->status = 200;
    return id;
}
static int32_t w_http_req(wasm_exec_env_t e, const char *spec, const void *body, uint32_t len) {
    (void)e; (void)body; (void)len;
    const char *u = strstr(spec, "\"url\":\"");
    if (!u) return -2;
    u += 7;
    char url[600];
    size_t k = 0;
    while (*u && *u != '"' && k < sizeof url - 1) { if (*u == '\\' && u[1]) u++; url[k++] = *u++; }
    url[k] = 0;
    return open_file_handle(url);
}
static int32_t w_http_state(wasm_exec_env_t e, int32_t h) {
    (void)e;
    if (h < 0 || h > 3 || !hh[h].used) return -2;
    if (hh[h].polls++ < 3) return 0;
    return hh[h].err ? hh[h].err : 1;
}
static int32_t w_http_status(wasm_exec_env_t e, int32_t h) { (void)e; return (h >= 0 && h < 4) ? hh[h].status : 0; }
static int32_t w_http_read(wasm_exec_env_t e, int32_t h, void *buf, uint32_t len) {
    (void)e;
    if (h < 0 || h > 3 || !hh[h].used) return -2;
    size_t n = hh[h].n - hh[h].pos;
    if (n > len) n = len;
    memcpy(buf, hh[h].body + hh[h].pos, n);
    hh[h].pos += n;
    return (int32_t)n;
}
static void w_http_close(wasm_exec_env_t e, int32_t h) { (void)e; if (h >= 0 && h < 4) { free(hh[h].body); memset(&hh[h], 0, sizeof hh[h]); } }
static int32_t w_ws_open(wasm_exec_env_t e, const char *u, const char *hd) { (void)e; (void)hd; fprintf(stderr, "[ws] %s\n", u); return -5; }
static int32_t w_ws_state(wasm_exec_env_t e, int32_t h) { (void)e; (void)h; return -8; }
static int32_t w_ws_send(wasm_exec_env_t e, int32_t h, const void *b, uint32_t n, int32_t bin) { (void)e; (void)h; (void)b; (void)n; (void)bin; return -8; }
static int32_t w_ws_recv(wasm_exec_env_t e, int32_t h, void *b, uint32_t n) { (void)e; (void)h; (void)b; (void)n; return 0; }
static void w_ws_close(wasm_exec_env_t e, int32_t h) { (void)e; (void)h; }
static FILE *mq;
static int32_t w_mqtt_sub(wasm_exec_env_t e, const char *f) {
    (void)e;
    fprintf(stderr, "[mqtt sub] %s\n", f);
    if (!mq) { char p[1024]; snprintf(p, sizeof p, "%s/mqtt.txt", fsdir); mq = fopen(p, "r"); }
    return 0;
}
static int32_t w_mqtt_pub(wasm_exec_env_t e, const char *t, const void *b, uint32_t n, int32_t r) {
    (void)e; (void)r;
    fprintf(stderr, "[mqtt pub] %s = %.*s\n", t, (int)n, (const char *)b);
    return 0;
}
static int32_t w_mqtt_recv(wasm_exec_env_t e, char *topic, uint32_t tcap, void *buf, uint32_t cap) {
    (void)e;
    char line[2048];
    if (!mq || frame % 30 != 0 || !fgets(line, sizeof line, mq)) return -1;
    line[strcspn(line, "\r\n")] = 0;
    char *bar = strchr(line, '|');
    if (!bar) return -1;
    *bar = 0;
    snprintf(topic, tcap, "%s", line);
    size_t n = strlen(bar + 1);
    if (n > cap) n = cap;
    memcpy(buf, bar + 1, n);
    return (int32_t)n;
}
static int32_t w_ha_available(wasm_exec_env_t e) { (void)e; return 1; }
static int32_t w_ha_req(wasm_exec_env_t e, const char *m, const char *p, const void *b, uint32_t n) {
    (void)e; (void)b; (void)n;
    char key[400];
    snprintf(key, sizeof key, "ha %s %s", m, p);
    return open_file_handle(key);
}
static int32_t w_ha_ws(wasm_exec_env_t e) { (void)e; return -9; }
static int32_t w_mdns(wasm_exec_env_t e, const char *s, const char *p) {
    (void)e;
    char key[200];
    snprintf(key, sizeof key, "mdns %s %s", s, p);
    return open_file_handle(key);
}

static NativeSymbol s_nv[] = {
    {"try_call", (void *)w_try_call, "(ii)i", NULL}, {"throw", (void *)w_throw, "()", NULL},
    {"gfx_width", (void *)w_width, "()i", NULL}, {"gfx_height", (void *)w_height, "()i", NULL},
    {"gfx_persist", (void *)w_persist, "(i)", NULL}, {"gfx_blit", (void *)w_blit, "(*~iiii)", NULL},
    {"gfx_present", (void *)w_present, "()i", NULL}, {"gfx_touch_count", (void *)w_touch_count, "()i", NULL},
    {"gfx_touch_point", (void *)w_touch_point, "(i)i", NULL}, {"gfx_input", (void *)w_input, "()i", NULL},
    {"gfx_back", (void *)w_back, "()i", NULL}, {"gfx_pad", (void *)w_pad, "()i", NULL},
    {"gfx_tone", (void *)w_tone, "(ii)", NULL}, {"pad_count", (void *)w_pad_count, "()i", NULL},
    {"pad_state", (void *)w_pad_state, "(i*~)i", NULL}, {"kbd_state", (void *)w_kbd, "(*~)i", NULL},
    {"mouse_read", (void *)w_mouse, "(*~)i", NULL}, {"lang", (void *)w_lang, "(*~)i", NULL},
    {"millis", (void *)w_millis, "()i", NULL}, {"time_unix", (void *)w_time, "()I", NULL},
    {"rand", (void *)w_rand, "()i", NULL}, {"toast", (void *)w_toast, "(i$)", NULL},
    {"log", (void *)w_log, "(i$)", NULL}, {"print", (void *)w_print, "($)", NULL},
    {"sound", (void *)w_sound, "($)", NULL}, {"speak", (void *)w_speak, "($$)", NULL},
    {"backlight", (void *)w_backlight, "(i)", NULL},
    {"http_req", (void *)w_http_req, "($*~)i", NULL}, {"http_state", (void *)w_http_state, "(i)i", NULL},
    {"http_status", (void *)w_http_status, "(i)i", NULL}, {"http_read", (void *)w_http_read, "(i*~)i", NULL},
    {"http_close", (void *)w_http_close, "(i)", NULL}, {"ws_open", (void *)w_ws_open, "($$)i", NULL},
    {"ws_state", (void *)w_ws_state, "(i)i", NULL}, {"ws_send", (void *)w_ws_send, "(i*~i)i", NULL},
    {"ws_recv", (void *)w_ws_recv, "(i*~)i", NULL}, {"ws_close", (void *)w_ws_close, "(i)", NULL},
    {"mqtt_sub", (void *)w_mqtt_sub, "($)i", NULL}, {"mqtt_pub", (void *)w_mqtt_pub, "($*~i)i", NULL},
    {"mqtt_recv", (void *)w_mqtt_recv, "(*~*~)i", NULL}, {"ha_available", (void *)w_ha_available, "()i", NULL},
    {"ha_req", (void *)w_ha_req, "($$*~)i", NULL}, {"ha_ws", (void *)w_ha_ws, "()i", NULL},
    {"mdns_browse", (void *)w_mdns, "($$)i", NULL},
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
    if (argc < 6) { fprintf(stderr, "usage: luahost <module> <app_id> <fsdir> <frames> <out.ppm> [script] [W H]\n"); return 2; }
    fsdir = argv[3];
    frames_max = atoi(argv[4]);
    out_ppm = argv[5];
    if (argc > 6) snprintf(script, sizeof script, "%s", argv[6]);
    if (argc > 8) { CW = atoi(argv[7]); CH = atoi(argv[8]); }
    canvas = calloc((size_t)CW * CH, 2);
    srand(1);
    if (!wasm_runtime_init()) return 1;
    wasm_runtime_register_natives("nv", s_nv, sizeof s_nv / sizeof s_nv[0]);
    uint32_t size;
    unsigned char *buf = read_file(argv[1], &size);
    if (!buf) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    char err[128];
    wasm_module_t mod = wasm_runtime_load(buf, size, err, sizeof err);
    if (!mod) { fprintf(stderr, "load: %s\n", err); return 1; }
    static char m0[1100], m1[1100], envapp[80], aargs[600];
    snprintf(m0, sizeof m0, "/::%s", fsdir);
    snprintf(m1, sizeof m1, "/engine::%s/engine", fsdir);
    const char *maps[2] = { m0, m1 };
    snprintf(envapp, sizeof envapp, "NUCLEO_APP=%s", argv[2]);
    const char *env[] = { envapp, "HOME=/" };
    char *wargv[8] = { argv[2] };
    int wargc = 1;
    const char *a = getenv("LUAHOST_ARGS");
    if (a) {
        snprintf(aargs, sizeof aargs, "%s", a);
        for (char *t = strtok(aargs, " "); t && wargc < 7; t = strtok(NULL, " ")) wargv[wargc++] = t;
    }
    wasm_runtime_set_wasi_args_ex(mod, NULL, 0, maps, 2, env, 2, wargv, wargc, 0, 1, 2);
    InstantiationArgs ia;
    memset(&ia, 0, sizeof ia);
    ia.default_stack_size = 128 * 1024;
    ia.max_memory_pages = 8 * 16;
    wasm_module_inst_t inst = wasm_runtime_instantiate_ex(mod, &ia, err, sizeof err);
    if (!inst) { fprintf(stderr, "instantiate: %s\n", err); return 1; }
    wasm_exec_env_t ex = wasm_runtime_create_exec_env(inst, 128 * 1024);
    wasm_function_inst_t f = wasm_runtime_lookup_function(inst, "run");
    uint32_t av[2] = {0};
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    bool ok = f && wasm_runtime_call_wasm(ex, f, 0, av);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (!ok) { const char *e = wasm_runtime_get_exception(inst); fprintf(stderr, "run trapped: %s\n", e ? e : "?"); }
    double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    printf("frames %d  %.1f ms  (%.2f ms/frame)\n", frame, ms, frame ? ms / frame : 0);
    write_ppm(out_ppm);
    return ok ? 0 : 1;
}
