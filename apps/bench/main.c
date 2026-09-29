// bench — CPU Bench: a visual micro-benchmark for the WASM engine (ABI 4, full-screen gfx). Five
// kernels stress different things (integers+memory, bit ops, float math, recursion, memory
// bandwidth); results are drawn as a bar-per-kernel comparison plus a total, and the best total
// ever seen is persisted (nv_save/nv_load) so a re-run after an OTA shows whether the OS got
// faster or slower.
#include "nucleo_sdk.h"

#define BG      NV_RGB(18, 20, 28)
#define PANEL   NV_RGB(34, 38, 52)
#define TRACK   NV_RGB(48, 52, 68)
#define ACCENT  NV_RGB(84, 162, 255)
#define GREEN   NV_RGB(56, 200, 120)
#define RED     NV_RGB(232, 84, 92)
#define INK     NV_RGB(240, 242, 248)
#define DIMINK  NV_RGB(150, 156, 172)
#define WHITE   NV_RGB(255, 255, 255)

// ---------------------------------------------------------------- kernels

static uint8_t g_buf[16 * 1024];
static uint8_t g_buf2[16 * 1024];

// Integers + memory: count the primes below N with a byte sieve.
static uint32_t k_sieve(void) {
    enum { N = 20000 };
    static uint8_t comp[N];
    uint32_t count = 0;
    for (int r = 0; r < 4; r++) {
        for (int i = 0; i < N; i++) comp[i] = 0;
        count = 0;
        for (int i = 2; i < N; i++) {
            if (comp[i]) continue;
            count++;
            for (int j = i * 2; j < N; j += i) comp[j] = 1;
        }
    }
    return count;
}

