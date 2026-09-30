// svmhost.c — PC test host for the ScummVM port (WSL): WAMR + WASI like ports/host/nvhost.c, plus
// the game-surface imports the NucleoOS backend uses (gfx canvas, touch, pad, audio stream), so the
// app runs headless on the PC with scripted input:
//
//   svmhost [--dir=host::guest]... [--mem=MB] [--canvas=WxH] [--script=FILE] [--out=DIR] module [args]
//
// Script lines (times in ms since start; '#' comments):
//   <ms> tap X Y          finger down at canvas X,Y for 80 ms
//   <ms> hold X Y MS      finger down for MS ms (long press)
//   <ms> tap2 X Y         two-finger tap
//   <ms> drag X0 Y0 X1 Y1 MS
//   <ms> pad BITS MS      nv_gfx_pad bits held for MS ms
//   <ms> back             OS back gesture
//   <ms> shot NAME        write OUT/NAME.ppm from the last presented frame
//   <ms> quit             nv_gfx_present returns 0 from now on
// Audio goes to OUT/audio.raw (s16le, the rate/channels printed at open); the backlog drains in
// real time. Every present is counted; OUT/last.ppm is refreshed once a second.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "wasm_export.h"

static int s_cw = 320, s_ch = 200;
static uint16_t *s_canvas, *s_shown;
static const char *s_out = ".";
static int64_t s_t0;
static int s_quit, s_back;
static FILE *s_audio;
static int s_rate, s_chans;
static int64_t s_audio_frames, s_audio_t0;
static long s_presents;

typedef struct { int t; char op[8]; int a, b, c, d, e; char name[64]; } Step;
static Step s_steps[512];
static int s_nsteps, s_next;
// current synthetic input
static int s_fingers, s_fx, s_fy, s_until, s_pad, s_pad_until;
static int s_drag, s_dx0, s_dy0, s_dx1, s_dy1, s_dt0, s_dms;
static int s_key, s_key_until, s_mdx, s_mdy, s_mbtn, s_has_kbd, s_has_mouse;

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000 - s_t0;
}

static void write_ppm(const char *name, const uint16_t *px) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s.ppm", s_out, name);
    FILE *f = fopen(p, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", s_cw, s_ch);
    for (int i = 0; i < s_cw * s_ch; i++) {
        const uint16_t v = px[i];
        unsigned char rgb[3] = { (unsigned char)((v >> 8) & 0xF8), (unsigned char)((v >> 3) & 0xFC),
                                 (unsigned char)((v << 3) & 0xF8) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    fprintf(stderr, "[svmhost] %6lld ms shot %s (%ld presents)\n", (long long)now_ms(), p, s_presents);
}

static void run_script(void) {
    const int t = (int)now_ms();
    while (s_next < s_nsteps && s_steps[s_next].t <= t) {
        Step *s = &s_steps[s_next++];
        if (!strcmp(s->op, "tap"))   { s_fingers = 1; s_fx = s->a; s_fy = s->b; s_until = t + 80; }
        if (!strcmp(s->op, "tap2"))  { s_fingers = 2; s_fx = s->a; s_fy = s->b; s_until = t + 80; }
        if (!strcmp(s->op, "hold"))  { s_fingers = 1; s_fx = s->a; s_fy = s->b; s_until = t + s->c; }
        if (!strcmp(s->op, "drag"))  { s_fingers = 1; s_drag = 1; s_dx0 = s->a; s_dy0 = s->b; s_dx1 = s->c;
                                       s_dy1 = s->d; s_dt0 = t; s_dms = s->e; s_until = t + s->e; }
        if (!strcmp(s->op, "pad"))   { s_pad = s->a; s_pad_until = t + s->b; }
        if (!strcmp(s->op, "back"))  s_back++;
        if (!strcmp(s->op, "key"))   { s_has_kbd = 1; s_key = s->a; s_key_until = t + s->b; }
        if (!strcmp(s->op, "mouse")) { s_has_mouse = 1; s_mdx += s->a; s_mdy += s->b; s_mbtn = s->c; }
        if (!strcmp(s->op, "shot"))  write_ppm(s->name, s_shown);
        if (!strcmp(s->op, "quit"))  s_quit = 1;
        fprintf(stderr, "[svmhost] %6d ms %s %d %d\n", t, s->op, s->a, s->b);
    }
    if (s_fingers && t >= s_until) { s_fingers = 0; s_drag = 0; }
    if (s_drag && s_dms > 0) {
        const int k = t - s_dt0 > s_dms ? s_dms : t - s_dt0;
        s_fx = s_dx0 + (s_dx1 - s_dx0) * k / s_dms;
        s_fy = s_dy0 + (s_dy1 - s_dy0) * k / s_dms;
    }
    if (s_pad && t >= s_pad_until) s_pad = 0;
}

static void load_script(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "no script %s\n", path); exit(2); }
    char line[256];
    while (fgets(line, sizeof line, f) && s_nsteps < 512) {
        if (line[0] == '#' || line[0] == '\n') continue;
        Step *s = &s_steps[s_nsteps];
        memset(s, 0, sizeof *s);
        const int n = sscanf(line, "%d %7s", &s->t, s->op);
        if (n < 2) continue;
        const char *rest = strstr(line, s->op) + strlen(s->op);
        if (!strcmp(s->op, "shot")) sscanf(rest, "%63s", s->name);
        else sscanf(rest, "%i %d %d %d %d", &s->a, &s->b, &s->c, &s->d, &s->e);
        s_nsteps++;
    }
    fclose(f);
}

