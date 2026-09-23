// bench — CPU micro-benchmark for the WASM engine: the same kernels run as app.wasm (interpreter)
// and app.aot (native), so the timings compare the two and the checksums prove identical results.
// Build:  .\sdk\build_app.ps1 -AppDir apps\bench [-Aot]
// Run:    GET /api/app/run?id=bench  (prints one line per kernel + the total)
#include "nucleo_sdk.h"

static uint8_t g_buf[16 * 1024];   // apps link with 64 KB of linear memory

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

typedef uint32_t (*kernel_fn)(void);

NV_EXPORT("run")
void run(void) {
    static const struct { const char *name; kernel_fn fn; } K[] = {
        {"sieve", k_sieve}, {"crc32", k_crc}, {"mandel", k_mandel}, {"fib", k_fib},
    };
    int32_t total = 0;
    for (unsigned i = 0; i < sizeof K / sizeof K[0]; i++) {
        const int32_t t0 = nv_millis();
        const uint32_t v = K[i].fn();
        const int32_t ms = nv_millis() - t0;
        total += ms;
        nv_printf("%s: %d ms (check %u)", K[i].name, ms, v);
    }
    nv_printf("total: %d ms", total);
}