// Bit twiddling: bitwise CRC-32 over a pseudo-random buffer.
static uint32_t k_crc(void) {
    uint32_t seed = 12345;
    for (uint32_t i = 0; i < sizeof g_buf; i++) {
        seed = seed * 1103515245u + 12345u;
        g_buf[i] = (uint8_t)(seed >> 16);
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (int r = 0; r < 4; r++)
        for (uint32_t i = 0; i < sizeof g_buf; i++) {
            crc ^= g_buf[i];
            for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    return ~crc;
}

// Single-precision float: a small Mandelbrot, summing the iteration counts.
static uint32_t k_mandel(void) {
    enum { W = 96, H = 64, IT = 48 };
    uint32_t sum = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const float cr = -2.0f + 2.6f * (float)x / W, ci = -1.2f + 2.4f * (float)y / H;
            float zr = 0.0f, zi = 0.0f;
            int i = 0;
            while (i < IT && zr * zr + zi * zi < 4.0f) {
                const float t = zr * zr - zi * zi + cr;
                zi = 2.0f * zr * zi + ci;
                zr = t;
                i++;
            }
            sum += (uint32_t)i;
        }
    return sum;
}

// Function calls + recursion.
static uint32_t fib(uint32_t n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
static uint32_t k_fib(void) { return fib(20); }

// Memory bandwidth: repeated memcpy over a 16 KB block.
static uint32_t k_memcpy(void) {
    for (int r = 0; r < 3000; r++) memcpy(g_buf2, g_buf, sizeof g_buf);
    uint32_t sum = 0;
    for (uint32_t i = 0; i < sizeof g_buf2; i += 64) sum += g_buf2[i];
    return sum;
}

typedef uint32_t (*kernel_fn)(void);
typedef struct { const char *name; kernel_fn fn; } Kernel;
static const Kernel K[] = {
    {"SIEVE", k_sieve}, {"CRC32", k_crc}, {"MANDEL", k_mandel}, {"FIB", k_fib}, {"MEMCPY", k_memcpy},
};
#define NK ((int)(sizeof K / sizeof K[0]))

// ---------------------------------------------------------------- state

enum { ST_IDLE, ST_RUNNING, ST_DONE };
static int      W, H, state = ST_IDLE;
static int      cur = 0;               // next kernel to run, while ST_RUNNING
static int      ms[NK];                // per-kernel time, filled as each completes
static uint32_t chk[NK];
static int      total_ms = 0;
static int      best_ms = -1;          // -1 = no saved record yet
static int      new_best = 0;
static int      redraw = 2;

typedef struct { int x, y, w, h; } Rect;
static Rect b_run;

static int hit(Rect r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static void itoa10(int v, char *out) {
    char tmp[12];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0) { tmp[n++] = '0' + (v % 10); v /= 10; }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
}

static void btn(Rect r, const char *label, int fill, int ink, int scale) {
    nv_gfx_rect(r.x, r.y, r.w, r.h, fill);
    int tw = nv_gfx_text_width(label, scale);
    nv_gfx_text(r.x + (r.w - tw) / 2, r.y + (r.h - 7 * scale) / 2, label, ink, scale);
}

static void load_best(void) {
    int v;
    if (nv_load("best.bin", &v, sizeof v) == (int)sizeof v && v > 0) best_ms = v;
}
static void save_best(int v) { nv_save("best.bin", &v, sizeof v); }

static void start_run(void) {
    state = ST_RUNNING;
    cur = 0;
    total_ms = 0;
    new_best = 0;
    for (int i = 0; i < NK; i++) { ms[i] = -1; chk[i] = 0; }
    redraw = 2;
}

// One kernel per call, so the UI can redraw between them and stay responsive even if a kernel
// takes a while — each call runs the next pending kernel and returns.
static void step_run(void) {
    if (cur >= NK) {
        state = ST_DONE;
        if (best_ms < 0 || total_ms < best_ms) { new_best = 1; best_ms = total_ms; save_best(best_ms); }
        return;
    }
    const int t0 = nv_millis();
    chk[cur] = K[cur].fn();
    ms[cur] = nv_millis() - t0;
    total_ms += ms[cur];
    cur++;
}

static void draw_idle(void) {
    nv_gfx_text_center(220, "Five kernels: integers, bit ops,", DIMINK, 2);
    nv_gfx_text_center(252, "float math, recursion, memory bandwidth.", DIMINK, 2);
    btn(b_run, "RUN BENCHMARK", ACCENT, WHITE, 3);
    if (best_ms >= 0) {
        char s[32], v[12];
        itoa10(best_ms, v);
        int n = 0;
        for (const char *p = "BEST: "; *p; p++) s[n++] = *p;
        for (char *p = v; *p; p++) s[n++] = *p;
        for (const char *p = " ms"; *p; p++) s[n++] = *p;
        s[n] = 0;
        nv_gfx_text_center(430, s, DIMINK, 2);
    } else {
        nv_gfx_text_center(430, "no run recorded yet", DIMINK, 2);
    }
}

static void draw_results(void) {
    int max_ms = 1;
    for (int i = 0; i < NK; i++) if (ms[i] > max_ms) max_ms = ms[i];

    const int row_y0 = 110, row_h = 56;
    const int name_x = 60, track_x = 260, track_w = 560, track_h = 30, val_x = 850;
    for (int i = 0; i < NK; i++) {
        int y = row_y0 + i * row_h;
        int running_now = (state == ST_RUNNING && i == cur);
        int done = (state == ST_DONE) || (state == ST_RUNNING && i < cur);

        nv_gfx_text(name_x, y + 4, K[i].name, running_now ? ACCENT : INK, 2);
        nv_gfx_rect(track_x, y, track_w, track_h, TRACK);
        if (done) {
            int w = (int)((int64_t)ms[i] * (track_w - 4) / max_ms);
            if (w < 2) w = 2;
            nv_gfx_rect(track_x + 2, y + 2, w, track_h - 4, ACCENT);
            char v[16];
            itoa10(ms[i], v);
            int n = 0; char s[24];
            for (char *p = v; *p; p++) s[n++] = *p;
            s[n++] = 'm'; s[n++] = 's'; s[n] = 0;
            nv_gfx_text(val_x, y + 4, s, INK, 2);
        } else if (running_now) {
            nv_gfx_text(val_x, y + 4, "...", ACCENT, 2);
        }
    }

    int total_y = row_y0 + NK * row_h + 14;
    nv_gfx_rect(name_x, total_y, track_x + track_w - name_x, 2, PANEL);
    nv_gfx_text(name_x, total_y + 16, "TOTAL", INK, 3);
    if (state == ST_DONE) {
        char v[16]; itoa10(total_ms, v);
        int n = 0; char s[24];
        for (char *p = v; *p; p++) s[n++] = *p;
        s[n++] = ' '; s[n++] = 'm'; s[n++] = 's'; s[n] = 0;
        int tw = nv_gfx_text_width(s, 3);
        nv_gfx_text(val_x + 40 - tw, total_y + 16, s, new_best ? GREEN : INK, 3);
    }

    if (state == ST_DONE) {
        if (new_best) nv_gfx_text_center(total_y + 70, "NEW BEST!", GREEN, 3);
        else {
            char v[16]; itoa10(best_ms, v);
            int n = 0; char s[32];
            for (const char *p = "best: "; *p; p++) s[n++] = *p;
            for (char *p = v; *p; p++) s[n++] = *p;
            for (const char *p = " ms"; *p; p++) s[n++] = *p;
            s[n] = 0;
            nv_gfx_text_center(total_y + 70, s, DIMINK, 2);
        }
        btn(b_run, "RUN AGAIN", PANEL, INK, 3);
    }
}

static void draw(void) {
    nv_gfx_clear(BG);
    nv_gfx_text_center(50, "CPU BENCH", ACCENT, 4);
    if (state == ST_IDLE) draw_idle();
    else draw_results();
}

NV_EXPORT("run")
void run(void) {
    W = nv_gfx_width();
    H = nv_gfx_height();
    (void)H;
    b_run = (Rect){ W / 2 - 200, 500, 400, 76 };
    load_best();

    int prev_down = 0;
    while (nv_gfx_present()) {
        if (nv_gfx_back()) break;

        int x, y, down = nv_touch(&x, &y);
        int tap = (!down && prev_down);
        prev_down = down;
        if (tap && (state == ST_IDLE || state == ST_DONE) && hit(b_run, x, y)) start_run();

        if (state == ST_RUNNING) { step_run(); redraw = 2; }

        if (redraw > 0) { draw(); redraw--; }
    }
}