// ---- nv imports -----------------------------------------------------------------------------------
static void nv_log(wasm_exec_env_t e, const char *m) { fprintf(stderr, "[nv.log] %s", m); }
static void nv_log2(wasm_exec_env_t e, int32_t lvl, const char *m) { fprintf(stderr, "[nv.log %d] %s", lvl, m); }
static int32_t nv_millis(wasm_exec_env_t e) { run_script(); return (int32_t)(now_ms() + 100000); }
static int32_t gfx_width(wasm_exec_env_t e) { return s_cw; }
static int32_t gfx_height(wasm_exec_env_t e) { return s_ch; }
static void gfx_blit(wasm_exec_env_t e, const void *px, int32_t len, int32_t x, int32_t y, int32_t w, int32_t h) {
    if (len < w * h * 2) return;
    for (int r = 0; r < h; r++) {
        if (y + r < 0 || y + r >= s_ch) continue;
        for (int c = 0; c < w; c++) {
            if (x + c < 0 || x + c >= s_cw) continue;
            s_canvas[(y + r) * s_cw + x + c] = ((const uint16_t *)px)[r * w + c];
        }
    }
}
static int32_t gfx_input(wasm_exec_env_t e) {
    run_script();
    return s_fingers ? (1 << 24) | (s_fy << 12) | s_fx : ((s_fy << 12) | s_fx);
}
static int32_t gfx_back(wasm_exec_env_t e) { run_script(); int v = s_back; s_back = 0; return v; }
static int32_t gfx_present(wasm_exec_env_t e) {
    static int64_t last_dump;
    run_script();
    memcpy(s_shown, s_canvas, s_cw * s_ch * 2);
    s_presents++;
    if (now_ms() - last_dump > 1000) { last_dump = now_ms(); write_ppm("last", s_shown); }
    usleep(16000);
    return !s_quit;
}
static int32_t gfx_touch_count(wasm_exec_env_t e) { run_script(); return s_fingers; }
static int32_t gfx_touch_point(wasm_exec_env_t e, int32_t i) {
    run_script();
    if (i >= s_fingers) return 0;
    return (1 << 24) | ((s_fy + i * 20) << 12) | (s_fx + i * 20);
}
static int32_t gfx_pad(wasm_exec_env_t e) { run_script(); return s_pad; }
static int32_t pad_count(wasm_exec_env_t e) { return 0; }
static int32_t pad_state(wasm_exec_env_t e, void *st, int32_t len) { return 0; }
static int32_t audio_open(wasm_exec_env_t e, int32_t rate, int32_t ch) {
    char p[512];
    snprintf(p, sizeof p, "%s/audio.raw", s_out);
    s_audio = fopen(p, "wb");
    s_rate = rate; s_chans = ch; s_audio_t0 = now_ms();
    fprintf(stderr, "[svmhost] audio_open %d Hz x%d -> %s\n", rate, ch, p);
    return s_audio != NULL;
}
static int32_t audio_backlog(wasm_exec_env_t e) {
    if (!s_audio) return -1;
    const int64_t played = (now_ms() - s_audio_t0) * s_rate / 1000;
    if (played > s_audio_frames) s_audio_frames = played;   // underrun: silence played
    return (int32_t)((s_audio_frames - played) * 2 * s_chans);
}
static int32_t audio_write(wasm_exec_env_t e, const void *pcm, int32_t n) {
    if (!s_audio) return -1;
    audio_backlog(e);
    fwrite(pcm, 1, n, s_audio);
    s_audio_frames += n / (2 * s_chans);
    return n;
}
static void audio_close(wasm_exec_env_t e) { if (s_audio) fclose(s_audio); s_audio = NULL; }


