// harness.c — headless PC harness for the Arduboy shim (built with -DNV_SIM): stub nv_* host with a
// 1024x600 RGB565 canvas, a simulated clock (a present = 16 ms, nv_sleep_ms advances it), a
// simulated audio sink (0.2 s pre-roll, then real time) and scripted input. Runs the sketch for N
// presents and dumps the canvas (PPM) plus, for chosen presents, the 128x64 game frame (PGM).
//
//   harness <presents> <out.ppm> [script]
//   script: comma-separated "F0-F1:key", key = a b up down left right start select l back,
//           or "F0-F1:t=X/Y" (finger 0 down at canvas X,Y)
//   env AB_FS=<dir>        nv_save/nv_load folder (default .)
//   env AB_SNAP=<F,F,...>  also write <out>_<F>.pgm (game frame) and <out>_<F>.ppm (canvas)
//   env AB_LANG=it         UI language
// Build + run: bash ports/arduboy/build.sh (gcc in WSL). Part of ports/arduboy.
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "nucleo_sdk.h"
#include "font5x7.h"

enum { W = 1024, H = 600 };
static uint16_t cv[W * H];
static long presents, limit;
static double sim_ms;
static const char *out_path;
static int64_t aud_written, aud_underruns, aud_nonzero;
static int aud_rate = 48000, aud_ch = 2, aud_primed, aud_open, aud_opens;
static long blits, blit_px, sleeps;
static int16_t aud_peak;
static uint8_t frame[1024];
static int have_frame;
static long snaps[32];
static int nsnaps;

typedef struct { long f0, f1; char key[16]; int x, y; } step_t;
static step_t steps[128];
static int nsteps;

static const char *fs_root(void) { return getenv("AB_FS") ? getenv("AB_FS") : "."; }
static void px(int x, int y, int c) { if ((unsigned)x < W && (unsigned)y < H) cv[y * W + x] = (uint16_t)c; }

void nv_print(const char *m) { printf("[print] %s\n", m); }
void nv_log(int32_t l, const char *m) { printf("[log%d] %s\n", (int)l, m); }
int32_t nv_millis(void) { sim_ms += 0.02; return (int32_t)sim_ms; }   // busy-wait loops advance too
int64_t nv_time_unix(void) { return (int64_t)time(NULL); }
int32_t nv_lang(char *b, uint32_t n) { snprintf(b, n, "%s", getenv("AB_LANG") ? getenv("AB_LANG") : "en"); return (int32_t)strlen(b); }
int32_t nv_rand(void) { return rand(); }
void nv_sleep_ms(int32_t ms) { if (ms > 0) sim_ms += ms; sleeps++; }
int32_t nv_save(const char *name, const void *d, int32_t len) {
    char p[512]; snprintf(p, sizeof p, "%s/%s", fs_root(), name);
    FILE *f = fopen(p, "wb"); if (!f) return 0;
    fwrite(d, 1, (size_t)len, f); fclose(f); return 1;
}
int32_t nv_load(const char *name, void *d, int32_t len) {
    char p[512]; snprintf(p, sizeof p, "%s/%s", fs_root(), name);
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
    if (x < 0 || y < 0 || x + w > W || y + h > H) { printf("blit: out of canvas %d,%d %dx%d\n", x, y, w, h); exit(2); }
    const uint16_t *s = p;
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) px(x + i, y + j, s[j * w + i]);
    blits++; blit_px += (long)w * h;
}
void nv_gfx_text(int32_t x, int32_t y, const char *s, int32_t c, int32_t sc) {
    for (; *s; s++, x += 6 * sc) {
        const uint8_t *gl = FONT5x7[font_glyph(*s)];
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 5; col++)
                if (gl[row] & (1 << (4 - col))) nv_gfx_rect(x + col * sc, y + row * sc, sc, sc, c);
    }
}
int32_t nv_gfx_text_width(const char *s, int32_t sc) { return (int32_t)strlen(s) * 6 * sc; }
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
int32_t nv_gfx_input_raw(void) { const step_t *s = active("t"); return s ? (1 << 24) | (s->y << 12) | s->x : 0; }
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

static void write_ppm(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        const uint16_t c = cv[i];
        const unsigned char rgb[3] = {(unsigned char)((c >> 11) << 3), (unsigned char)(((c >> 5) & 63) << 2),
                                      (unsigned char)((c & 31) << 3)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}
static void write_pgm(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P5\n128 64\n255\n");
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 128; x++) fputc((frame[(y / 8) * 128 + x] >> (y & 7)) & 1 ? 255 : 0, f);
    fclose(f);
}
void nv_sim_frame(const uint8_t *buf) { memcpy(frame, buf, sizeof frame); have_frame = 1; }

