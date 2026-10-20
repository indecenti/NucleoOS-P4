// native_host.c — the nv_* imports of the Biblioteca app for a native build on the PC (ASan, UBSan):
// a 1024x600 canvas in memory driven by a script of touches, keys and screenshots (PPM).
//
//   bib_native SCRIPT OUTDIR     (environment: BIB_ROOT = the folder holding zim/ and appdata/,
//                                 BIB_LANG = it|en|..., BIB_W / BIB_H = canvas size)
// Script, one command per line (# comments):
//   wait N | tap X Y | hold X Y N | drag X0 Y0 X1 Y1 N | key USAGE [MODS] | type TEXT | back |
//   shot NAME | kbd 0|1 | open PATH | stats | net 0|1 (no network / back)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nucleo_sdk.h"

void run(void);

#ifdef BIB_MEMCAP
// The device's budget: the app's linear memory is capped by the manifest (16 MiB, the static data
// and the 256 KB stack included). Built without ASan, malloc and friends are counted here and fail
// past BIB_MEMCAP bytes, like memory.grow does on the board; the peak is printed at the end.
#include <stddef.h>
void *__real_malloc(size_t n);
void *__real_realloc(void *p, size_t n);
void __real_free(void *p);
static size_t mem_now, mem_peak, mem_cap = BIB_MEMCAP;
static long mem_fail;
void *__wrap_malloc(size_t n) {
    if (mem_now + n + 16 > mem_cap) { mem_fail++; return NULL; }
    size_t *p = (size_t *)__real_malloc(n + 16);
    if (!p) return NULL;
    p[0] = n;
    mem_now += n + 16;
    if (mem_now > mem_peak) mem_peak = mem_now;
    return (char *)p + 16;
}
void __wrap_free(void *q) {
    if (!q) return;
    size_t *p = (size_t *)((char *)q - 16);
    mem_now -= p[0] + 16;
    __real_free(p);
}
void *__wrap_calloc(size_t a, size_t b) {
    if (b && a > (size_t)-1 / b) return NULL;
    void *p = __wrap_malloc(a * b);
    if (p) memset(p, 0, a * b);
    return p;
}
void *__wrap_realloc(void *q, size_t n) {
    if (!q) return __wrap_malloc(n);
    size_t *p = (size_t *)((char *)q - 16);
    const size_t old = p[0];
    if (n > old && mem_now + (n - old) > mem_cap) { mem_fail++; return NULL; }
    size_t *r = (size_t *)__real_realloc(p, n + 16);
    if (!r) return NULL;
    mem_now = mem_now - old + n;
    r[0] = n;
    if (mem_now > mem_peak) mem_peak = mem_now;
    return (char *)r + 16;
}
#endif

#ifdef BIB_MEMCAP   // the harness's own buffers are not the app's
#define HOST_MALLOC __real_malloc
#define HOST_REALLOC __real_realloc
#define HOST_FREE __real_free
#else
#define HOST_MALLOC malloc
#define HOST_REALLOC realloc
#define HOST_FREE free
#endif

static int CW = 1024, CH = 600;
static uint16_t *canvas;
static const char *outdir = ".";

typedef struct {
    int down, x, y, back, kbd_usage, kbd_mods, stats, net;   // net: 1 = off, 2 = on
    char shot[64];
} frame_t;
static frame_t *fr;
static int nfr, cap, cur;
static int kbd_on = 1;
static char open_file[400];
static long blits, blit_rows;

static frame_t *push(void) {
    if (nfr == cap) {
        cap = cap ? cap * 2 : 1024;
        fr = (frame_t *)HOST_REALLOC(fr, sizeof *fr * cap);
    }
    memset(&fr[nfr], 0, sizeof *fr);
    return &fr[nfr++];
}

static int usage_of(char c, int *mods) {
    *mods = 0;
    if (c >= 'a' && c <= 'z') return 0x04 + c - 'a';
    if (c >= 'A' && c <= 'Z') { *mods = 0x02; return 0x04 + c - 'A'; }
    if (c >= '1' && c <= '9') return 0x1E + c - '1';
    if (c == '0') return 0x27;
    if (c == ' ') return 0x2C;
    if (c == '-') return 0x38;   // Italian layout: '-' is on the US '/' key
    if (c == '\'') return 0x2D;
    return 0;
}

