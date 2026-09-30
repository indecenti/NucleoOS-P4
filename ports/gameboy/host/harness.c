// harness.c â€” headless PC harness for gb_frontend.c (built with -DNV_SIM): stub nv_* host with a
// 1024x600 RGB565 canvas, a simulated clock (one present = one 59.73 Hz frame), a simulated audio
// stream and scripted input. Runs run() for N presents and dumps the canvas as PPM.
//
//   harness <frames> <out.ppm> [script]
//   script: comma-separated "F0-F1:key", key = a b start select up down left right l,
//           or "F0-F1:t=X/Y" (finger 0 down at canvas X,Y)
// Build + run: bash ports/gameboy/build.sh test   (gcc in WSL)
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "nucleo_sdk.h"

#ifndef GB_FS_ROOT
#define GB_FS_ROOT "."
#endif

enum { W = 1024, H = 600 };
static uint16_t cv[W * H];
static long presents, limit;
static const char *out_path;
static int64_t aud_written, aud_underruns;
static int aud_rate = 48000, aud_ch = 2, aud_primed;
static int aud_open;
static long blits, blit_px;

typedef struct { long f0, f1; char key[16]; int x, y; } step_t;
static step_t steps[64];
static int nsteps;

static int32_t now_ms(void) { return (int32_t)(presents * 16742 / 1000); }

static void px(int x, int y, int c) { if ((unsigned)x < W && (unsigned)y < H) cv[y * W + x] = (uint16_t)c; }

void nv_print(const char *m) { printf("[print] %s\n", m); }
void nv_log(int32_t l, const char *m) { printf("[log%d] %s\n", (int)l, m); }
void nv_toast(int32_t k, const char *m) { printf("[toast%d] %s\n", (int)k, m); }
int32_t nv_millis(void) { return now_ms(); }
int64_t nv_time_unix(void) { return (int64_t)time(NULL); }
int32_t nv_lang(char *b, uint32_t n) {
    const char *l = getenv("GB_LANG") ? getenv("GB_LANG") : "en";
    snprintf(b, n, "%s", l);
    return (int32_t)strlen(b);
}
int32_t nv_rand(void) { return rand(); }
void nv_sleep_ms(int32_t ms) { (void)ms; }
int32_t nv_save(const char *name, const void *d, int32_t len) {
    char p[512]; snprintf(p, sizeof p, "%s/%s", GB_FS_ROOT, name);
    FILE *f = fopen(p, "wb"); if (!f) return 0;
    fwrite(d, 1, (size_t)len, f); fclose(f); return 1;
}
int32_t nv_load(const char *name, void *d, int32_t len) {
    char p[512]; snprintf(p, sizeof p, "%s/%s", GB_FS_ROOT, name);
    FILE *f = fopen(p, "rb"); if (!f) return 0;
    int32_t n = (int32_t)fread(d, 1, (size_t)len, f); fclose(f); return n;
}
int32_t nv_gfx_width(void) { return W; }
int32_t nv_gfx_height(void) { return H; }
void nv_gfx_clear(int32_t c) { for (int i = 0; i < W * H; i++) cv[i] = (uint16_t)c; }
void nv_gfx_rect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t c) {
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) px(i, j, c);
}
void nv_gfx_circle(int32_t cx, int32_t cy, int32_t r, int32_t c) {
    for (int j = -r; j <= r; j++) for (int i = -r; i <= r; i++) if (i * i + j * j <= r * r) px(cx + i, cy + j, c);
}
void nv_gfx_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t c) { (void)x0; (void)y0; (void)x1; (void)y1; (void)c; }
static long edge(long ax, long ay, long bx, long by, long px_, long py) { return (bx - ax) * (py - ay) - (by - ay) * (px_ - ax); }
void nv_gfx_tri(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t c) {
    int mnx = x0 < x1 ? x0 : x1; mnx = mnx < x2 ? mnx : x2;
    int mxx = x0 > x1 ? x0 : x1; mxx = mxx > x2 ? mxx : x2;
    int mny = y0 < y1 ? y0 : y1; mny = mny < y2 ? mny : y2;
    int mxy = y0 > y1 ? y0 : y1; mxy = mxy > y2 ? mxy : y2;
    for (int y = mny; y <= mxy; y++) for (int x = mnx; x <= mxx; x++) {
        long a = edge(x0, y0, x1, y1, x, y), b = edge(x1, y1, x2, y2, x, y), d = edge(x2, y2, x0, y0, x, y);
        if ((a >= 0 && b >= 0 && d >= 0) || (a <= 0 && b <= 0 && d <= 0)) px(x, y, c);
    }
}
void nv_gfx_blit_raw(const void *p, int32_t len, int32_t x, int32_t y, int32_t w, int32_t h) {
    if ((int64_t)w * h * 2 > len) { printf("blit: bad len\n"); exit(2); }
    const uint16_t *s = p;
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) px(x + i, y + j, s[j * w + i]);
    blits++; blit_px += (long)w * h;
}
void nv_gfx_image(const char *n, int32_t x, int32_t y, int32_t w, int32_t h) { (void)n; (void)x; (void)y; (void)w; (void)h; }
// No font on the PC: each glyph is a 5x7 block (enough to check the layout).
void nv_gfx_text(int32_t x, int32_t y, const char *s, int32_t c, int32_t sc) {
    for (; *s; s++, x += 6 * sc) if (*s != ' ') nv_gfx_rect(x, y + sc, 5 * sc, 5 * sc, c);
}
int32_t nv_gfx_text_width(const char *s, int32_t sc) { return (int32_t)strlen(s) * 6 * sc; }
void nv_gfx_terrain_raw(const void *t, int32_t l, int32_t x0, int32_t yb, int32_t a, int32_t b) { (void)t; (void)l; (void)x0; (void)yb; (void)a; (void)b; }
void nv_gfx_tone(int32_t f, int32_t ms) { (void)f; (void)ms; }
void nv_gfx_persist(int32_t on) { (void)on; }