int32_t nv_gfx_present(void) {
    for (int i = 0; i < nsnaps; i++)
        if (snaps[i] == presents) {
            char p[600];
            snprintf(p, sizeof p, "%.*s_%ld.ppm", (int)(strlen(out_path) - 4), out_path, presents);
            write_ppm(p);
            if (have_frame) { snprintf(p, sizeof p, "%.*s_%ld.pgm", (int)(strlen(out_path) - 4), out_path, presents); write_pgm(p); }
        }
    presents++;
    sim_ms += 16;
    return presents < limit;
}

// audio sink model: pre-roll gate (0.2 s queued), then real time; underruns play silence
static int64_t aud_consumed(void) { return (int64_t)sim_ms * aud_rate * aud_ch * 2 / 1000; }
static int64_t aud_base;
int32_t nv_audio_open(int32_t rate, int32_t ch) { aud_open = 1; aud_opens++; aud_rate = rate; aud_ch = ch; aud_primed = 0; aud_written = 0; return 1; }
int32_t nv_audio_backlog(void) {
    if (!aud_primed) {
        if (aud_written < (int64_t)aud_rate * aud_ch * 2 / 5) return (int32_t)aud_written;
        aud_primed = 1; aud_base = aud_consumed();
    }
    int64_t q = aud_written - (aud_consumed() - aud_base);
    if (q < 0) { aud_underruns++; aud_base -= q; q = 0; }
    return (int32_t)q;
}
int32_t nv_audio_write(const void *p, int32_t n) {
    const int16_t *s = p;
    for (int i = 0; i < n / 2; i++) {
        int v = s[i] < 0 ? -s[i] : s[i];
        if (v > aud_peak) aud_peak = (int16_t)v;
        if (v > 64) aud_nonzero++;
    }
    aud_written += n;
    return n;
}
void nv_audio_close(void) { aud_open = 0; }

static clock_t hs_t0;
static void hs_finish(void) {
    static int done;
    if (done) return;
    done = 1;
    const double sec = (double)(clock() - hs_t0) / CLOCKS_PER_SEC;
    write_ppm(out_path);
    printf("presents %ld, sim %.1f s, blits %ld (%.1f Kpx avg), sleeps %ld, audio opens %d, %lld ms written, "
           "peak %d, non-silent %lld ms, underruns %lld, cpu %.2f s\n",
           presents, sim_ms / 1000.0, blits, blits ? blit_px / 1000.0 / blits : 0.0, sleeps, aud_opens,
           (long long)(aud_written * 1000 / (aud_rate * 4)), aud_peak, (long long)(aud_nonzero * 1000 / (aud_rate * 2)),
           (long long)aud_underruns, sec);
    fflush(stdout);
}
static void on_alarm(int sig) {
    (void)sig;
    printf("HUNG: no progress (presents %ld, sim %.1f s)\n", presents, sim_ms / 1000.0);
    hs_finish();
    _exit(3);
}
static int hs_setup(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: harness <presents> <out.ppm> [script]\n"); return 1; }
    limit = atol(argv[1]);
    out_path = argv[2];
    if (argc > 3) {
        char *s = strdup(argv[3]), *tok, *save = NULL;
        for (tok = strtok_r(s, ",", &save); tok && nsteps < 128; tok = strtok_r(NULL, ",", &save)) {
            step_t *st = &steps[nsteps];
            char key[32] = "";
            if (sscanf(tok, "%ld-%ld:%31s", &st->f0, &st->f1, key) != 3) continue;
            if (!strncmp(key, "t=", 2)) { strcpy(st->key, "t"); sscanf(key + 2, "%d/%d", &st->x, &st->y); }
            else snprintf(st->key, sizeof st->key, "%s", key);
            nsteps++;
        }
    }
    if (getenv("AB_SNAP")) {
        char *s = strdup(getenv("AB_SNAP")), *tok, *save = NULL;
        for (tok = strtok_r(s, ",", &save); tok && nsnaps < 32; tok = strtok_r(NULL, ",", &save)) snaps[nsnaps++] = atol(tok);
    }
    hs_t0 = clock();
    atexit(hs_finish);
    signal(SIGALRM, on_alarm);
    alarm(60);
    return 0;
}

#ifndef AB_WAMR_HOST
void run(void);
int main(int argc, char **argv) {
    if (hs_setup(argc, argv)) return 1;
    run();
    return 0;
}
#endif