static int load_script(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[512];
    for (int i = 0; i < 3; i++) push();
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *p = line;
        while (*p == ' ') p++;
        if (!*p || *p == '#') continue;
        int a, b, c, d, n;
        char s[400];
        if (sscanf(p, "wait %d", &n) == 1) { while (n-- > 0) push(); }
        else if (sscanf(p, "tap %d %d", &a, &b) == 2) {
            for (int i = 0; i < 2; i++) { frame_t *x = push(); x->down = 1; x->x = a; x->y = b; }
            push(); push();
        } else if (sscanf(p, "hold %d %d %d", &a, &b, &n) == 3) {
            while (n-- > 0) { frame_t *x = push(); x->down = 1; x->x = a; x->y = b; }
            push(); push();
        } else if (sscanf(p, "drag %d %d %d %d %d", &a, &b, &c, &d, &n) == 5) {
            for (int i = 0; i <= n; i++) {
                frame_t *x = push();
                x->down = 1;
                x->x = a + (c - a) * i / n;
                x->y = b + (d - b) * i / n;
            }
            push(); push();
        } else if (sscanf(p, "key %i %i", &a, &b) == 2 || sscanf(p, "key %i", &a) == 1) {
            if (sscanf(p, "key %i %i", &a, &b) != 2) b = 0;
            frame_t *x = push();
            x->kbd_usage = a;
            x->kbd_mods = b;
            push();
        } else if (!strncmp(p, "type ", 5)) {
            for (const char *t = p + 5; *t; t++) {
                int m, u = usage_of(*t, &m);
                if (!u) continue;
                frame_t *x = push();
                x->kbd_usage = u;
                x->kbd_mods = m;
                push();
            }
        } else if (!strcmp(p, "back")) { push()->back = 1; push(); push(); }
        else if (sscanf(p, "shot %63s", s) == 1) { snprintf(push()->shot, sizeof fr[0].shot, "%s", s); }
        else if (sscanf(p, "kbd %d", &n) == 1) kbd_on = n;
        else if (sscanf(p, "open %399s", s) == 1) snprintf(open_file, sizeof open_file, "%s", s);
        else if (!strcmp(p, "stats")) push()->stats = 1;
        else if (sscanf(p, "net %d", &n) == 1) push()->net = n ? 2 : 1;
        else { fprintf(stderr, "script: %s?\n", p); return -1; }
    }
    fclose(f);
    return 0;
}

static void write_ppm(const char *name) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s.ppm", outdir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", CW, CH);
    unsigned char *rgb = (unsigned char *)HOST_MALLOC((size_t)CW * CH * 3);
    for (int i = 0; i < CW * CH; i++) {
        const uint16_t px = canvas[i];
        rgb[3 * i] = (unsigned char)((px >> 11) << 3 | (px >> 13));
        rgb[3 * i + 1] = (unsigned char)(((px >> 5) & 63) << 2 | ((px >> 9) & 3));
        rgb[3 * i + 2] = (unsigned char)((px & 31) << 3 | ((px >> 2) & 7));
    }
    fwrite(rgb, 1, (size_t)CW * CH * 3, f);
    HOST_FREE(rgb);
    fclose(f);
}

void nv_log(int32_t level, const char *msg) { fprintf(stderr, "log %d: %s\n", level, msg); }
int32_t nv_gfx_width(void) { return CW; }
int32_t nv_gfx_height(void) { return CH; }
void nv_gfx_persist(int32_t on) { (void)on; }
int32_t nv_millis(void) { return cur * 16; }
int32_t nv_rand(void) { return rand(); }
int64_t nv_time_unix(void) { return 1760000000 + cur / 60; }
int32_t nv_lang(char *buf, uint32_t len) {
    const char *l = getenv("BIB_LANG");
    snprintf(buf, len, "%s", l ? l : "it");
    return 2;
}
void nv_gfx_blit_raw(const void *px, int32_t len, int32_t x, int32_t y, int32_t w, int32_t h) {
    if ((int64_t)w * h * 2 > len || x != 0 || w != CW || y < 0 || y + h > CH) {
        fprintf(stderr, "bad blit x=%d y=%d w=%d h=%d len=%d\n", x, y, w, h, len);
        abort();
    }
    memcpy(canvas + (size_t)y * CW, px, (size_t)w * h * 2);
    blits++;
    blit_rows += h;
}
int32_t nv_gfx_input_raw(void) {
    const frame_t *f = cur > 0 && cur - 1 < nfr ? &fr[cur - 1] : NULL;
    return f && f->down ? (1 << 24) | (f->y << 12) | f->x : 0;
}
static int pend_back;
int32_t nv_gfx_back(void) { int b = pend_back; pend_back = 0; return b; }
int32_t nv_kbd_state(uint8_t *buf, int32_t len) {
    if (!kbd_on) return -1;
    memset(buf, 0, (size_t)len);
    const frame_t *f = cur > 0 && cur - 1 < nfr ? &fr[cur - 1] : NULL;
    if (!f || !f->kbd_usage) return 0;
    buf[0] = (uint8_t)f->kbd_mods;
    buf[1] = (uint8_t)f->kbd_usage;
    return 1;
}
int32_t nv_open_path(char *buf, int32_t len) {
    if (!open_file[0]) return 0;
    snprintf(buf, (size_t)len, "%s", open_file);
    return (int32_t)strlen(open_file);
}

