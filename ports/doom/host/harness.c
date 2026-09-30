// harness.c — headless PC harness for the Doom front-end (built with -DNV_SIM, gcc in WSL).
// Stubs the nv_* host: a 320x240 RGB565 canvas, a simulated clock that only advances when the
// game sleeps (so the run is deterministic and CPU time is measured apart), a simulated audio
// stream consumed at its rate, and a scripted pad. Boots the launcher, starts the IWAD, lets the
// demo loop play and dumps frames as PPM; prints the CPU cost per rendered frame and of the mixer.
//
//   harness <fs-root> <iwad> <presents>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "nucleo_sdk.h"

enum { W = 320, H = 240 };
static uint16_t cv[W * H];
static long presents, limit;
static int64_t sim_us;                 // simulated clock
static int64_t aud_written, aud_underruns, aud_peak;
static int aud_open, aud_rate = 22050, aud_ch = 2;
static int64_t aud_t0;
static long blits;
static double cpu_mix;

static double cpu_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

void nv_print(const char *m) { printf("[print] %s\n", m); }
void nv_log(int32_t l, const char *m) { printf("[log%d] %s\n", (int)l, m); }
void nv_toast(int32_t k, const char *m) { printf("[toast%d] %s\n", (int)k, m); }
int32_t nv_millis(void) { return (int32_t)(sim_us / 1000); }
void nv_sleep_ms(int32_t ms) { sim_us += (int64_t)ms * 1000; }
int32_t nv_lang(char *b, uint32_t n) { snprintf(b, n, "%s", getenv("DOOM_LANG") ? getenv("DOOM_LANG") : "en"); return (int32_t)strlen(b); }
int32_t nv_save(const char *name, const void *d, int32_t len) {
    FILE *f = fopen(name, "wb"); if (!f) return 0;
    fwrite(d, 1, (size_t)len, f); fclose(f); return 1;
}
int32_t nv_load(const char *name, void *d, int32_t len) {
    FILE *f = fopen(name, "rb"); if (!f) return 0;
    int32_t n = (int32_t)fread(d, 1, (size_t)len, f); fclose(f); return n;
}
static void px(int x, int y, int c) { if ((unsigned)x < W && (unsigned)y < H) cv[y * W + x] = (uint16_t)c; }
void nv_gfx_clear(int32_t c) { for (int i = 0; i < W * H; i++) cv[i] = (uint16_t)c; }
void nv_gfx_rect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t c) {
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) px(i, j, c);
}
int32_t nv_gfx_text_width(const char *s, int32_t sc) { return (int32_t)strlen(s) * 6 * sc; }
void nv_gfx_text(int32_t x, int32_t y, const char *s, int32_t c, int32_t sc) {   // boxes, no glyphs
    for (; *s; s++, x += 6 * sc) if (*s != ' ') nv_gfx_rect(x, y, 5 * sc, 7 * sc, c);
}
void nv_gfx_blit_raw(const void *p, int32_t len, int32_t x, int32_t y, int32_t w, int32_t h) {
    if ((int64_t)w * h * 2 > len || x || y || w != W || h != H) { printf("blit: bad args\n"); exit(2); }
    memcpy(cv, p, (size_t)w * h * 2);
    blits++;
}
static void dump(const char *path) {
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        const uint16_t c = cv[i];
        const uint8_t rgb[3] = {(uint8_t)((c >> 11) << 3), (uint8_t)(((c >> 5) & 63) << 2), (uint8_t)((c & 31) << 3)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}
static double cpu_start;
int32_t nv_gfx_present(void) {
    presents++;
    sim_us += 1000;   // a present costs at least a vsync slice
    static const long shots[] = {5, 60, 400, 900, 1500, 2400};
    for (unsigned i = 0; i < sizeof shots / sizeof *shots; i++)
        if (presents == shots[i]) { char p[32]; snprintf(p, sizeof p, "frame%04ld.ppm", presents); dump(p); }
    if (presents >= limit) {
        const double sec = cpu_now() - cpu_start;
        printf("presents %ld, blits %ld, sim %.1f s, cpu %.2f s (%.0f us/present, mixer %.0f us/present)\n",
               presents, blits, sim_us / 1e6, sec, sec * 1e6 / presents, cpu_mix * 1e6 / presents);
        printf("audio %lld bytes (%.1f s), underruns %lld, peak %lld / 32767\n", (long long)aud_written,
               aud_written / (double)(aud_rate * 2 * aud_ch), (long long)aud_underruns, (long long)aud_peak);
        exit(0);
    }
    return 1;
}
int32_t nv_gfx_back(void) { return 0; }
int32_t nv_gfx_touch_count(void) { return 0; }
int32_t nv_gfx_touch_point_raw(int32_t i) { (void)i; return 0; }
int32_t nv_gfx_input_raw(void) { return 0; }
int32_t nv_gfx_width(void) { return W; }
int32_t nv_gfx_height(void) { return H; }
int32_t nv_gfx_pad(void) {   // A pressed for a moment to leave the launcher
    return (presents >= 3 && presents <= 5) ? NV_PAD_A : 0;
}
int32_t nv_pad_count(void) { return 0; }
int32_t nv_pad_state(int32_t i, nv_pad_state_t *st, int32_t len) { (void)i; (void)st; (void)len; return 0; }
int32_t nv_pad_rumble(int32_t i, int32_t a, int32_t b, int32_t ms) { (void)i; (void)a; (void)b; (void)ms; return 0; }
int32_t nv_kbd_state(uint8_t *buf, int32_t len) { (void)buf; (void)len; return -1; }
int32_t nv_mouse_read(nv_mouse_t *m, int32_t len) { (void)m; (void)len; return 0; }
int32_t nv_http_req(const char *spec, const void *b, uint32_t l) { (void)spec; (void)b; (void)l; return NV_NET_E_PERM; }
int32_t nv_http_state(int32_t h) { (void)h; return -1; }
int32_t nv_http_status(int32_t h) { (void)h; return 0; }
int32_t nv_http_read(int32_t h, void *b, uint32_t l) { (void)h; (void)b; (void)l; return 0; }
void nv_http_close(int32_t h) { (void)h; }

// audio: consumed in simulated time at the opened rate
static int64_t aud_consumed(void) { return (sim_us - aud_t0) * aud_rate / 1000000 * 2 * aud_ch; }
int32_t nv_audio_open(int32_t rate, int32_t ch) { aud_open = 1; aud_rate = rate; aud_ch = ch; aud_t0 = sim_us; return 1; }
int32_t nv_audio_backlog(void) {
    int64_t b = aud_written - aud_consumed();
    if (b < 0) { aud_underruns++; aud_written = aud_consumed(); b = 0; }
    return (int32_t)b;
}
int32_t nv_audio_write(const void *p, int32_t n) {
    const int16_t *s = p;
    for (int i = 0; i < n / 2; i++) { int v = s[i] < 0 ? -s[i] : s[i]; if (v > aud_peak) aud_peak = v; }
    static FILE *wav;
    if (!wav && getenv("DOOM_PCM")) wav = fopen(getenv("DOOM_PCM"), "wb");
    if (wav) fwrite(p, 1, (size_t)n, wav);
    aud_written += n;
    return n;
}
void nv_audio_close(void) { aud_open = 0; }
void nv_sim_mix_time(double s) { cpu_mix += s; }

void run(void);
int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "harness <fs-root> <iwad> <presents>\n"); return 1; }
    if (chdir(argv[1]) != 0) { perror("chdir"); return 1; }
    limit = atol(argv[3]);
    cpu_start = cpu_now();
    run();
    printf("run() returned after %ld presents\n", presents);
    return 0;
}