// ---- ABI v12 http (synchronous, through curl) + lang ------------------------------------------------
typedef struct { unsigned char *buf; int len, status, used; } Http;
static Http s_http[4];
static const char *json_str(const char *j, const char *key, char *out, int cap) {
    const char *p = strstr(j, key);
    if (!p) return NULL;
    p = strchr(p + strlen(key), ':');
    if (!p) return NULL;
    p = strchr(p, '"');
    if (!p) return NULL;
    int n = 0;
    for (p++; *p && *p != '"' && n < cap - 1; p++) out[n++] = *p;
    out[n] = 0;
    return out;
}
static int32_t http_req(wasm_exec_env_t e, const char *spec, const void *body, uint32_t len) {
    int h = -1;
    for (int i = 0; i < 4; i++) if (!s_http[i].used) { h = i; break; }
    if (h < 0) return -3;
    char url[512], range[64] = "", cmd[1024];
    if (!json_str(spec, "\"url\"", url, sizeof url)) return -2;
    json_str(spec, "\"Range\"", range, sizeof range);
    const char *r = strstr(range, "bytes=");
    snprintf(cmd, sizeof cmd, "curl -s -w '\\n%%{http_code}' %s%s '%s'", r ? "-r " : "", r ? r + 6 : "", url);
    FILE *f = popen(cmd, "r");
    Http *x = &s_http[h];
    x->used = 1; x->len = 0; x->status = 0;
    x->buf = malloc(2 << 20);
    int n;
    while ((n = fread(x->buf + x->len, 1, (2 << 20) - x->len, f)) > 0) x->len += n;
    pclose(f);
    // trailing "\n<code>" appended by -w
    int k = x->len - 1;
    while (k > 0 && x->buf[k] != '\n') k--;
    x->status = atoi((char *)x->buf + k + 1);
    x->len = k;
    x->status = x->status ? x->status : -5;
    fprintf(stderr, "[svmhost] http %s range %s -> %d, %d bytes\n", url, r ? r + 6 : "-", x->status, x->len);
    return h;
}
static int32_t http_state(wasm_exec_env_t e, int32_t h) { return (h >= 0 && h < 4 && s_http[h].used) ? (s_http[h].status > 0 ? 1 : -5) : -2; }
static int32_t http_status(wasm_exec_env_t e, int32_t h) { return (h >= 0 && h < 4) ? s_http[h].status : 0; }
static int32_t http_read(wasm_exec_env_t e, int32_t h, void *buf, uint32_t len) {
    if (h < 0 || h >= 4 || !s_http[h].used) return -2;
    Http *x = &s_http[h];
    int n = x->len < (int)len ? x->len : (int)len;
    memcpy(buf, x->buf, n);
    memmove(x->buf, x->buf + n, x->len - n);
    x->len -= n;
    return n;
}
static void http_close(wasm_exec_env_t e, int32_t h) { if (h >= 0 && h < 4) { free(s_http[h].buf); s_http[h].used = 0; } }
// Keyboard / mouse (ABI 14): script "key USAGE MS" holds one HID usage, "mouse DX DY BUTTONS" is one
// report (motion delivered once, buttons held until the next mouse line).
static int32_t kbd_state(wasm_exec_env_t e, uint8_t *buf, int32_t len) {
    run_script();
    if (!s_has_kbd) return -1;
    memset(buf, 0, len);
    if (s_key && now_ms() < s_key_until && len > 1) { buf[1] = (uint8_t)s_key; return 1; }
    return 0;
}
static int32_t mouse_read(wasm_exec_env_t e, int32_t *m, int32_t len) {
    run_script();
    if (!s_has_mouse || len < 16) return 0;
    m[0] = s_mdx; m[1] = s_mdy; m[2] = 0; m[3] = s_mbtn;
    s_mdx = s_mdy = 0;
    return 1;
}
static int32_t nv_lang_(wasm_exec_env_t e, char *buf, uint32_t len) {
    const char *l = getenv("SVM_LANG") ? getenv("SVM_LANG") : "it";
    snprintf(buf, len, "%s", l);
    return (int32_t)strlen(buf);
}