// ---- ABI 12 HTTP, for the download screen: the Kiwix catalog is test/opds_ita.xml, a thumbnail is
// test/icon48.png (BIB_TESTDATA = that folder), an answer comes 10 frames later. BIB_OFFLINE=1: no network.
static struct { int used, status, ready; char *body; size_t n, off; } s_http[4];
static int s_offline;   // the script's "net 0"
static void dl_tick(void);

static char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long k = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = (char *)HOST_MALLOC((size_t)k + 1);
    *n = fread(b, 1, (size_t)k, f);
    fclose(f);
    return b;
}

int32_t nv_http_req(const char *spec, const void *body, uint32_t len) {
    (void)body; (void)len;
    int h = 0;
    while (h < 4 && s_http[h].used) h++;
    if (h == 4) return -3;
    const char *u = strstr(spec, "\"url\":\"");
    if (!u) return -2;
    char url[400];
    snprintf(url, sizeof url, "%s", u + 7);
    url[strcspn(url, "\"")] = 0;
    s_http[h].used = 1;
    s_http[h].ready = cur + 10;
    s_http[h].off = s_http[h].n = 0;
    s_http[h].body = NULL;
    s_http[h].status = 404;
    const char *dir = getenv("BIB_TESTDATA");
    char p[600];
    if (getenv("BIB_OFFLINE") || s_offline) s_http[h].status = -5;
    else if (dir && strstr(url, "/catalog/v2/entries?lang=ita") && strstr(url, "start=0")) snprintf(p, sizeof p, "%s/opds_ita.xml", dir);
    else if (dir && strstr(url, "/catalog/v2/illustration/")) snprintf(p, sizeof p, "%s/icon48.png", dir);
    else p[0] = 0;
    if (s_http[h].status != -5 && p[0] && (s_http[h].body = slurp(p, &s_http[h].n))) s_http[h].status = 200;
    fprintf(stderr, "http: %s -> %d\n", url, s_http[h].status);
    return h;
}
int32_t nv_http_state(int32_t h) {
    if (h < 0 || h >= 4 || !s_http[h].used) return -2;
    if (cur < s_http[h].ready) return 0;
    return s_http[h].status < 0 ? s_http[h].status : 1;
}
int32_t nv_http_status(int32_t h) { return h >= 0 && h < 4 && s_http[h].status > 0 ? s_http[h].status : 0; }
int32_t nv_http_read(int32_t h, void *buf, uint32_t len) {
    if (h < 0 || h >= 4 || !s_http[h].used) return -2;
    size_t k = s_http[h].n - s_http[h].off;
    if (k > len) k = len;
    if (k) memcpy(buf, s_http[h].body + s_http[h].off, k);
    s_http[h].off += k;
    return (int32_t)k;
}
void nv_http_close(int32_t h) {
    if (h < 0 || h >= 4) return;
    HOST_FREE(s_http[h].body);
    memset(&s_http[h], 0, sizeof s_http[h]);
}

// ---- ABI 18 the system downloader: a download grows 512 KB a frame (shown as 180 KB/s) and, when
// complete, BIB_DL_FIXTURE (a small .zim) is copied to its destination under BIB_ROOT.
static struct { int id, state; char title[96], dest[160]; long long size, done; } s_dl[16];
static int s_ndl, s_dl_next = 1;
static const char *const kDlSt[] = {"queued", "paused", "done", "failed", "running"};

static void json_get(const char *spec, const char *key, char *out, size_t cap) {
    char k[40];
    snprintf(k, sizeof k, "\"%s\":\"", key);
    const char *v = strstr(spec, k);
    out[0] = 0;
    if (!v) return;
    v += strlen(k);
    size_t i = 0;
    for (; *v && *v != '"' && i + 1 < cap; v++) { if (*v == '\\' && v[1]) v++; out[i++] = *v; }
    out[i] = 0;
}

int32_t nv_dl_start(const char *spec) {
    if (s_ndl == 16) return -3;
    char dest[160], title[96], meta4[300];
    json_get(spec, "dest", dest, sizeof dest);
    json_get(spec, "title", title, sizeof title);
    json_get(spec, "meta4", meta4, sizeof meta4);
    if (dest[0] != '/' || !meta4[0]) return -2;
    for (int i = 0; i < s_ndl; i++) if (!strcmp(s_dl[i].dest, dest) && s_dl[i].state != 2) return -5;
    s_dl[s_ndl].id = s_dl_next++;
    s_dl[s_ndl].state = 4;
    snprintf(s_dl[s_ndl].title, sizeof s_dl[0].title, "%s", title);
    snprintf(s_dl[s_ndl].dest, sizeof s_dl[0].dest, "%s", dest);
    s_dl[s_ndl].size = 50ll << 20;
    s_dl[s_ndl].done = 0;
    fprintf(stderr, "dl: start %d %s -> %s\n", s_dl[s_ndl].id, meta4, dest);
    return s_dl[s_ndl++].id;
}