static const step_t *active(const char *key) {
    for (int i = 0; i < nsteps; i++)
        if (presents >= steps[i].f0 && presents <= steps[i].f1 && !strcmp(steps[i].key, key)) return &steps[i];
    return NULL;
}
int32_t nv_gfx_touch_count(void) { return active("t") ? 1 : 0; }
int32_t nv_gfx_touch_point_raw(int32_t i) {
    const step_t *s = active("t");
    if (i || !s) return 0;
    return (1 << 24) | (s->y << 12) | s->x;
}
int32_t nv_gfx_input_raw(void) {
    const step_t *s = active("t");
    return s ? (1 << 24) | (s->y << 12) | s->x : 0;
}
int32_t nv_gfx_pad(void) {
    int32_t v = 0;
    if (active("a")) v |= NV_PAD_A;
    if (active("b")) v |= NV_PAD_B;
    if (active("start")) v |= NV_PAD_START;
    if (active("select")) v |= NV_PAD_SELECT;
    if (active("up")) v |= NV_PAD_UP;
    if (active("down")) v |= NV_PAD_DOWN;
    if (active("left")) v |= NV_PAD_LEFT;
    if (active("right")) v |= NV_PAD_RIGHT;
    if (active("l")) v |= NV_PAD_L;
    return v;
}
int32_t nv_gfx_back(void) { return active("back") ? 1 : 0; }
int32_t nv_pad_count(void) { return 0; }
int32_t nv_pad_state(int32_t i, nv_pad_state_t *st, int32_t len) { (void)i; (void)st; (void)len; return 0; }