static NativeSymbol s_nv[] = {
    { "print",           (void *)nv_log,         "($)",       NULL },
    { "log",             (void *)nv_log2,        "(i$)",      NULL },
    { "millis",          (void *)nv_millis,      "()i",       NULL },
    { "gfx_width",       (void *)gfx_width,      "()i",       NULL },
    { "gfx_height",      (void *)gfx_height,     "()i",       NULL },
    { "gfx_blit",        (void *)gfx_blit,       "(*~iiii)",  NULL },
    { "gfx_input",       (void *)gfx_input,      "()i",       NULL },
    { "gfx_back",        (void *)gfx_back,       "()i",       NULL },
    { "gfx_present",     (void *)gfx_present,    "()i",       NULL },
    { "gfx_touch_count", (void *)gfx_touch_count, "()i",      NULL },
    { "gfx_touch_point", (void *)gfx_touch_point, "(i)i",     NULL },
    { "gfx_pad",         (void *)gfx_pad,        "()i",       NULL },
    { "pad_count",       (void *)pad_count,      "()i",       NULL },
    { "pad_state",       (void *)pad_state,      "(i*~)i",    NULL },
    { "audio_open",      (void *)audio_open,     "(ii)i",     NULL },
    { "audio_write",     (void *)audio_write,    "(*~)i",     NULL },
    { "audio_backlog",   (void *)audio_backlog,  "()i",       NULL },
    { "audio_close",     (void *)audio_close,    "()",        NULL },
    { "http_req",        (void *)http_req,       "($*~)i",    NULL },
    { "http_state",      (void *)http_state,     "(i)i",      NULL },
    { "http_status",     (void *)http_status,    "(i)i",      NULL },
    { "http_read",       (void *)http_read,      "(i*~)i",    NULL },
    { "http_close",      (void *)http_close,     "(i)",       NULL },
    { "lang",            (void *)nv_lang_,       "(*~)i",     NULL },
    { "kbd_state",       (void *)kbd_state,      "(*~)i",     NULL },
    { "mouse_read",      (void *)mouse_read,     "(*~)i",     NULL },
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
    int nmap = 0, mem_mb = 16, i = 1;
    s_t0 = 0; s_t0 = now_ms();
    for (; i < argc && !strncmp(argv[i], "--", 2); i++) {
        if (!strncmp(argv[i], "--dir=", 6) && nmap < 8) maps[nmap++] = argv[i] + 6;
        else if (!strncmp(argv[i], "--mem=", 6)) mem_mb = atoi(argv[i] + 6);
        else if (!strncmp(argv[i], "--canvas=", 9)) sscanf(argv[i] + 9, "%dx%d", &s_cw, &s_ch);
        else if (!strncmp(argv[i], "--script=", 9)) load_script(argv[i] + 9);
        else if (!strncmp(argv[i], "--out=", 6)) s_out = argv[i] + 6;
    }
    if (i >= argc) { fprintf(stderr, "usage: see svmhost.c\n"); return 2; }
    s_canvas = calloc(s_cw * s_ch, 2);
    s_shown = calloc(s_cw * s_ch, 2);
    if (!wasm_runtime_init()) return 1;
    wasm_runtime_register_natives("nv", s_nv, sizeof s_nv / sizeof s_nv[0]);
    uint32_t size;
    unsigned char *buf = read_file(argv[i], &size);
    if (!buf) { fprintf(stderr, "cannot read %s\n", argv[i]); return 1; }
    char err[128];
    wasm_module_t mod = wasm_runtime_load(buf, size, err, sizeof err);
    if (!mod) { fprintf(stderr, "load: %s\n", err); return 1; }
    const char *env[] = { "TERM=dumb" };
    wasm_runtime_set_wasi_args_ex(mod, NULL, 0, maps, (uint32_t)nmap, env, 1, argv + i, argc - i, 0, 1, 2);
    InstantiationArgs ia;
    memset(&ia, 0, sizeof ia);
    ia.default_stack_size = 256 * 1024;
    ia.max_memory_pages = (uint32_t)mem_mb * 16;
    wasm_module_inst_t inst = wasm_runtime_instantiate_ex(mod, &ia, err, sizeof err);
    if (!inst) { fprintf(stderr, "instantiate: %s\n", err); return 1; }
    const bool ran = wasm_application_execute_main(inst, 0, NULL);
    const char *ex = wasm_runtime_get_exception(inst);
    int rc = ran ? (int)wasm_runtime_get_wasi_exit_code(inst) : 1;
    if (!ran && ex && strstr(ex, "wasi proc exit")) rc = (int)wasm_runtime_get_wasi_exit_code(inst);
    else if (!ran) fprintf(stderr, "trap: %s\n", ex ? ex : "?");
    fprintf(stderr, "[svmhost] exit %d after %lld ms, %ld presents\n", rc, (long long)now_ms(), s_presents);
    write_ppm("last", s_shown);
    return rc;
}