static void dl_tick(void) {
    for (int i = 0; i < s_ndl; i++) {
        if (s_dl[i].state != 4) continue;
        s_dl[i].done += 512 << 10;
        if (s_dl[i].done < s_dl[i].size) continue;
        s_dl[i].done = s_dl[i].size;
        s_dl[i].state = 2;
        const char *root = getenv("BIB_ROOT"), *fx = getenv("BIB_DL_FIXTURE");
        if (root && fx) {
            char to[400];
            snprintf(to, sizeof to, "%s%s", root, s_dl[i].dest);
            size_t n = 0;
            char *b = slurp(fx, &n);
            FILE *f = b ? fopen(to, "wb") : NULL;
            if (f) { fwrite(b, 1, n, f); fclose(f); }
            HOST_FREE(b);
            fprintf(stderr, "dl: done %d -> %s\n", s_dl[i].id, to);
        }
    }
}

static int dl_json(int i, char *buf, size_t cap) {
    return snprintf(buf, cap, "{\"id\":%d,\"state\":\"%s\",\"title\":\"%s\",\"dest\":\"%s\",\"size\":%lld,\"done\":%lld,"
                    "\"verified\":%lld,\"bps\":%d,\"phase\":\"%s\",\"error\":\"\"}", s_dl[i].id, kDlSt[s_dl[i].state],
                    s_dl[i].title, s_dl[i].dest, s_dl[i].size, s_dl[i].done, s_dl[i].done, s_dl[i].state == 4 ? 184320 : 0,
                    s_dl[i].state == 4 ? "fetch" : "");
}
int32_t nv_dl_info(int32_t id, char *buf, uint32_t len) {
    for (int i = 0; i < s_ndl; i++) if (s_dl[i].id == id) return dl_json(i, buf, len);
    return -2;
}
int32_t nv_dl_list(char *buf, uint32_t len) {
    size_t k = (size_t)snprintf(buf, len, "[");
    for (int i = 0; i < s_ndl && k + 600 < len; i++) {
        if (i) buf[k++] = ',';
        k += (size_t)dl_json(i, buf + k, len - k);
    }
    k += (size_t)snprintf(buf + k, len - k, "]");
    return (int32_t)k;
}
int32_t nv_dl_pause(int32_t id, int32_t on) {
    for (int i = 0; i < s_ndl; i++)
        if (s_dl[i].id == id) { s_dl[i].state = on ? 1 : 4; return 1; }
    return -2;
}
int32_t nv_dl_cancel(int32_t id) {
    for (int i = 0; i < s_ndl; i++)
        if (s_dl[i].id == id) { memmove(&s_dl[i], &s_dl[i + 1], sizeof s_dl[0] * (size_t)(s_ndl - i - 1)); s_ndl--; return 1; }
    return -2;
}
int32_t nv_dl_free(void) { return 12000; }

int32_t nv_gfx_present(void) {
    if (cur >= nfr) return 0;
    dl_tick();
    const frame_t *f = &fr[cur++];
    if (f->back) pend_back = 1;
    if (f->net) s_offline = f->net == 1;
    if (f->shot[0]) write_ppm(f->shot);
    if (f->stats) printf("frame %d: %ld blits, %ld rows\n", cur, blits, blit_rows);
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: bib_native SCRIPT OUTDIR\n"); return 2; }
    outdir = argv[2];
    if (getenv("BIB_W")) CW = atoi(getenv("BIB_W"));
    if (getenv("BIB_H")) CH = atoi(getenv("BIB_H"));
    canvas = (uint16_t *)HOST_MALLOC((size_t)CW * CH * 2);
    memset(canvas, 0, (size_t)CW * CH * 2);
    srand(1234);
    if (load_script(argv[1]) < 0) return 2;
    const clock_t t0 = clock();
    run();
    printf("native: %d/%d frames, %ld blits (%ld rows), %.2f s CPU\n", cur, nfr, blits, blit_rows,
           (double)(clock() - t0) / CLOCKS_PER_SEC);
#ifdef BIB_MEMCAP
    printf("memory: peak %.2f MiB of %.2f MiB, %ld refused\n", mem_peak / 1048576.0, mem_cap / 1048576.0, mem_fail);
#endif
    HOST_FREE(canvas);
    HOST_FREE(fr);
    return 0;
}