static void dump(void) {
    FILE *f = fopen(out_path, "wb");
    if (!f) { perror(out_path); exit(1); }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        const uint16_t c = cv[i];
        const unsigned char rgb[3] = {(unsigned char)((c >> 11) << 3), (unsigned char)(((c >> 5) & 63) << 2),
                                      (unsigned char)((c & 31) << 3)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}
int32_t nv_gfx_present(void) {
    if (getenv("GB_TRACE") && presents % 25 == 0)
        printf("present %ld: %lld ms of audio written\n", presents, (long long)(aud_written * 1000 / (aud_rate * 4)));
    presents++;
    return presents < limit;
}

// audio: consumed in real (simulated) time at the opened rate
// Model of the OS sink (components/nv_hal/nv_audio.cpp): a MUSIC stream has a pre-roll gate — the
// feeder drains nothing until 0.2 s is queued — then plays in real time; underruns play silence.
// GB_AUDIO_STUCK=1: the sink never drains (dead codec) — the emulator must keep running anyway.
static int64_t aud_consumed(void) { return (int64_t)now_ms() * aud_rate * aud_ch * 2 / 1000; }
static int64_t aud_base;
int32_t nv_audio_open(int32_t rate, int32_t ch) { aud_open = 1; aud_rate = rate; aud_ch = ch; aud_primed = 0; return 1; }
int32_t nv_audio_backlog(void) {
    if (getenv("GB_AUDIO_STUCK")) return (int32_t)aud_written;
    if (!aud_primed) {
        if (aud_written < (int64_t)aud_rate * aud_ch * 2 / 5) return (int32_t)aud_written;
        aud_primed = 1; aud_base = aud_consumed();
    }
    int64_t q = aud_written - (aud_consumed() - aud_base);
    if (q < 0) { aud_underruns++; aud_base -= q; q = 0; }   // the device plays silence meanwhile
    return (int32_t)q;
}
int32_t nv_audio_write(const void *p, int32_t n) {
    static int64_t peak;
    const int16_t *s = p;
    for (int i = 0; i < n / 2; i++) { int v = s[i] < 0 ? -s[i] : s[i]; if (v > peak) peak = v; }
    aud_written += n;
    if (presents == limit - 1) printf("audio peak %lld / 32767\n", (long long)peak);
    return n;
}
void nv_audio_close(void) { aud_open = 0; }

// Split so host/gbhost.c (the same stubs behind WAMR, running the real app.wasm/.aot) can reuse it.
static clock_t hs_t0;
static int hs_setup(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: harness <frames> <out.ppm> [script]\n"); return 1; }
    limit = atol(argv[1]);
    out_path = argv[2];
    if (argc > 3) {
        char *s = strdup(argv[3]), *tok, *save = NULL;
        for (tok = strtok_r(s, ",", &save); tok && nsteps < 64; tok = strtok_r(NULL, ",", &save)) {
            step_t *st = &steps[nsteps];
            char key[32] = "";
            if (sscanf(tok, "%ld-%ld:%31s", &st->f0, &st->f1, key) != 3) continue;
            if (!strncmp(key, "t=", 2)) { strcpy(st->key, "t"); sscanf(key + 2, "%d/%d", &st->x, &st->y); }
            else snprintf(st->key, sizeof st->key, "%s", key);
            nsteps++;
        }
    }
    hs_t0 = clock();
    return 0;
}
static void hs_finish(void) {
    const double sec = (double)(clock() - hs_t0) / CLOCKS_PER_SEC;
    dump();
    printf("presents %ld, blits %ld (%.1f Kpx avg), audio %lld bytes = %lld ms, underruns %lld, cpu %.2f s (%.0f us/present)\n",
           presents, blits, blits ? blit_px / 1000.0 / blits : 0.0, (long long)aud_written,
           (long long)(aud_written * 1000 / (aud_rate * 4)), (long long)aud_underruns, sec, presents ? sec * 1e6 / presents : 0.0);
}

#ifndef GB_WAMR_HOST
void run(void);
int main(int argc, char **argv) {
    if (hs_setup(argc, argv)) return 1;
    run();
    hs_finish();
    return 0;
}
#endif
